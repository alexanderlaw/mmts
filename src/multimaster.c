/*
 * multimaster.c
 *
 * Multimaster based on logical replication
 *
 */
// XXX: evict that
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "postgres.h"
#include "access/xtm.h"
#include "access/clog.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/relscan.h"
#include "common/username.h"
#include "catalog/namespace.h"
#include "nodes/makefuncs.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/logical.h"
#include "replication/logicalfuncs.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "funcapi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "parser/parse_node.h"
#include "commands/extension.h"
#include "catalog/pg_class.h"
#include "commands/tablecmds.h"
#include "commands/publicationcmds.h"
#include "commands/subscriptioncmds.h"
#include "executor/spi.h"
#include "catalog/pg_type.h"
#include "tcop/tcopprot.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_publication.h"
#include "utils/syscache.h"
#include "catalog/objectaddress.h"
#include "utils/rel.h"
#include "catalog/indexing.h"
#include "utils/hsearch.h"
#include "commands/defrem.h"
#include "utils/syscache.h"

#include "multimaster.h"
#include "ddd.h"
#include "ddl.h"
#include "state.h"
#include "receiver.h"
#include "resolver.h"
#include "logger.h"

#include "compat.h"

typedef enum
{
	MTM_STATE_LOCK_ID
} MtmLockIds;

typedef struct
{
	TimestampTz	last_timestamp;
	slock_t		mutex;
} MtmTime;

static MtmTime *mtm_time;

#define MTM_SHMEM_SIZE (8*1024*1024)

void _PG_init(void);
void _PG_fini(void);

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(mtm_after_node_create);
PG_FUNCTION_INFO_V1(mtm_after_node_drop);

static size_t MtmGetTransactionStateSize(void);
static void	  MtmSerializeTransactionState(void* ctx);
static void	  MtmDeserializeTransactionState(void* ctx);
static void*  MtmCreateSavepointContext(void);
static void	  MtmRestoreSavepointContext(void* ctx);
static void	  MtmReleaseSavepointContext(void* ctx);
static void*  MtmSuspendTransaction(void);
static void   MtmResumeTransaction(void* ctx);

static void MtmShmemStartup(void);

static void launcher_init(void);
void launcher_main(Datum main_arg);
void drop_node_entry(int node_id);

MtmState* Mtm;

MemoryContext MtmApplyContext;

// XXX: do we really need all this guys (except ddd)?
static TransactionManager MtmTM =
{
	MtmDetectGlobalDeadLock,
	MtmGetTransactionStateSize,
	MtmSerializeTransactionState,
	MtmDeserializeTransactionState,
	PgInitializeSequence,
	MtmCreateSavepointContext,
	MtmRestoreSavepointContext,
	MtmReleaseSavepointContext,
	MtmSuspendTransaction,
	MtmResumeTransaction
};

LWLock *MtmCommitBarrier;
LWLock *MtmReceiverBarrier;
LWLock *MtmSyncpointLock;

// XXX
bool  MtmBackgroundWorker;
int	  MtmReplicationNodeId;
int	  MtmMaxNodes;

int	  MtmTransSpillThreshold; // XXX: align with receiver buffer size
int	  MtmHeartbeatSendTimeout;
int	  MtmHeartbeatRecvTimeout;
char* MtmRefereeConnStr;

static int	 MtmQueueSize;


static shmem_startup_hook_type PreviousShmemStartupHook;

/*
 * -------------------------------------------
 * Synchronize access to MTM structures.
 * Using LWLock seems to be more efficient (at our benchmarks)
 * Multimaster uses trash of 2N+1 lwlocks, where N is number of nodes.
 * locks[0] is used to synchronize access to multimaster state,
 * locks[1..N] are used to provide exclusive access to replication session for each node
 * locks[N+1..2*N] are used to synchronize access to distributed lock graph at each node
 * -------------------------------------------
 */

// #define DEBUG_MTM_LOCK 1

#if DEBUG_MTM_LOCK
static timestamp_t MtmLockLastReportTime;
static timestamp_t MtmLockElapsedWaitTime;
static timestamp_t MtmLockMaxWaitTime;
static size_t      MtmLockHitCount;
#endif

