/*----------------------------------------------------------------------------
 *
 * commit.c
 *		Replace ordinary commit with 3PC.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *----------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/twophase.h"
#include "access/transam.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "commands/dbcommands.h"
#include "tcop/tcopprot.h"

#include "multimaster.h"
#include "logger.h"
#include "ddl.h"
#include "state.h"
#include "syncpoint.h"

static Oid		MtmDatabaseId;
static bool		DmqSubscribed;
static int		sender_to_node[MTM_MAX_NODES];

MtmCurrentTrans MtmTx;

static void MtmBeginTransaction(MtmCurrentTrans* x);
static void MtmPrePrepareTransaction(MtmCurrentTrans* x);

static bool GatherPrepares(MtmCurrentTrans* x, nodemask_t participantsMask,
						   int *failed_at);
static void GatherPrecommits(MtmCurrentTrans* x, nodemask_t participantsMask, MtmMessageCode code);


void
MtmXactCallback2(XactEvent event, void *arg)
{
	if (!MtmIsLogicalReceiver)
	{
		switch (event)
		{
			case XACT_EVENT_START:
				MtmBeginTransaction(&MtmTx);
				break;
			case XACT_EVENT_PRE_PREPARE:
				MtmPrePrepareTransaction(&MtmTx);
				break;
			case XACT_EVENT_COMMIT_COMMAND:
				if (IsTransactionOrTransactionBlock()
						&& !IsTransactionBlock()
						&& !IsSubTransaction())
				{
					MtmTwoPhaseCommit(&MtmTx);
				}
				break;
			default:
				break;
		}
	}
}

static void
MtmBeginTransaction(MtmCurrentTrans* x)
{
	// XXX: move it down the callbacks?
	x->isDistributed = MtmIsUserTransaction();
	x->containsDML = false; // will be set by executor hook

	MtmDDLResetStatement();

	/* XXX: ugly hack with debug_query_string */

	/* Application name can be changed using PGAPPNAME environment variable */
	if (x->isDistributed && Mtm->status != MTM_ONLINE
		&& strcmp(application_name, MULTIMASTER_ADMIN) != 0
		&& strcmp(application_name, MULTIMASTER_BROADCAST_SERVICE) != 0
		&& debug_query_string && pg_strcasecmp(debug_query_string, "create extension multimaster;") != 0)
	{
		/* Reject all user's transactions at offline cluster.
		 * Allow execution of transaction by bg-workers to makeit possible to perform recovery.
		 */
		mtm_log(ERROR,
				"Multimaster node is not online: current status %s",
				MtmNodeStatusMnem[Mtm->status]);
	}
}

static void
MtmPrePrepareTransaction(MtmCurrentTrans* x)
{
	if (!x->isDistributed)
		return;

	if (!MtmDatabaseId)
		MtmDatabaseId = get_database_oid(MtmDatabaseName, false);

	if (MtmDatabaseId != MyDatabaseId)
		mtm_log(ERROR,
			"Refusing to work. Multimaster configured to work with database '%s'",
			MtmDatabaseName);

	Assert(TransactionIdIsValid(GetCurrentTransactionId()));
}

bool // XXX: do we need that bool?
MtmTwoPhaseCommit(MtmCurrentTrans* x)
{
	nodemask_t participantsMask;
	bool	ret;
	int		failed_at;
	TransactionId xid;
	char	stream[DMQ_NAME_MAXLEN];
	pgid_t  gid;

	/* XXX: avoid Mtm->extension_created */
	if (!x->isDistributed || !x->containsDML || !Mtm->extension_created)
		return false;

	if (!DmqSubscribed)
	{
		int i, sender_id = 0;
		for (i = 0; i < Mtm->nAllNodes; i++)
		{
			if (i + 1 != MtmNodeId)
			{
				dmq_attach_receiver(psprintf("node%d", i + 1), i);
				sender_to_node[sender_id++] = i + 1;
			}
		}
		DmqSubscribed = true;
	}

	if (!IsTransactionBlock())
	{
		BeginTransactionBlock(false);
		CommitTransactionCommand();
		StartTransactionCommand();
	}

	xid = GetTopTransactionId();
	MtmGenerateGid(gid, xid);
	sprintf(stream, "xid" XID_FMT, xid);
	dmq_stream_subscribe(stream);
	mtm_log(MtmTxTrace, "%s subscribed for %s", gid, stream);

	x->xid = xid;

	/*
	 * This lock is taken for a quite a long period of time but normally
	 * all callers lock it in shared mode, so it shouldn't be noticeable
	 * performance-wise.
	 *
	 * It is only used during startup of WalSender(node_id) in recovered mode
	 * to create a barrier after which all transactions doing our 3PC are
	 * guaranted to have seen participantsMask with node_id enabled, so the
	 * receiver can apply them in parallel and be sure that precommit will
	 * not happens before node_id applies prepare.
	 *
	 * See also comments at the end of MtmReplicationStartupHook().
	 */
	while (Mtm->stop_new_commits)
		MtmSleep(USECS_PER_SEC);

	LWLockAcquire(MtmCommitBarrier, LW_SHARED);

	MtmLock(LW_SHARED);
	participantsMask = (((nodemask_t)1 << Mtm->nAllNodes) - 1) &
								  ~Mtm->disabledNodeMask &
								  ~((nodemask_t)1 << (MtmNodeId-1));
	if (Mtm->status != MTM_ONLINE)
		mtm_log(ERROR, "This node became offline during current transaction");
	MtmUnlock();

	ret = PrepareTransactionBlock(gid);
	if (!ret)
	{
		if (!MtmVolksWagenMode)
			mtm_log(WARNING, "Failed to prepare transaction %s", gid);
		return true;
	}
	mtm_log(MtmTxFinish, "TXFINISH: %s prepared", gid);
	CommitTransactionCommand();

	ret = GatherPrepares(x, participantsMask, &failed_at);
	if (!ret)
	{
		dmq_stream_unsubscribe(stream);
		FinishPreparedTransaction(gid, false, false);
		mtm_log(MtmTxFinish, "TXFINISH: %s aborted", gid);
		mtm_log(ERROR, "Failed to prepare transaction %s at node %d",
						gid, failed_at);
	}

	SetPreparedTransactionState(gid, MULTIMASTER_PRECOMMITTED);
	mtm_log(MtmTxFinish, "TXFINISH: %s precommitted", gid);
	GatherPrecommits(x, participantsMask, MSG_PRECOMMITTED);

	StartTransactionCommand();
	FinishPreparedTransaction(gid, true, false);
	mtm_log(MtmTxFinish, "TXFINISH: %s committed", gid);
	GatherPrecommits(x, participantsMask, MSG_COMMITTED);

	LWLockRelease(MtmCommitBarrier);

	dmq_stream_unsubscribe(stream);
	mtm_log(MtmTxTrace, "%s unsubscribed for %s", gid, stream);

	MaybeLogSyncpoint();

	return true;
}

