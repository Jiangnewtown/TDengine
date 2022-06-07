/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "syncAppendEntriesReply.h"
#include "syncCommit.h"
#include "syncIndexMgr.h"
#include "syncInt.h"
#include "syncRaftLog.h"
#include "syncRaftStore.h"
#include "syncSnapshot.h"
#include "syncUtil.h"
#include "syncVoteMgr.h"

// TLA+ Spec
// HandleAppendEntriesResponse(i, j, m) ==
//    /\ m.mterm = currentTerm[i]
//    /\ \/ /\ m.msuccess \* successful
//          /\ nextIndex'  = [nextIndex  EXCEPT ![i][j] = m.mmatchIndex + 1]
//          /\ matchIndex' = [matchIndex EXCEPT ![i][j] = m.mmatchIndex]
//       \/ /\ \lnot m.msuccess \* not successful
//          /\ nextIndex' = [nextIndex EXCEPT ![i][j] =
//                               Max({nextIndex[i][j] - 1, 1})]
//          /\ UNCHANGED <<matchIndex>>
//    /\ Discard(m)
//    /\ UNCHANGED <<serverVars, candidateVars, logVars, elections>>
//
int32_t syncNodeOnAppendEntriesReplyCb(SSyncNode* ths, SyncAppendEntriesReply* pMsg) {
  int32_t ret = 0;

  char logBuf[128] = {0};
  snprintf(logBuf, sizeof(logBuf), "==syncNodeOnAppendEntriesReplyCb== term:%lu", ths->pRaftStore->currentTerm);
  syncAppendEntriesReplyLog2(logBuf, pMsg);

  if (pMsg->term < ths->pRaftStore->currentTerm) {
    sTrace("DropStaleResponse, receive term:%" PRIu64 ", current term:%" PRIu64 "", pMsg->term,
           ths->pRaftStore->currentTerm);
    return ret;
  }

  syncIndexMgrLog2("==syncNodeOnAppendEntriesReplyCb== before pNextIndex", ths->pNextIndex);
  syncIndexMgrLog2("==syncNodeOnAppendEntriesReplyCb== before pMatchIndex", ths->pMatchIndex);

  // no need this code, because if I receive reply.term, then I must have sent for that term.
  //  if (pMsg->term > ths->pRaftStore->currentTerm) {
  //    syncNodeUpdateTerm(ths, pMsg->term);
  //  }

  if (pMsg->term > ths->pRaftStore->currentTerm) {
    char logBuf[128] = {0};
    snprintf(logBuf, sizeof(logBuf), "syncNodeOnAppendEntriesReplyCb error term, receive:%lu current:%lu", pMsg->term,
             ths->pRaftStore->currentTerm);
    syncNodeLog2(logBuf, ths);
    sError("%s", logBuf);
    return ret;
  }

  assert(pMsg->term == ths->pRaftStore->currentTerm);

  if (pMsg->success) {
    // nextIndex'  = [nextIndex  EXCEPT ![i][j] = m.mmatchIndex + 1]
    syncIndexMgrSetIndex(ths->pNextIndex, &(pMsg->srcId), pMsg->matchIndex + 1);

    // matchIndex' = [matchIndex EXCEPT ![i][j] = m.mmatchIndex]
    syncIndexMgrSetIndex(ths->pMatchIndex, &(pMsg->srcId), pMsg->matchIndex);

    // maybe commit
    syncMaybeAdvanceCommitIndex(ths);

  } else {
    SyncIndex nextIndex = syncIndexMgrGetIndex(ths->pNextIndex, &(pMsg->srcId));

    // notice! int64, uint64
    if (nextIndex > SYNC_INDEX_BEGIN) {
      --nextIndex;
    } else {
      nextIndex = SYNC_INDEX_BEGIN;
    }
    syncIndexMgrSetIndex(ths->pNextIndex, &(pMsg->srcId), nextIndex);
  }

  syncIndexMgrLog2("==syncNodeOnAppendEntriesReplyCb== after pNextIndex", ths->pNextIndex);
  syncIndexMgrLog2("==syncNodeOnAppendEntriesReplyCb== after pMatchIndex", ths->pMatchIndex);

  return ret;
}