void MtmLock(LWLockMode mode)
{
	LWLockAcquire((LWLockId)&Mtm->locks[MTM_STATE_LOCK_ID], mode);
}

void MtmUnlock(void)
{
	LWLockRelease((LWLockId)&Mtm->locks[MTM_STATE_LOCK_ID]);
}

void MtmLockNode(int nodeId, LWLockMode mode)
{
	Assert(nodeId > 0 && nodeId <= MtmMaxNodes*2);
	LWLockAcquire((LWLockId)&Mtm->locks[nodeId], mode);
}

bool MtmTryLockNode(int nodeId, LWLockMode mode)
{
	return LWLockConditionalAcquire((LWLockId)&Mtm->locks[nodeId], mode);
}

void MtmUnlockNode(int nodeId)
{
	Assert(nodeId > 0 && nodeId <= MtmMaxNodes*2);
	LWLockRelease((LWLockId)&Mtm->locks[nodeId]);
}

/*
 * -------------------------------------------
 * System time manipulation functions
 * -------------------------------------------
 */

TimestampTz
MtmGetIncreasingTimestamp()
{
	TimestampTz now = GetCurrentTimestamp();

	/*
	 * Don't let time move backward; if it hasn't advanced, use incremented
	 * last value.
	 */
	SpinLockAcquire(&mtm_time->mutex);
	if (now <= mtm_time->last_timestamp)
		now = ++mtm_time->last_timestamp;
	else
		mtm_time->last_timestamp = now;
	SpinLockRelease(&mtm_time->mutex);

	return now;
}

timestamp_t MtmGetSystemTime(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (timestamp_t)tv.tv_sec*USECS_PER_SEC + tv.tv_usec;
}

void MtmSleep(timestamp_t usec)
{
	timestamp_t waketm = MtmGetSystemTime() + usec;

	for (;;)
	{
		int rc;
		timestamp_t sleepfor;

		CHECK_FOR_INTERRUPTS();

		sleepfor = waketm - MtmGetSystemTime();
		if (sleepfor < 0)
			break;

		rc = WaitLatch(MyLatch,
						WL_TIMEOUT | WL_POSTMASTER_DEATH,
						sleepfor/1000.0, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);
	}
}


/*
 * Distribute transaction manager functions
 */

static size_t
MtmGetTransactionStateSize(void)
{
	return sizeof(MtmTx);
}

static void
MtmSerializeTransactionState(void* ctx)
{
	memcpy(ctx, &MtmTx, sizeof(MtmTx));
}

static void
MtmDeserializeTransactionState(void* ctx)
{
	memcpy(&MtmTx, ctx, sizeof(MtmTx));
}

static void* MtmCreateSavepointContext(void)
{
	return NULL;
}

static void	 MtmRestoreSavepointContext(void* ctx)
{
}

static void	 MtmReleaseSavepointContext(void* ctx)
{
}

static void* MtmSuspendTransaction(void)
{
	MtmCurrentTrans* ctx = malloc(sizeof(MtmCurrentTrans));
	*ctx = MtmTx;
	MtmBeginTransaction();
	return ctx;
}

static void MtmResumeTransaction(void* ctx)
{
	MtmTx = *(MtmCurrentTrans*)ctx;
	free(ctx);
}


/*
 * Initialize message
 */
void MtmInitMessage(MtmArbiterMessage* msg, MtmMessageCode code)
{
	memset(msg, '\0', sizeof(MtmArbiterMessage));

	msg->code = code;
	msg->connectivityMask = SELF_CONNECTIVITY_MASK;
	msg->node = Mtm->my_node_id;
}

/*
 * Perform initialization of multimaster state.
 * This function is called from shared memory startup hook (after completion of initialization of shared memory)
 */