static bool
GatherPrepares(MtmCurrentTrans* x, nodemask_t participantsMask, int *failed_at)
{
	bool prepared = true;

	Assert(participantsMask != 0);

	while (participantsMask != 0)
	{
		bool ret;
		DmqSenderId sender_id;
		StringInfoData buffer;
		MtmArbiterMessage *msg;

		ret = dmq_pop(&sender_id, &buffer, participantsMask);

		if (ret)
		{
			msg = (MtmArbiterMessage *) buffer.data;

			Assert(msg->node == sender_to_node[sender_id]);
			Assert(msg->code == MSG_PREPARED || msg->code == MSG_ABORTED);
			Assert(msg->dxid == x->xid);
			Assert(BIT_CHECK(participantsMask, sender_to_node[sender_id] - 1));

			mtm_log(MtmTxTrace,
				"GatherPrepares: got '%s' for tx" XID_FMT " from node%d",
				msg->code == MSG_PREPARED ? "ok" : "failed",
				x->xid, sender_to_node[sender_id]);

			BIT_CLEAR(participantsMask, sender_to_node[sender_id] - 1);

			if (msg->code == MSG_ABORTED)
			{
				prepared = false;
				*failed_at = msg->node;
			}
		}
		else
		{
			/*
			 * If queue is detached then the neignbour node is probably
			 * disconnected. Let's wait when it became disabled as we can
			 * became offline by this time.
			 */
			MtmLock(LW_SHARED);
			if (BIT_CHECK(Mtm->disabledNodeMask, sender_to_node[sender_id] - 1))
			{
				if (Mtm->status != MTM_ONLINE)
				{
					elog(ERROR, "our node was disabled during transaction commit");
				}
				else
				{
					BIT_CLEAR(participantsMask, sender_to_node[sender_id] - 1);
					mtm_log(MtmTxTrace,
						"GatherPrepares: dropping node%d from participants of tx" XID_FMT,
						sender_to_node[sender_id], x->xid);
					prepared = false;
					*failed_at = sender_to_node[sender_id];
				}
			}
			MtmUnlock();
		}
	}

	// XXX: assert that majority has responded

	return prepared;
}

static void
GatherPrecommits(MtmCurrentTrans* x, nodemask_t participantsMask, MtmMessageCode code)
{
	Assert(participantsMask != 0);

	while (participantsMask != 0)
	{
		bool ret;
		DmqSenderId sender_id;
		StringInfoData buffer;
		MtmArbiterMessage *msg;

		ret = dmq_pop(&sender_id, &buffer, participantsMask);

		if (ret)
		{
			msg = (MtmArbiterMessage *) buffer.data;

			Assert(msg->node == sender_to_node[sender_id]);
			Assert(msg->code == code);
			Assert(msg->dxid == x->xid);
			Assert(BIT_CHECK(participantsMask, sender_to_node[sender_id] - 1));

			mtm_log(MtmTxTrace,
				"GatherPrecommits: got 'ok' for tx" XID_FMT " from node%d",
				x->xid, sender_to_node[sender_id]);

			BIT_CLEAR(participantsMask, sender_to_node[sender_id] - 1);
		}
		else
		{
			/*
			 * If queue is detached then the neignbour node is probably
			 * disconnected. Let's wait when it became disabled as we can
			 * became offline by this time.
			 */
			MtmLock(LW_SHARED);
			if (BIT_CHECK(Mtm->disabledNodeMask, sender_to_node[sender_id] - 1))
			{
				if (Mtm->status != MTM_ONLINE)
				{
					elog(ERROR, "our node was disabled during transaction commit");
				}
				else
				{
					BIT_CLEAR(participantsMask, sender_to_node[sender_id] - 1);
					mtm_log(MtmTxTrace,
						"GatherPrecommit: dropping node%d from participants of tx" XID_FMT,
						sender_to_node[sender_id], x->xid);
				}
			}
			MtmUnlock();
		}
	}

	// XXX: assert that majority has responded
}