int32_t syncNodeOnAppendEntriesReplySnapshotCb(SSyncNode* ths, SyncAppendEntriesReply* pMsg) {
  int32_t ret = 0;

  // print log
  char logBuf[128] = {0};
  snprintf(logBuf, sizeof(logBuf), "recv SyncAppendEntriesReply, term:%lu", ths->pRaftStore->currentTerm);
  syncAppendEntriesReplyLog2(logBuf, pMsg);

  // if already drop replica, do not process
  if (!syncNodeInRaftGroup(ths, &(pMsg->srcId))) {
    sInfo("maybe already dropped");
    return ret;
  }

  // drop stale response
  if (pMsg->term < ths->pRaftStore->currentTerm) {
    sTrace("recv SyncAppendEntriesReply, drop stale response, receive_term:%lu current_term:%lu", pMsg->term,
           ths->pRaftStore->currentTerm);
    return ret;
  }

  syncIndexMgrLog2("recv SyncAppendEntriesReply, before pNextIndex:", ths->pNextIndex);
  syncIndexMgrLog2("recv SyncAppendEntriesReply, before pMatchIndex:", ths->pMatchIndex);
  {
    SSnapshot snapshot;
    ths->pFsm->FpGetSnapshot(ths->pFsm, &snapshot);
    sTrace("recv SyncAppendEntriesReply, before snapshot.lastApplyIndex:%ld, snapshot.lastApplyTerm:%lu",
           snapshot.lastApplyIndex, snapshot.lastApplyTerm);
  }

  // no need this code, because if I receive reply.term, then I must have sent for that term.
  //  if (pMsg->term > ths->pRaftStore->currentTerm) {
  //    syncNodeUpdateTerm(ths, pMsg->term);
  //  }

  if (pMsg->term > ths->pRaftStore->currentTerm) {
    char logBuf[128] = {0};
    snprintf(logBuf, sizeof(logBuf), "recv SyncAppendEntriesReply, error term, receive_term:%lu current_term:%lu",
             pMsg->term, ths->pRaftStore->currentTerm);
    syncNodeLog2(logBuf, ths);
    sError("%s", logBuf);
    return ret;
  }

  ASSERT(pMsg->term == ths->pRaftStore->currentTerm);

  if (pMsg->success) {
    // nextIndex'  = [nextIndex  EXCEPT ![i][j] = m.mmatchIndex + 1]
    syncIndexMgrSetIndex(ths->pNextIndex, &(pMsg->srcId), pMsg->matchIndex + 1);
    sTrace("update next index:%ld, success:%d", pMsg->matchIndex + 1, pMsg->success);

    // matchIndex' = [matchIndex EXCEPT ![i][j] = m.mmatchIndex]
    syncIndexMgrSetIndex(ths->pMatchIndex, &(pMsg->srcId), pMsg->matchIndex);

    // maybe commit
    if (ths->state == TAOS_SYNC_STATE_LEADER) {
      syncMaybeAdvanceCommitIndex(ths);
    }

  } else {
    SyncIndex nextIndex = syncIndexMgrGetIndex(ths->pNextIndex, &(pMsg->srcId));
    sTrace("begin to update next index:%ld, success:%d", nextIndex, pMsg->success);

    // notice! int64, uint64
    if (nextIndex > SYNC_INDEX_BEGIN) {
      --nextIndex;

      // has snapshot
      if (syncNodeHasSnapshot(ths)) {
        // get sender
        SSyncSnapshotSender* pSender = NULL;
        for (int i = 0; i < ths->replicaNum; ++i) {
          if (syncUtilSameId(&(pMsg->srcId), &((ths->replicasId)[i]))) {
            pSender = (ths->senders)[i];
          }
        }
        ASSERT(pSender != NULL);

        SyncIndex sentryIndex;
        if (pSender->start && pSender->term == ths->pRaftStore->currentTerm) {
          // already start
          sentryIndex = pSender->snapshot.lastApplyIndex;

        } else {
          // start send snapshot, first time
          snapshotSenderDoStart(pSender);
          pSender->start = true;
          sentryIndex = pSender->snapshot.lastApplyIndex;
        }

        // update nextIndex to sentryIndex + 1
        if (nextIndex <= sentryIndex) {
          nextIndex = sentryIndex + 1;
        }
      }

    } else {
      nextIndex = SYNC_INDEX_BEGIN;
    }

    syncIndexMgrSetIndex(ths->pNextIndex, &(pMsg->srcId), nextIndex);
    sTrace("update next index:%ld, success:%d", nextIndex, pMsg->success);
  }

  syncIndexMgrLog2("recv SyncAppendEntriesReply, after pNextIndex:", ths->pNextIndex);
  syncIndexMgrLog2("recv SyncAppendEntriesReply, after pMatchIndex:", ths->pMatchIndex);
  {
    SSnapshot snapshot;
    ths->pFsm->FpGetSnapshot(ths->pFsm, &snapshot);
    sTrace("recv SyncAppendEntriesReply, after snapshot.lastApplyIndex:%ld, snapshot.lastApplyTerm:%lu",
           snapshot.lastApplyIndex, snapshot.lastApplyTerm);
  }

  return ret;
}