static void
MtmStateShmemStartup()
{
	bool found;
	int i;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	mtm_time = (MtmTime *) ShmemInitStruct("mtm_time", sizeof(MtmTime), &found);
	if (!found)
	{
		mtm_time->last_timestamp = 0;
		SpinLockInit(&mtm_time->mutex);
	}

	Mtm = (MtmState*)ShmemInitStruct(MULTIMASTER_NAME, sizeof(MtmState) + sizeof(BgwPool)*(MtmMaxNodes), &found);
	if (!found)
	{
		MemSet(Mtm, 0, sizeof(MtmState));
		Mtm->stop_new_commits = false;
		Mtm->recovered = false;
		Mtm->status = MTM_DISABLED; //MTM_INITIALIZATION;
		Mtm->recoverySlot = 0;
		Mtm->locks = GetNamedLWLockTranche(MULTIMASTER_NAME);
		Mtm->disabledNodeMask = (nodemask_t) -1;
		Mtm->clique = (nodemask_t) -1;
		Mtm->refereeGrant = false;
		Mtm->refereeWinnerId = 0;
		Mtm->stalledNodeMask = 0;
		Mtm->stoppedNodeMask = 0;
		Mtm->pglogicalReceiverMask = 0;
		Mtm->pglogicalSenderMask = 0;
		Mtm->recoveryCount = 0;
		Mtm->localTablesHashLoaded = false;
		Mtm->latestSyncpoint = InvalidXLogRecPtr;

		// XXX: change to dsa and make it per-receiver
		for (i = 0; i < MtmMaxNodes; i++)
		{
			Mtm->dmq_dest_ids[i] = -1;
			BgwPoolInit(&Mtm->pools[i], MtmExecutor, MtmQueueSize, 0);
		}
	}

	RegisterXactCallback(MtmXactCallback2, NULL);

	MtmCommitBarrier = &(GetNamedLWLockTranche(MULTIMASTER_NAME)[MtmMaxNodes*2+1].lock);
	MtmReceiverBarrier = &(GetNamedLWLockTranche(MULTIMASTER_NAME)[MtmMaxNodes*2+2].lock);
	MtmSyncpointLock = &(GetNamedLWLockTranche(MULTIMASTER_NAME)[MtmMaxNodes*2+3].lock);

	TM = &MtmTM;
	LWLockRelease(AddinShmemInitLock);
}

static void
MtmShmemStartup(void)
{
	if (PreviousShmemStartupHook)
		PreviousShmemStartupHook();

	MtmDeadlockDetectorShmemStartup(MtmMaxNodes);
	MtmDDLReplicationShmemStartup();
	MtmStateShmemStartup();
}

void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.	 (We don't throw error here because it seems useful to
	 * allow the cs_* functions to be created even when the
	 * module isn't active.	 The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable(
		"multimaster.heartbeat_send_timeout",
		"Timeout in milliseconds of sending heartbeat messages",
		"Period of broadcasting heartbeat messages by arbiter to all nodes",
		&MtmHeartbeatSendTimeout,
		200,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.heartbeat_recv_timeout",
		"Timeout in milliseconds of receiving heartbeat messages",
		"If no heartbeat message is received from node within this period, it assumed to be dead",
		&MtmHeartbeatRecvTimeout,
		1000,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.max_nodes",
		"Maximal number of cluster nodes",
		"This parameters allows to add new nodes to the cluster, default value 0 restricts number of nodes to one specified in multimaster.conn_strings",
		&MtmMaxNodes,
		6,
		0,
		MTM_MAX_NODES,
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.trans_spill_threshold",
		"Maximal size of transaction after which transaction is written to the disk",
		NULL,
		&MtmTransSpillThreshold,
		100 * 1024, /* 100Mb */
		0,
		MaxAllocSize/1024,
		PGC_SIGHUP,
		GUC_UNIT_KB,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.monotonic_sequences",
		"Enforce monotinic behaviour of sequence values obtained from different nodes",
		NULL,
		&MtmMonotonicSequences,
		false,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.ignore_tables_without_pk",
		"Do not replicate tables without primary key",
		NULL,
		&MtmIgnoreTablesWithoutPk,
		false,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomStringVariable(
		"multimaster.referee_connstring",
		"Referee connection string",
		NULL,
		&MtmRefereeConnStr,
		"",
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.volkswagen_mode",
		"Pretend to be normal postgres. This means skip some NOTICE's and use local sequences. Default false.",
		NULL,
		&MtmVolksWagenMode,
		false,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.max_workers",
		"Maximal number of multimaster dynamic executor workers",
		NULL,
		&MtmMaxWorkers,
		100,
		0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.queue_size",
		"Multimaster queue size",
		NULL,
		&MtmQueueSize,
		10*1024*1024,
		1024*1024,
		INT_MAX,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomStringVariable(
		"multimaster.remote_functions",
		"List of function names which should be executed remotely at all multimaster nodes instead of executing them at master and replicating result of their work",
		NULL,
		&MtmRemoteFunctionsList,
		"lo_create,lo_unlink",
		PGC_USERSET, /* context */
		GUC_LIST_INPUT, /* flags */
		NULL,		 /* GucStringCheckHook check_hook */
		MtmSetRemoteFunction,		 /* GucStringAssignHook assign_hook */
		NULL		 /* GucShowHook show_hook */
	);

	MtmDeadlockDetectorInit(MtmMaxNodes);

	/*
	 * Request additional shared resources.	 (These are no-ops if we're not in
	 * the postmaster process.)	 We'll allocate or attach to the shared
	 * resources in mtm_shmem_startup().
	 */
	RequestAddinShmemSpace(MTM_SHMEM_SIZE + MtmMaxNodes*MtmQueueSize + sizeof(MtmTime));
	RequestNamedLWLockTranche(MULTIMASTER_NAME, 1 + MtmMaxNodes*2 + 3);

	dmq_init();
	dmq_receiver_start_hook = MtmOnNodeConnect;
	dmq_receiver_stop_hook = MtmOnNodeDisconnect;

	ResolverInit();

	MtmDDLReplicationInit();

	launcher_init();

	/*
	 * Install hooks.
	 */
	PreviousShmemStartupHook = shmem_startup_hook;
	shmem_startup_hook = MtmShmemStartup;
}

/*
 * Module unload callback
 *
 * XXX: check 'drop extension multimaster'
 */
void
_PG_fini(void)
{
	shmem_startup_hook = PreviousShmemStartupHook;
}

/*
 * Genenerate global transaction identifier for two-pahse commit.
 * It should be unique for all nodes
 */
void
MtmGenerateGid(char *gid, TransactionId xid, int node_id)
{
	sprintf(gid, "MTM-%d-" XID_FMT, node_id, xid);
	return;
}

int
MtmGidParseNodeId(const char* gid)
{
	int node_id = -1;
	sscanf(gid, "MTM-%d-%*d", &node_id);
	return node_id;
}

TransactionId
MtmGidParseXid(const char* gid)
{
	TransactionId xid = InvalidTransactionId;
	sscanf(gid, "MTM-%*d-" XID_FMT, &xid);
	Assert(TransactionIdIsValid(xid));
	return xid;
}

/*
 * Publication named 'multimaster' acts as a flag that that multimaster
 * extension was created and configured, so we can hijack transactions.
 * We can't hijack transactions before configuration is done because
 * configuration itself is going to need some transactions that better not
 * be aborted because of Mtm->status being DISABLED. Also publication
 * is uniq with respect to (db_id, pub_name) so "All your base are belong
 * to us" won't happen.
 */
bool
MtmIsEnabled()
{
	return OidIsValid(get_publication_oid(MULTIMASTER_NAME, true));
}

bool
MtmAllApplyWorkersFinished()
{
	int i;

	for (i = 0; i < Mtm->nAllNodes; i++)
	{
		volatile int ntasks;

		if (i == Mtm->my_node_id - 1)
			continue;

		SpinLockAcquire(&Mtm->pools[i].lock);
		ntasks = Mtm->pools[i].active + Mtm->pools[i].pending;
		SpinLockRelease(&Mtm->pools[i].lock);

		mtm_log(MtmApplyBgwFinish, "MtmAllApplyWorkersFinished %d tasks not finished", ntasks);

		if (ntasks != 0)
			false;
	}

	return true;
}

/*****************************************************************************
 *
 * Node management stuff.
 *
 *****************************************************************************/

/*
 * Check correctness of multimaster configuration
 */
static bool
check_config()
{
	bool ok = true;

	if (MtmMaxNodes < 1)
	{
		mtm_log(WARNING, "multimaster requires multimaster.max_nodes > 0");
		ok = false;
	}

	if (max_prepared_xacts < 1)
	{
		mtm_log(WARNING,
			 "multimaster requires max_prepared_transactions > 0, "
			 "because all transactions are implicitly two-phase");
		ok = false;
	}

	{
		int workers_required = 2 * MtmMaxNodes + 1;
		if (max_worker_processes < workers_required)
		{
			mtm_log(WARNING,
				 "multimaster requires max_worker_processes >= %d",
				 workers_required);
			ok = false;
		}
	}

	if (wal_level != WAL_LEVEL_LOGICAL)
	{
		mtm_log(WARNING,
			 "multimaster requires wal_level = 'logical', "
			 "because it is build on top of logical replication");
		ok = false;
	}

	if (max_wal_senders < MtmMaxNodes)
	{
		mtm_log(WARNING,
			 "multimaster requires max_wal_senders >= %d (multimaster.max_nodes), ",
			 MtmMaxNodes);
		ok = false;
	}

	if (max_replication_slots < MtmMaxNodes)
	{
		mtm_log(WARNING,
			 "multimaster requires max_replication_slots >= %d (multimaster.max_nodes), ",
			 MtmMaxNodes);
		ok = false;
	}

	return ok;
}

Datum
mtm_after_node_create(PG_FUNCTION_ARGS)
{
	CreatePublicationStmt *pub_stmt = makeNode(CreatePublicationStmt);
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	int			node_id;
	bool		node_id_isnull;
	bool		is_self;
	bool		is_self_isnull;
	char	   *conninfo;
	bool		conninfo_isnull;
	char	   *origin_name;
	char	   *recovery_slot;

	Assert(CALLED_AS_TRIGGER(fcinfo));
	Assert(TRIGGER_FIRED_FOR_ROW(trigdata->tg_event));
	Assert(TRIGGER_FIRED_BY_INSERT(trigdata->tg_event));

	node_id = DatumGetInt32(heap_getattr(trigdata->tg_trigtuple,
							 Anum_mtm_nodes_id,
							 RelationGetDescr(trigdata->tg_relation),
							 &node_id_isnull));
	Assert(!node_id_isnull);

	conninfo = text_to_cstring(DatumGetTextP(heap_getattr(trigdata->tg_trigtuple,
							 Anum_mtm_nodes_connifo,
							 RelationGetDescr(trigdata->tg_relation),
							 &conninfo_isnull)));

	is_self = DatumGetBool(heap_getattr(trigdata->tg_trigtuple,
							 Anum_mtm_nodes_is_self,
							 RelationGetDescr(trigdata->tg_relation),
							 &is_self_isnull));
	Assert(!is_self_isnull);

	if (node_id <= 0 || node_id > MTM_MAX_NODES)
		mtm_log(ERROR,
			"node_id should be in range from 1 to %d, but %d is given",
			MTM_MAX_NODES, node_id);

	if (!check_config())
		mtm_log(ERROR, "multimaster can't start with current configs");

	if (is_self)
	{
		/*
		 * Create dummy pub. It will be used by backends to check whether
		 * multimaster is configured.
		 */
		pub_stmt->pubname = MULTIMASTER_NAME;
		pub_stmt->for_all_tables = true;
		pub_stmt->tables = NIL;
		pub_stmt->options = NIL;
		CreatePublication(pub_stmt);

		/* liftoff */
		MtmMonitorStart(MyDatabaseId, GetUserId());
	}
	else
	{
		CreateSubscriptionStmt *cs_stmt = makeNode(CreateSubscriptionStmt);
		int			my_node_id = Mtm->my_node_id;

		if (my_node_id == 0)
		{
			const char *em_node_id =
						GetConfigOption("mtm.emerging_node_id", true, false);

			if (em_node_id == NULL)
				mtm_log(ERROR,
					"please create node with is_self = 'true' first or set mtm.emerging_node_id");

			sscanf(em_node_id, "%d", &my_node_id);

			if (my_node_id <= 0 || my_node_id > MTM_MAX_NODES)
				mtm_log(ERROR,
					"mtm.emerging_node_id should be in range from 1 to %d", MTM_MAX_NODES);
		}

		Assert(!conninfo_isnull);

		/*
		 * We are not creating slots for other nodes on our node by calling
		 * ReplicationSlotCreate() because of following reasons:
		 *   * Slot creation requires transaction without assigned xid. So when we want
		 *     to setup bunch of nodes we can't just several times call function that
		 *     setups individual node.
		 *   * That way our receiver will not face situation when slot on remote
		 *     node wasn't yet created during initial setup.
		 *   * We anyway need to check connection string.
		 *
		 * So instead we are creating slots for our node on neighbour.
		 */
		MtmReceiverCreateSlot(conninfo, my_node_id);

		/*
		 * Dummy subscription. It is used by launcher to start workers in databases
		 * where multimaster is configured (pg_publication is shared catalog
		 * relation, so launcher can find it from postgres database). Also our
		 * workers and backends are subscribed to cache invalidations of
		 * pg_publication, so that can know aboun node creating/deletion.
		 */
		cs_stmt->subname = psprintf(MTM_SUBNAME_FMT, node_id);
		cs_stmt->conninfo = conninfo;
		cs_stmt->publication = list_make1(makeString(MULTIMASTER_NAME));
		cs_stmt->options = list_make4(
			makeDefElem("slot_name", (Node *) makeString("none"), -1),
			makeDefElem("create_slot", (Node *) makeString("false"), -1),
			makeDefElem("connect", (Node *) makeString("false"), -1),
			makeDefElem("enabled", (Node *) makeString("false"), -1)
		);
		CreateSubscription(cs_stmt, true);

		/* Create recovery slot to hold WAL files that we may need during recovery */
		recovery_slot = psprintf(MULTIMASTER_RECOVERY_SLOT_PATTERN, node_id);
		ReplicationSlotCreate(recovery_slot, false, RS_PERSISTENT);
		ReplicationSlotReserveWal();
		/* Write this slot to disk */
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
		ReplicationSlotRelease();

		/*
		 * Create origin for this neighbour.
		 * It is tempting to use 'pg_#{suboid}' but accessing syscache in
		 * MtmLoadConfig() will lead to deadlock if receiver tries to load
		 * config just before committing tx that modified subscriptions.
		 *
		 * Other way around is to write suboid to mtm.nodes tuple, but that is
		 * too much ado for now.
		 */
		origin_name = psprintf(MULTIMASTER_SLOT_PATTERN, node_id);
		replorigin_create(origin_name);
	}

	PG_RETURN_VOID();
}

Datum
mtm_after_node_drop(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	int			node_id;
	bool		node_id_isnull;
	bool		is_self;
	bool		is_self_isnull;

	Assert(CALLED_AS_TRIGGER(fcinfo));
	Assert(TRIGGER_FIRED_FOR_ROW(trigdata->tg_event));
	Assert(TRIGGER_FIRED_BY_DELETE(trigdata->tg_event));

	node_id = DatumGetInt32(heap_getattr(trigdata->tg_trigtuple,
							 Anum_mtm_nodes_id,
							 RelationGetDescr(trigdata->tg_relation),
							 &node_id_isnull));
	Assert(!node_id_isnull);
	Assert(node_id > 0 && node_id <= MTM_MAX_NODES);

	is_self = DatumGetBool(heap_getattr(trigdata->tg_trigtuple,
							 Anum_mtm_nodes_is_self,
							 RelationGetDescr(trigdata->tg_relation),
							 &is_self_isnull));
	Assert(!is_self_isnull);

	/*
	 * This will produce invalidation that others can consume and reload
	 * state.
	 */
	if (is_self)
	{
		DropStmt *dp_stmt = makeNode(DropStmt);

		dp_stmt->removeType = OBJECT_PUBLICATION;
		dp_stmt->behavior = DROP_CASCADE;
		dp_stmt->concurrent = false;
		dp_stmt->missing_ok = false;
		dp_stmt->objects = list_make1(makeString(MULTIMASTER_NAME));
		RemoveObjects(dp_stmt);
	}
	else
	{
		DropSubscriptionStmt *ds_stmt = makeNode(DropSubscriptionStmt);

		ds_stmt->subname = psprintf(MTM_SUBNAME_FMT, node_id);
		DropSubscription(ds_stmt, true);
	}

	PG_RETURN_VOID();
}

/*
 * Load mtm config.
 *
 * In case of absense of configured nodes will return cfg->n_nodes = 0.
 */
MtmConfig *
MtmLoadConfig()
{
	MtmConfig  *cfg;
	int			rc;
	int			i;
	bool		inside_tx = IsTransactionState();
	TupleDesc	tupdesc;

	cfg = (MtmConfig *) MemoryContextAlloc(TopMemoryContext,
										   sizeof(MtmConfig));

	if (!inside_tx)
		StartTransactionCommand();

	/* Load node_id's with connection strings from mtm.nodes */

	if (SPI_connect() != SPI_OK_CONNECT)
		mtm_log(ERROR, "could not connect using SPI");

	PushActiveSnapshot(GetTransactionSnapshot());

	rc = SPI_execute("select * from mtm.nodes order by id asc", true, 0);
	if (rc < 0 || rc != SPI_OK_SELECT)
		mtm_log(ERROR, "Failed to load saved nodes");

	Assert(SPI_processed <= MtmMaxNodes);
	tupdesc	= SPI_tuptable->tupdesc;

	cfg->n_nodes = 0;
	cfg->my_node_id = 0;

	for (i = 0; i < SPI_processed; i++)
	{
		HeapTuple	tup = SPI_tuptable->vals[i];
		bool		isnull;
		int			node_id;
		char	   *connstr;
		bool		is_self;

		node_id = DatumGetInt32(SPI_getbinval(tup, tupdesc, Anum_mtm_nodes_id, &isnull));
		Assert(!isnull);
		Assert(TupleDescAttr(tupdesc, Anum_mtm_nodes_id - 1)->atttypid == INT4OID);

		connstr = SPI_getvalue(tup, tupdesc, Anum_mtm_nodes_connifo);
		Assert(TupleDescAttr(tupdesc, Anum_mtm_nodes_connifo - 1)->atttypid == TEXTOID);

		is_self = DatumGetBool(SPI_getbinval(tup, tupdesc, Anum_mtm_nodes_is_self, &isnull));
		Assert(!isnull);
		Assert(TupleDescAttr(tupdesc, Anum_mtm_nodes_is_self - 1)->atttypid == BOOLOID);

		/* Empty connstr mean that this is our node */
		if (is_self)
		{
			/* Ensure that there is only one tuple representing our node */
			Assert(cfg->my_node_id == 0);
			cfg->my_node_id = node_id;
		}
		else
		{
			/* Assume that connstr correctness was checked upon creation */
			cfg->nodes[cfg->n_nodes].conninfo = MemoryContextStrdup(TopMemoryContext, connstr);
			cfg->nodes[cfg->n_nodes].node_id = node_id;
			Assert(cfg->nodes[cfg->n_nodes].conninfo != NULL);
			cfg->n_nodes++;
		}
	}

	/* Load origin_id's */
	for (i = 0; i < cfg->n_nodes; i++)
	{
		char	   *origin_name;
		RepOriginId	origin_id;
		int			node_id = cfg->nodes[i].node_id;

		origin_name = psprintf(MULTIMASTER_SLOT_PATTERN, node_id);
		origin_id = replorigin_by_name(origin_name, true);

		Assert(origin_id != InvalidRepOriginId);
		cfg->nodes[i].origin_id = origin_id;
	}

	SPI_finish();
	PopActiveSnapshot();

	if (!inside_tx)
		CommitTransactionCommand();

	return cfg;
}

MtmConfig *
MtmReloadConfig(MtmConfig *old_cfg, mtm_cfg_change_cb node_add_cb,
				mtm_cfg_change_cb node_drop_cb, Datum arg)
{
	MtmConfig  *new_cfg;
	Bitmapset  *old_bms = NULL,
			   *new_bms = NULL,
			   *deleted,
			   *created;
	int			i,
				node_id;

	new_cfg = MtmLoadConfig();

	/* Set proper values in Mtm structure */
	MtmStateFill(new_cfg);

	/*
	 * Construct bitmapsets from old and new mtm_config's and find
	 * out whether some nodes were added or deleted.
	 */
	if (old_cfg != NULL)
	{
		for (i = 0; i < old_cfg->n_nodes; i++)
			old_bms = bms_add_member(old_bms, old_cfg->nodes[i].node_id);
	}
	for (i = 0; i < new_cfg->n_nodes; i++)
		new_bms = bms_add_member(new_bms, new_cfg->nodes[i].node_id);

	deleted = bms_difference(old_bms, new_bms);
	created = bms_difference(new_bms, old_bms);

	/*
	 * Call launch/stop callbacks for added/deleted nodes.
	 */
	if (node_add_cb)
	{
		node_id = -1;
		while ((node_id = bms_next_member(created, node_id)) >= 0)
			node_add_cb(node_id, new_cfg, arg);
	}

	if (node_drop_cb)
	{
		node_id = -1;
		while ((node_id = bms_next_member(deleted, node_id)) >= 0)
			node_drop_cb(node_id, new_cfg, arg);
	}

	if (old_cfg != NULL)
		pfree(old_cfg);

	return new_cfg;
}

/* Helper to find node with specified id in cfg->nodes */
MtmNode *
MtmNodeById(MtmConfig *cfg, int node_id)
{
	int			i;

	for (i = 0; i < cfg->n_nodes; i++)
	{
		if (cfg->nodes[i].node_id == node_id)
			return &(cfg->nodes[i]);
	}

	return NULL;
}

/*****************************************************************************
 *
 * Launcher worker.
 *
 * During node boot searches for configured multimasters inspecting
 * pg_subscription and starts mtm-monitor.
 *
 *****************************************************************************/

/*
 * Register static worker for launcher.
 */
static void
launcher_init()
{
	BackgroundWorker worker;

	/* Set up common data for worker */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "multimaster");
	sprintf(worker.bgw_function_name, "launcher_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "mtm-launcher");
	snprintf(worker.bgw_type, BGW_MAXLEN, "mtm-launcher");
	RegisterBackgroundWorker(&worker);
}

/*
 * Scans for all databases with enabled multimaster.
 */
void
launcher_main(Datum main_arg)
{
	Relation		rel;
	HeapScanDesc	scan;
	HeapTuple		tup;
	HASHCTL			hash_info;
	HTAB		   *already_started;

	/* init this worker */
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	memset(&hash_info, 0, sizeof(hash_info));
	hash_info.entrysize = hash_info.keysize = sizeof(Oid);
	already_started = hash_create("already_started", 16, &hash_info,
								  HASH_ELEM | HASH_BLOBS);

	/* Connect to a postgres database */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	/*
	 * Start a transaction so we can access pg_subscription, and get a snapshot.
	 * We don't have a use for the snapshot itself, but we're interested in
	 * the secondary effect that it sets RecentGlobalXmin.  (This is critical
	 * for anything that reads heap pages, because HOT may decide to prune
	 * them even if the process doesn't attempt to modify any tuples.)
	 */
	StartTransactionCommand();
	(void) GetTransactionSnapshot();

	rel = heap_open(SubscriptionRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 0, NULL);

	/* is there any mtm subscriptions in a given database? */
	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_subscription subform = (Form_pg_subscription) GETSTRUCT(tup);
		int			node_id;

		if (!subform->subenabled &&
			sscanf(NameStr(subform->subname), MTM_SUBNAME_FMT, &node_id) == 1 &&
			!hash_search(already_started, &subform->subdbid, HASH_FIND, NULL))
		{
			bool		found;

			MtmMonitorStart(subform->subdbid, subform->subowner);
			hash_search(already_started, &subform->subdbid, HASH_ENTER, &found);
			Assert(!found);
		}
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	CommitTransactionCommand();
}



