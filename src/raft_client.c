/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2020
 */

#include <stdlib.h>
#include <uuid/uuid.h>

#include "alloc.h"
#include "common.h"
#include "crc32.h"
#include "epoll_mgr.h"
#include "log.h"
#include "raft_net.h"
#include "raft.h"
#include "raft_client.h"
#include "random.h"
#include "ref_tree_proto.h"
#include "registry.h"

REGISTRY_ENTRY_FILE_GENERATE;

#define RAFT_CLIENT_MAX_INSTANCES 8
#define RAFT_CLIENT_RPC_SENDER_MAX 8
#define RAFT_CLIENT_EVP_IDX 0

// This is the same as the number of total pending requests per RCI
#define RAFT_CLIENT_MAX_SUB_APP_INSTANCES 4096

typedef void * raft_client_thread_t;
typedef int    raft_client_app_ctx_int_t; // raft client app thread
typedef void   raft_client_app_ctx_t;
typedef void   raft_client_epoll_t;
typedef int    raft_client_epoll_int_t;

#define RAFT_CLIENT_SUCCESSFUL_PING_UNTIL_VIABLE 4
static size_t raftClientNpingsUntilViable =
    RAFT_CLIENT_SUCCESSFUL_PING_UNTIL_VIABLE;

static int raftClientSubAppMax = RAFT_CLIENT_MAX_SUB_APP_INSTANCES;

#define RAFT_CLIENT_TIMERFD_EXPIRE_MS 10U
static unsigned long long raftClientTimerFDExpireMS =
    RAFT_CLIENT_TIMERFD_EXPIRE_MS;

#define RAFT_CLIENT_REQUEST_RATE_PER_SEC 1000
static size_t raftClientRequestRatePerSec = RAFT_CLIENT_REQUEST_RATE_PER_SEC;

#define RAFT_CLIENT_STALE_SERVER_TIME_MS        \
    (RAFT_CLIENT_TIMERFD_EXPIRE_MS * RAFT_CLIENT_TIMERFD_EXPIRE_MS)
static unsigned long long raftClientStaleServerTimeMS =
    RAFT_CLIENT_STALE_SERVER_TIME_MS;

static unsigned long long raftClientRetryTimeoutMS =
    (RAFT_CLIENT_TIMERFD_EXPIRE_MS * 2);

static bool raftClientLeaderIsViable = false;

static pthread_mutex_t raftClientMutex = PTHREAD_MUTEX_INITIALIZER;

static struct raft_client_instance
    *raftClientInstances[RAFT_CLIENT_MAX_INSTANCES];

/**
 * raft_client_request_handle -
 * @rcrh_arg:  application state which may be applied to the request.
 *    Typically used for non-blocking requests.  The raft client does not read
 *    or modify data pointed to by this member.
 * @rcrh_ready:  flag which denotes the request is ready for app processing.
 * @rcrh_blocking:  application is blocking, pthread_cond_signal() is needed.
 * @rcrh_submitted:  request submission time.
 * @rcrh_timeout:  max time a request should wait.  Blocking requests will be
 *    signaled either when the request complete or when the timeout expires -
 *    whichever occurs first.  NOTE: write requests may complete on the server
 *    side after the timeout has expired.
 * @rcrh_num_retries:  number of retries necessary to complete request.
 * @rcrh_reply_buf:  reply buffer which has been allocated by the application
 *    and populated by the raft_client.
 * @rcrh_reply_used_size:  amount of data which has from @rcrh_reply_buf which
 *    has been filled by the raft client reply handler.
 * @rcrh_reply_size:  The size of @rcrh_reply_buf.  If the reply size is
 *    greater than @rcrh_reply_buf_max_size, an error will be set and
 *    @rcrh_reply_size contain the RPC size value.
 * @rcrh_sin_reply_addr:  IP address of the server which made the reply.
 * @rcrh_sin_reply_port:  Port number of the replying server.
 * @rcrh_error:  Request error.  Typically this should be the rcrm_app_error
 *    from the raft client RPC.
 */
struct raft_client_request_handle
{
    void                       *rcrh_arg;
    uint8_t                     rcrh_ready:1;
    uint8_t                     rcrh_completing:1; // mutually ex with cancel
    uint8_t                     rcrh_initializing:1;
    uint8_t                     rcrh_blocking:1;
    uint8_t                     rcrh_sendq:1;
    uint8_t                     rcrh_cancel:1;
    uint8_t                     rcrh_cb_exec:1;
    int16_t                     rcrh_error;
    uint16_t                    rcrh_sin_reply_port;
    struct in_addr              rcrh_sin_reply_addr;
    struct timespec             rcrh_submitted;
    struct timespec             rcrh_last_send;
    const struct timespec       rcrh_timeout;
    size_t                      rcrh_num_sends;
    size_t                      rcrh_reply_used_size;
    const size_t                rcrh_reply_size;
    struct raft_client_rpc_msg *rcrh_rpc;
    char                       *rcrh_reply_buf;
    void                      (*rcrh_async_cb)(
        const struct raft_net_client_user_id *, void *, char *, size_t, int);
};

#define SA_2_RPC(sa) (sa)->rcsa_rh.rcrh_rpc
#define RCI_2_RI(rci) (rci)->rci_ri

struct raft_client_interface;

/**
 * raft_client_sub_app - sub-application handle which is used to track pending
 *    requests to the raft backend.
 * @rcsa_rncui:  sub-app identifier - this item must be first in the structure
 *    and raft_client_sub_app_cmp() should not inspect any members other than
 *    it.
 * @rcsa_rtentry:
 */
struct raft_client_sub_app
{
    struct raft_net_client_user_id      rcsa_rncui; //Must be the first memb!
    struct raft_client_interface       *rcsa_rci;
    REF_TREE_ENTRY(raft_client_sub_app) rcsa_rtentry;
    STAILQ_ENTRY(raft_client_sub_app)   rcsa_lentry; // retry queue
    struct raft_client_request_handle   rcsa_rh;
};

static uint64_t
raft_client_sub_app_2_msg_id(const struct raft_client_sub_app *sa)
{
    return (sa && SA_2_RPC(sa)) ?
        SA_2_RPC(sa)->rcrm_msg_id : 0;
}

#define DBG_RAFT_CLIENT_SUB_APP(log_level, sa, fmt, ...)                \
{                                                                       \
    char __uuid_str[UUID_STR_LEN];                                      \
    uuid_unparse(                                                       \
        RAFT_NET_CLIENT_USER_ID_2_UUID(&(sa)->rcsa_rncui, 0, 0),        \
        __uuid_str);                                                    \
    LOG_MSG(                                                            \
        log_level,                                                      \
        "sa@%p %s.%lx.%lx msgid=%lx nr=%zu r=%d %c%c%c%c%c%c e=%d "     \
        fmt,                                                            \
        sa,  __uuid_str,                                                \
        RAFT_NET_CLIENT_USER_ID_2_UINT64(&(sa)->rcsa_rncui, 0, 2),      \
        RAFT_NET_CLIENT_USER_ID_2_UINT64(&(sa)->rcsa_rncui, 0, 3),      \
        raft_client_sub_app_2_msg_id(sa),                               \
        (sa)->rcsa_rh.rcrh_num_sends,                                   \
        (sa)->rcsa_rh.rcrh_blocking      ? 'b' : '-',                   \
        (sa)->rcsa_rh.rcrh_cancel        ? 'c' : '-',                   \
        (sa)->rcsa_rh.rcrh_cb_exec       ? 'e' : '-',                   \
        (sa)->rcsa_rh.rcrh_initializing  ? 'i' : '-',                   \
        (sa)->rcsa_rh.rcrh_ready         ? 'r' : '-',                   \
        (sa)->rcsa_rh.rcrh_sendq         ? 's' : '-',                   \
        (sa)->rcsa_rh.rcrh_error,                                       \
        (sa)->rcsa_rtentry.rbe_ref_cnt, ##__VA_ARGS__);                 \
}

#define DBG_RAFT_CLIENT_SUB_APP_TS(log_level, sa, time_ms, fmt, ...)    \
{                                                                       \
    unsigned long long current_ms = time_ms ? time_ms :                 \
        niova_realtime_coarse_clock_get_msec();                         \
                                                                        \
    DBG_RAFT_CLIENT_SUB_APP(                                            \
        log_level, sa, "sub:la=%llu:%llu"fmt,                           \
        (current_ms - timespec_2_msec(&(sa)->rcsa_rh.rcrh_last_send)),  \
        (current_ms -                                                   \
         timespec_2_msec(&(sa)->rcsa_rh.rcrh_submitted)),               \
        ##__VA_ARGS__);                                                 \
}

#define RAFT_CLIENT_SUB_APP_FATAL_IF(cond, sa, fmt, ...)                \
{                                                                       \
    if (cond)                                                           \
        DBG_RAFT_CLIENT_SUB_APP(LL_FATAL, sa, fmt,  ##__VA_ARGS__);     \
}

static int
raft_client_sub_app_cmp(const struct raft_client_sub_app *a,
                        const struct raft_client_sub_app *b)
{
    return raft_net_client_user_id_cmp(&a->rcsa_rncui, &b->rcsa_rncui);
}

REF_TREE_HEAD(raft_client_sub_app_tree, raft_client_sub_app);
REF_TREE_GENERATE(raft_client_sub_app_tree, raft_client_sub_app, rcsa_rtentry,
                  raft_client_sub_app_cmp);

STAILQ_HEAD(raft_client_sub_app_send_queue, raft_client_sub_app);

struct raft_client_instance
{
    struct thread_ctl                     rci_thr_ctl;
    struct raft_client_sub_app_tree       rci_sub_apps;
    pthread_cond_t                        rci_cond;
    struct raft_instance                 *rci_ri;
    struct raft_client_sub_app_send_queue rci_sendq;
    bool                                  rci_leader_is_viable;
    struct timespec                       rci_last_request_sent;
    struct timespec                       rci_last_request_ackd; // by leader
    struct timespec                       rci_last_msg_recvd;
    niova_atomic32_t                      rci_sub_app_cnt;
    niova_atomic32_t                      rci_msg_id_counter;
    unsigned int                          rci_msg_id_prefix;
    const struct ctl_svc_node            *rci_leader_csn;
    size_t                                rci_leader_alive_cnt;
};

#define RCI_2_MUTEX(rci) &(rci)->rci_sub_apps.mutex

#define RCI_LOCK(rci) niova_mutex_lock(RCI_2_MUTEX(rci))
#define RCI_UNLOCK(rci) niova_mutex_unlock(RCI_2_MUTEX(rci))

static void
raft_client_sub_app_total_dec(struct raft_client_instance *rci)
{
    int total = niova_atomic_dec(&rci->rci_sub_app_cnt);
    NIOVA_ASSERT(total >= 0);
}

static void
raft_client_sub_app_total_inc(struct raft_client_instance *rci)
{
    int total = niova_atomic_inc(&rci->rci_sub_app_cnt);
    NIOVA_ASSERT(total >= 0);
}

static bool
raft_client_sub_app_may_add_new(const struct raft_client_instance *rci)
{
    int total = niova_atomic_read(&rci->rci_sub_app_cnt);
    NIOVA_ASSERT(total >= 0);

    return total >= raftClientSubAppMax ? false : true;
}

static struct raft_client_instance *
raft_client_instance_lookup(raft_client_instance_t instance)
{
    if (!instance)
        return NULL;

    struct raft_client_instance *rci = NULL;

    pthread_mutex_lock(&raftClientMutex);
    for (size_t i = 0; i < RAFT_CLIENT_MAX_INSTANCES; i++)
    {
        if (instance == (void *)raftClientInstances[i])
	{
            rci = raftClientInstances[i];
            break;
        }
    }
    pthread_mutex_unlock(&raftClientMutex);

    return rci;
}

static int
raft_client_instance_release(struct raft_client_instance *rci)
{
    int rc = -ENOENT;

    pthread_mutex_lock(&raftClientMutex);

    for (size_t i = 0; i < RAFT_CLIENT_MAX_INSTANCES; i++)
    {
        if (rci == raftClientInstances[i])
        {
            niova_free(rci);
            raftClientInstances[i] = NULL;
            rc = 0;
            break;
        }
    }

    pthread_mutex_unlock(&raftClientMutex);
    return rc;
}

static struct raft_client_sub_app *
raft_client_sub_app_construct(const struct raft_client_sub_app *in)
{
    if (!in)
        return NULL;

    struct raft_client_sub_app *sa =
        niova_calloc_can_fail((size_t)1, sizeof(struct raft_client_sub_app));

    if (!sa)
	return NULL;

    NIOVA_ASSERT(in->rcsa_rci);

    /* Prevent the timercb thread from inspecting this object until it's
     * initialization is complete.
     */
    sa->rcsa_rh.rcrh_initializing = 1;

    raft_net_client_user_id_copy(&sa->rcsa_rncui, &in->rcsa_rncui);
    sa->rcsa_rci = in->rcsa_rci;

    raft_client_sub_app_total_inc(sa->rcsa_rci);

    DBG_RAFT_CLIENT_SUB_APP(LL_DEBUG, sa, "");

    return sa;
}

static void
raft_client_sub_app_rpc_request_release(struct raft_client_sub_app *sa)
{
    if (SA_2_RPC(sa))
    {
        niova_free(SA_2_RPC(sa));
        SA_2_RPC(sa) = NULL;
    }
}

static int
raft_client_sub_app_destruct(struct raft_client_sub_app *destroy)
{
    if (!destroy)
	return -EINVAL;

    NIOVA_ASSERT(destroy->rcsa_rci);

    DBG_RAFT_CLIENT_SUB_APP(LL_DEBUG, destroy, "");

    RAFT_CLIENT_SUB_APP_FATAL_IF((destroy->rcsa_rh.rcrh_async_cb &&
                                  !destroy->rcsa_rh.rcrh_cb_exec), destroy,
                                 "callback was not issued");

    raft_client_sub_app_rpc_request_release(destroy);

    // RPCs must have been freed by raft_client_sub_app_rpc_request_release()
    NIOVA_ASSERT(SA_2_RPC(destroy) == NULL);

    raft_client_sub_app_total_dec(destroy->rcsa_rci);

    niova_free(destroy);

    return 0;
}

static void
raft_client_sub_app_put(struct raft_client_instance *rci,
                        struct raft_client_sub_app *sa,
                        const char *caller_func, const int caller_lineno)
{
    DBG_RAFT_CLIENT_SUB_APP(LL_DEBUG, sa, "%s:%d", caller_func, caller_lineno);

    NIOVA_ASSERT(rci == sa->rcsa_rci);

    RT_PUT(raft_client_sub_app_tree, &rci->rci_sub_apps, sa);
}

/**
 * raft_client_sub_app_done - called when the sub app processing is no longer
 *    required.  The object may exist after this call until all of if refs
 *    have been put.
 */
static void
raft_client_sub_app_done(struct raft_client_instance *rci,
                         struct raft_client_sub_app *sa,
                         const char *caller_func, const int caller_lineno)
{
    DBG_RAFT_CLIENT_SUB_APP(LL_DEBUG, sa, "%s:%d",
                            caller_func, caller_lineno);

    NIOVA_ASSERT(rci == sa->rcsa_rci);

    raft_client_sub_app_put(rci, sa, caller_func, caller_lineno);
}

static struct raft_client_sub_app *
raft_client_sub_app_lookup(struct raft_client_instance *rci,
                           const struct raft_net_client_user_id *rncui,
                           const char *caller_func, const int caller_lineno)
{
    NIOVA_ASSERT(rci && rncui);

    struct raft_client_sub_app *sa =
        RT_LOOKUP(raft_client_sub_app_tree, &rci->rci_sub_apps,
                  (const struct raft_client_sub_app *)rncui);

    if (sa)
        DBG_RAFT_CLIENT_SUB_APP(LL_DEBUG, sa, "%s:%d",
                                caller_func, caller_lineno);

    return sa;
}

static struct raft_client_sub_app *
raft_client_sub_app_add(struct raft_client_instance *rci,
                        const struct raft_net_client_user_id *rncui,
                        const char *caller_func, const int caller_lineno)
{
    NIOVA_ASSERT(rci && rncui);

    int error = 0;


    struct raft_client_sub_app match;

    raft_net_client_user_id_copy(&match->rcsa_rncui, rncui);
    match.rcsa_rci = rci;

    struct raft_client_sub_app *sa =
        RT_GET_ADD(raft_client_sub_app_tree, &rci->rci_sub_apps, &match,
                   &error);

    if (!sa) // ENOMEM
    {
        LOG_MSG(LL_NOTIFY, "raft_client_sub_app_construct(): %s",
                strerror(-error));

        return NULL;
    }

    DBG_RAFT_CLIENT_SUB_APP((error ? LL_NOTIFY : LL_DEBUG), sa, "%s:%d %s",
                            caller_func, caller_lineno, strerror(-error));

    if (error) // The entry already existed
    {
        raft_client_sub_app_put(rci, sa, __func__, __LINE__);
        return NULL;
    }

    NIOVA_ASSERT(rci == sa->rcsa_rci);

    DBG_RAFT_CLIENT_SUB_APP(LL_DEBUG, sa, "");

    return sa;
}

static void
raft_client_timerfd_settime(struct raft_instance *ri)
{
    raft_net_timerfd_settime(ri, raftClientTimerFDExpireMS);
}

static bool
raft_client_server_target_is_stale(const struct raft_instance *ri,
                                   const uuid_t server_uuid)
{
    unsigned long long recency_ms = 0;

    int rc = raft_net_comm_recency(ri, raft_peer_2_idx(ri, server_uuid),
                                   RAFT_COMM_RECENCY_UNACKED_SEND,
                                   &recency_ms);

    return (rc || recency_ms > raftClientStaleServerTimeMS) ? true : false;
}

static bool
raft_client_ping_target_is_stale(struct raft_instance *ri)
{
    NIOVA_ASSERT(ri);

    return (!ri->ri_csn_leader ||
            raft_client_server_target_is_stale(ri,
                                               ri->ri_csn_leader->csn_uuid)) ?
        true : false;
}

static void
raft_client_set_leader_viability(struct raft_client_instance *rci, bool viable)
{
    rci->rci_leader_is_viable = viable;
}

static bool
raft_client_leader_is_viable(const struct raft_client_instance *rci)
{
    return rci->rci_leader_is_viable;
}

static void
raft_client_rpc_msg_assign_id(struct raft_client_instance *rci,
                              struct raft_client_rpc_msg *rcrm)
{
    NIOVA_ASSERT(rci && rcrm);

    rcrm->rcrm_msg_id = rci->rci_msg_id_prefix;

    rcrm->rcrm_msg_id = (rcrm->rcrm_msg_id << 32 |
                         niova_atomic_inc(&rci->rci_msg_id_counter));
}

static int // may be raft_net_timerfd_cb_ctx_int_t or client-enqueue ctx
raft_client_rpc_msg_init(struct raft_client_instance *rci,
                         struct raft_client_rpc_msg *rcrm,
                         enum raft_client_rpc_msg_type msg_type,
                         uint16_t data_size, struct ctl_svc_node *dest_csn,
                         bool uses_client_entry_data)
{
    if (!rci || !RCI_2_RI(rci) || !RCI_2_RI(rci)->ri_csn_raft || !rcrm ||
        !dest_csn)
        return -EINVAL;

    else if (msg_type != RAFT_CLIENT_RPC_MSG_TYPE_PING &&
             msg_type != RAFT_CLIENT_RPC_MSG_TYPE_REQUEST)
        return -EOPNOTSUPP;

    else if (msg_type == RAFT_CLIENT_RPC_MSG_TYPE_REQUEST &&
             (data_size == 0 ||
              !raft_client_rpc_msg_size_is_valid(data_size,
                                                 uses_client_entry_data)))
        return -EMSGSIZE;

    struct raft_instance *ri = RCI_2_RI(rci);

    memset(rcrm, 0, sizeof(struct raft_client_rpc_msg));

    rcrm->rcrm_type = msg_type;
    rcrm->rcrm_version = 0;
    rcrm->rcrm_data_size = data_size;
    rcrm->rcrm_uses_raft_client_entry_data = uses_client_entry_data ? 1 : 0;

    uuid_copy(rcrm->rcrm_raft_id, ri->ri_csn_raft->csn_uuid);
    uuid_copy(rcrm->rcrm_dest_id, dest_csn->csn_uuid);
    uuid_copy(rcrm->rcrm_sender_id, ri->ri_csn_this_peer->csn_uuid);

    raft_client_rpc_msg_assign_id(rci, rcrm);

    return 0;
}

static int
raft_client_rpc_ping_init(struct raft_client_instance *rci,
                          struct raft_client_rpc_msg *rcrm)
{
    return raft_client_rpc_msg_init(rci, rcrm, RAFT_CLIENT_RPC_MSG_TYPE_PING,
                                    0, RCI_2_RI(rci)->ri_csn_leader, false);
}

/**
 * raft_client_ping_raft_service - send a 'ping' to the raft leader or another
 *    node if our known raft leader is not responsive.  The ping will reply
 *    with application-specific data for this client instance.
 *  NOTE:  raft_client_ping_raft_service() does not queue on the rci sendq.
 */
static raft_net_timerfd_cb_ctx_t
raft_client_ping_raft_service(struct raft_client_instance *rci)
{
    if (!rci || !RCI_2_RI(rci) || !RCI_2_RI(rci)->ri_csn_leader)
        return;

    struct raft_instance *ri = RCI_2_RI(rci);

    DBG_SIMPLE_CTL_SVC_NODE(LL_DEBUG, ri->ri_csn_leader, "");

    struct raft_client_rpc_msg rcrm;

    int rc = raft_client_rpc_ping_init(rci, &rcrm);
    FATAL_IF((rc), "raft_client_rpc_ping_init(): %s", strerror(-rc));

    rc = raft_net_send_client_msg(ri, &rcrm);
    if (rc)
        DBG_RAFT_CLIENT_RPC_LEADER(LL_DEBUG, ri, &rcrm,
                                   "raft_net_send_client_msg() %s",
                                   strerror(-rc));
}

static void
raft_client_set_ping_target(struct raft_client_instance *rci)
{
    NIOVA_ASSERT(rci && RCI_2_RI(rci));

    struct raft_instance *ri = RCI_2_RI(rci);

    if (raft_client_ping_target_is_stale(ri)
    {
        raft_client_set_leader_viability(rci, false);

        const raft_peer_t tgt =
            raft_net_get_most_recently_responsive_server(ri);

        NIOVA_ASSERT(tgt < ctl_svc_node_raft_2_num_members(ri->ri_csn_raft));

        /* Raft leader here is really a guess.  If 'target' is not the raft
         * leader then it should reply with the UUID of the raft leader.
         */
        ri->ri_csn_leader = ri->ri_csn_raft_peers[tgt];
    }
}

/**
 * raft_client_raft_instance_to_client_instance - given a raft_instance
 *    pointer, this function will return the raft_client_instance after a
 *    simple verification process.
 */
static struct raft_client_instance *
raft_client_raft_instance_to_client_instance(struct raft_instance *ri)
{
    NIOVA_ASSERT(ri && ri->ri_client_arg);

    struct raft_client_instance *rci =
        (struct raft_client_instance *)ri->ri_client_arg;

    // This ensures the client instance is still valid.
    NIOVA_ASSERT(rci == raft_client_instance_lookup(ri->ri_client_arg));

    return rci;
}

/**
 * raft_client_request_send_queue_add_locked - adds the sub app to the rci's
 *    send queue. The sa must not have its sendq bit already set.  This
 *    function takes a second reference on the sa since it's pointer is copied
 *    into the queue.  This function is called from timercb and raft_client_app
 *    context.
 */
static void // raft_client_app_ctx_t & raft_net_timerfd_cb_ctx_t
raft_client_request_send_queue_add_locked(struct raft_client_instance *rci,
                                          struct raft_client_sub_app *sa,
                                          const struct timespec *now,
                                          const char *caller_func,
                                          const int caller_lineno)
{
    NIOVA_ASSERT(rci && RCI_2_RI(rci) && sa && !sa->rcsa_rh.rcrh_sendq &&
                 !sa->rcsa_rh.rcrh_initializing);

    sa->rcsa_rh.rcrh_sendq = 1;

    // Take a ref on the 'sa'.
    REF_TREE_REF_GET_ELEM_LOCKED(sa, rcsa_rtentry);

    DBG_RAFT_CLIENT_SUB_APP_TS(LL_DEBUG, sa, (now ? timespec_2_msec(now) : 0),
                               "%s:%d", caller_func, caller_lineno);

    STAILQ_INSERT_TAIL(&rci->rci_sendq, sa, rcsa_lentry);
}

/**
 * raft_client_request_send_queue_remove_prep_locked - prepares the sa for
 *    removal from the send queue.  This function removes the entry but does
 *    not decrement the ref count since a decrement here may cause the object
 *    to destruct.  At this time, the ref tree destructor must take the rt
 *    mutex itself (here, it's already held).  If the object has been canceled
 *    or completed then this function returns -ESTALE and the subsequent call
 *    to raft_client_request_send_queue_remove_done() will likely destruct it.
 *    NOTE:  this function must be proceded with a call to
 *           raft_client_request_send_queue_remove_done().
 */
static int
raft_client_request_send_queue_remove_prep_locked(
    struct raft_client_instance *rci, struct raft_client_sub_app *sa,
    const struct timespec *now, const char *caller_func,
    const int caller_lineno)
{
    NIOVA_ASSERT(rci && sa && now && sa->rcsa_rh.rcrh_sendq);

    struct raft_client_request_handle *rh = &sa->rcsa_rh;

    STAILQ_REMOVE(&rci->rci_sendq, sa, raft_client_sub_app, rcsa_lentry);

    rh->rcrh_sendq = 0;

    int rc = (rh->rcrh_cancel || rh->rcrh_ready  || rh->rcrh_completing) ?
        -ESTALE : 0;

    if (!rc) // RPC will be sent
    {
        rh->rcrh_last_send = *now;
        rh->rcrh_num_sends++;
    }

    DBG_RAFT_CLIENT_SUB_APP((rc ? LL_NOTIFY : LL_DEBUG), sa, "%s:%d %s",
                            caller_func, caller_lineno, strerror(-rc));
    return rc;
}

/**
 * raft_client_request_send_queue_remove_done - removes the send queue
 *    reference.  This call must be issued after
 *    raft_client_request_send_queue_remove_prep_locked() and after the RPC
 *    has been issued to the network, via raft_client_rpc_launch(), if the
 *    call to raft_client_request_send_queue_remove_prep_locked() was
 *    successful.  It is possible that the sa is freed while the RPC is
 *    pending - in this case, the RPC completion (if any) will become a noop.
 */
static void
raft_client_request_send_queue_remove_done(struct raft_client_instance *rci,
                                           struct raft_client_sub_app *sa,
                                           const char *caller_func,
                                           const int caller_lineno)
{
    NIOVA_ASSERT(rci && sa && !sa->rcsa_rh.rcrh_sendq);

    // 'sa' may be destructed here.
    raft_client_sub_app_put(rci, sa, caller_func, caller_lineno);
}


/**
 * raft_client_check_pending_requests - called in timercb context, walks the
 *    tree of 'sa' objects looking for unqueued objects which still await
 *    replies.
 */
static raft_net_timerfd_cb_ctx_t
raft_client_check_pending_requests(struct raft_client_instance *rci)
{
    struct timespec now;
    niova_unstable_coarse_clock(&now);

    RCI_LOCK(rci);

    struct raft_client_sub_app *sa;
    size_t cnt = 0;

    RB_FOREACH(sa, raft_client_sub_app_tree, &rci->rci_sub_apps.rt_head)
    {
        if (!sa->rcsa_rh.rcrh_sendq &&
            !sa->rcsa_rh.rcrh_initializing &&
            (timespec_2_msec(&now) -
             timespec_2_msec(&sa->rcsa_rh.rcrh_last_send) >=
             raftClientRetryTimeoutMS))
        {
            raft_client_request_send_queue_add_locked(rci, sa, &now, __func__,
                                                      __LINE__);

            DBG_RAFT_CLIENT_SUB_APP_TS(LL_NOTIFY, sa, timespec_2_msec(&now),
                                       "cnt=%zu", cnt);
        }
    }

    RCI_UNLOCK(rci);

    if (cnt) // Signal that a request has been queued.
        ev_pipe_notify(RCI_2_RI(rci)->ri_evps[RAFT_CLIENT_EVP_IDX]);
}

/**
 * raft_client_timerfd_cb - callback is executed by the raft internals,
 *    typically after an expiration of raftClientTimerFDExpireMS.  The raft
 *    client uses this mechanism to reissue lingering RPC requests - those
 *    which are pending and have not yet received a reply.   It may optionally
 *    ping the raft backend service as well.
 */
static raft_net_timerfd_cb_ctx_t
raft_client_timerfd_cb(struct raft_instance *ri)
{
    NIOVA_ASSERT(ri);

    struct raft_client_instance *rci =
        raft_client_raft_instance_to_client_instance(ri);

    if (raft_client_leader_is_viable(rci))
    {
        raft_client_check_pending_requests(rci);
    }
    else
    {
        raft_client_set_ping_target(rci);
        raft_client_ping_raft_service(rci);
    }

    raft_client_timerfd_settime(ri);
}

static void
raft_client_msg_internals_init(struct raft_client_instance *rci)
{
    NIOVA_ASSERT(rci && RCI_2_RI(rci) && RCI_2_RI(rci)->ri_csn_this_peer);

    struct raft_instance *ri = RCI_2_RI(rci);

    rci->rci_msg_id_prefix =
        random_create_seed_from_uuid_and_tid(ri->ri_csn_this_peer->csn_uuid);

    niova_atomic_init(&rci->rci_msg_id_counter, 0);
}

static void
raft_client_instance_reset_leader_info(struct raft_client_instance *rci,
                                       bool nullify_leader_csn)
{
    if (!rci)
        return;

    if (nullify_leader_csn)
        rci->rci_leader_csn = NULL;

    rci->rci_leader_alive_cnt = 0;
    rci->rci_leader_is_viable = false;
}

static void
raft_client_instance_progress_leader_info(
    struct raft_client_instance *rci, const struct ctl_svc_node *sender_csn)
{
    if (!rci || (rci->rci_leader_csn && rci->rci_leader_csn != sender_csn))
        return;

    rci->rci_leader_alive_cnt++;
    if (!rci->rci_leader_csn)
        rci->rci_leader_csn = sender_csn;

    if (!rci->rci_leader_is_viable &&
        rci->rci_leader_alive_cnt > raftClientNpingsUntilViable)
        rci->rci_leader_is_viable = true;
}

static raft_net_udp_cb_ctx_t
raft_client_process_ping_reply(struct raft_client_instance *rci,
                               const struct raft_client_rpc_msg *rcrm,
                               const struct ctl_svc_node *sender_csn)
{
    if (!rci || !rcrm || !sender_csn)
        return;

    if (sender_csn != rci->rci_leader_csn)
        raft_client_instance_reset_leader_info(rci, true);

    switch (rcrm->rcrm_sys_error)
    {
    case 0:
        raft_client_instance_progress_leader_info(rci, sender_csn);
        break;
    case -EINPROGRESS: // fall through
    case -EAGAIN:      // fall through
    case -EBUSY:
        raft_client_instance_reset_leader_info(rci, false);
	break;
    case -ENOENT: // fall through
    case -ENOSYS:
        raft_client_instance_reset_leader_info(rci, true); // fall through
    default:
        break;
    }
}

static raft_net_udp_cb_ctx_t
raft_client_update_leader_from_redirect(struct raft_client_instance *rci,
                                        const struct raft_client_rpc_msg *rcrm,
                                        const struct sockaddr_in *from)
{
    if (!rci || !RCI_2_RI(rci) || !rcrm)
        return;

    int rc = raft_net_apply_leader_redirect(RCI_2_RI(rci),
                                            rcrm->rcrm_redirect_id,
                                            raftClientStaleServerTimeMS);

    DBG_RAFT_CLIENT_RPC((rc ? LL_NOTIFY : LL_DEBUG), rcrm, from,
                        "raft_net_apply_leader_redirect(): %s", strerror(-rc));
}

static int
raft_client_rpc_reply_validate(const struct raft_client_rpc_msg *reply)
{
    NIOVA_ASSERT(reply);

    if (!reply->rcrm_uses_raft_client_entry_data)
    {
        DBG_RAFT_CLIENT_RPC(LL_NOTIFY, rcrm, from,
                            "rpc does not use raft_client_entry_data");
        return -EINVAL;
    }
    else if (rcrm->rcrm_data_size <
             sizeof(struct raft_client_rpc_raft_entry_data))
    {
        DBG_RAFT_CLIENT_RPC(LL_NOTIFY, rcrm, from, "data size is too small %u",
                            rcrm->rcrm_data_size);
        return -EMSGSIZE;
    }

    const struct raft_client_rpc_raft_entry_data *rcrred =
        (const struct raft_client_rpc_raft_entry_data *)reply->rcrm_data;

    const uint32_t expected_size =
        (sizeof(struct raft_client_rpc_raft_entry_data) +
         rcrred->rcrred_data_size);

    if (rcrm->rcrm_data_size != expected_size)
    {
        DBG_RAFT_CLIENT_RPC(LL_NOTIFY, rcrm, from,
                            "data size is %u, expected %u",
                            rcrm->rcrm_data_size, expected_size);
        return -EMSGSIZE;
    }

    return 0;
}

static void
raft_client_rpc_msg_raft_entry_data_init(
    struct raft_client_rpc_raft_entry_data *rcrred,
    const struct raft_net_client_user_id *rncui, const char *request,
    const size_t request_size)
{
    NIOVA_ASSERT(rcrred && rncui && request_size);
    NIOVA_ASSERT(raft_client_rpc_msg_size_is_valid(request_size, true));

    rcrred->rcrred_version = 0;
    rcrred->rcrred_data_size = request_size;

    raft_net_client_user_id_copy(rcrred->rcrred_rncui, rncui);

    memcpy(rcrred->rcrred_data, request, request_size);

    NIOVA_CRC_OBJ(rcrred, raft_client_rpc_raft_entry_data, rcrred_crc,
                  request_size);
}

static struct raft_client_rpc_msg *
raft_client_sub_app_rpc_request_new(
    struct raft_client_instance *rci, struct raft_client_sub_app *ra,
    const char *request, const size_t request_size)
{
    if (!raft_client_rpc_msg_size_is_valid(request_size, true))
        return NULL;

    const struct raft_net_client_user_id *rncui = &sa->rcsa_rncui;

    struct raft_client_rpc_msg *sa_req =
        niova_calloc_can_fail(1, raft_client_rpc_msg_size(request_size, true));

    if (!sa_req)
        return -ENOMEM;

    int rc = raft_client_rpc_msg_init(
        RCI_2_RI(rci), sa_req, RAFT_CLIENT_RPC_MSG_TYPE_REQUEST,
        raft_client_rpc_payload_size(request_size, true),
        RCI_2_RI(rci)->ri_csn_leader);

    if (rc)
    {
        LOG_MSG(LL_NOTIFY, "raft_client_rpc_msg_init(): %s", strerror(-rc));
        niova_free(sa_req);

        return rc;
    }

    struct raft_client_rpc_raft_entry_data *rcrred =
        RAFT_NET_MAP_RPC(raft_client_rpc_raft_entry_data, sa_req);

    /* memcpy the request contents into the raft_entry_data portion of the rpc.
     * This copy is really just to simplify retries and the user API wrt
     * blocking / non-blocking requests.  A zero-copy method is possible should
     * it be necessary.
     */
    raft_client_rpc_msg_raft_entry_data_init(rcrred, rncui, request_size);

    DBG_RAFT_CLIENT_RPC_LEADER(LL_DEBUG, RCI_2_RI(rci), sa_req, "rcrred crc=%x");

    sa->rcsa_rh.rcrh_rpc = sa_req;

    return 0;
}

static void
raft_client_request_handle_init(struct raft_client_request_handle *rcrh,
                                char *reply, const size_t reply_size,
                                const struct timespec now,
                                const struct timespec timeout,
                                const bool block,
                                (void)(*cb)(const raft_net_client_user_id *,
                                            void *, char *, size_t, int),
                                void *arg)
{
    NIOVA_ASSERT(rcrh && rcrh->rcrh_initializing);

    memset(rcrh, 0, sizeof(struct raft_client_request_handle));

    rcrh->rcrh_arg = arg;
    rcrh->rcrh_blocking = block ? 1 : 0;
    rcrh->rcrh_reply_buf = reply;
    CONST_OVERRIDE(size_t, rcrh->rcrh_reply_size, reply_size);
    rcrh->rcrh_async_cb = cb;
    rcrh->rcrh_submitted = now;

    if (timespec_has_value(&timeout))
    {
        niova_realtime_clock(&rcrh->rcrh_timeout);
        timespecadd(&rcrh->rcrh_timeout, &timeout, &rcrh->rcrh_timeout);
    }
}

static int
raft_client_sub_app_wait(struct raft_client_instance *rci,
                         struct raft_client_sub_app *sa)
{
    NIOVA_ASSERT(rci && sa && sa->rcsa_rh.rcrh_rpc &&
                 sa->rcsa_rh.rcrh_blocking);

    struct raft_client_request_handle *rcrh = &sa->rcsa_rh;

    int rc = 0;

    if (timespec_has_value(&rcrh->rcrh_timeout))
    {
        rc = NIOVA_TIMEDWAIT_COND((rcrh->rcrh_ready || rcrh->rcrh_cancel),
                                  RCI_2_MUTEX(rci), &rci->rci_cond,
                                  &rcrh->rcrh_timeout);
    }
    else
    {
        NIOVA_WAIT_COND(rcrh->rcrh_ready, RCI_2_MUTEX(rci), &rci->rci_cond);
    }

    return rc;
}

static void
raft_client_sub_app_wake(struct raft_client_instance *rci,
                         struct raft_client_sub_app *sa)
{
    NIOVA_ASSERT(rci && sa && sa->rcsa_rh.rcrh_rpc &&
                 sa->rcsa_rh.rcrh_blocking);

    struct raft_client_request_handle *rcrh = &sa->rcsa_rh;

    NIOVA_SET_COND_AND_WAKE(broadcast, {rcrh->rcrh_ready = 1;},
                            RCI_2_MUTEX(rci), &rci->rci_cond);
}

static raft_client_app_ctx_t
raft_client_sub_app_cancel_pending_req(struct raft_client_instance *rci,
                                       struct raft_client_sub_app *sa,
                                       bool wakeup)
{
    NIOVA_ASSERT(rci && sa && sa->rcsa_rh.rcrh_rpc);

    struct raft_client_request_handle *rcrh = &sa->rcsa_rh;

    // Notifies the app layer and the timercb thread that the req was canceled.


    RCI_LOCK(rci);
    if (rcrh->rcrh_completing) // reply buffer is being accessed, wait
        NIOVA_WAIT_COND_LOCKED((!rcrh->rcrh_completing), RCI_2_MUTEX(rci),
                               &rci->rci_cond);

    NIOVA_ASSERT(!rcrh->rcrh_completing);

    if (!rcrh->rcrh_ready)
    {
        rcrh->rcrh_cancel = 1;
        if (wakeup)
        {
            NIOVA_SET_COND_AND_WAKE_LOCKED(broadcast, {}, &rci->rci_cond);
        }
    }

    RCI_UNLOCK(rci);

    DBG_RAFT_CLIENT_SUB_APP(LL_NOTIFY, sa, "");
}

/**
 * raft_client_request_cancel - cancels a pending request so that
 *    the reply buffer may be reused without interference from request
 *    completion handling.  The user must pass in the correct reply buffer so
 *    that it may be verified against the rncui.
 * @rci:  pointer to the raft client instance.
 * @rncui:  unique identifier object
 * @reply_buf:  the user buffer pointer which should be attached to the pending
 *    request.
 * NOTES:  raft_client_sub_app_cancel_pending_req() may block briefly if the
 *    buffer is currently being accessed by raft_net_udp_cb_ctx_t.
 */
raft_client_app_ctx_int_t
raft_client_request_cancel(struct raft_client_instance *rci,
                           const struct raft_net_client_user_id *rncui,
                           const char *reply_buf)
{
    if (!rci || !rncui || !reply_buf)
        return -EINVAL;

    struct raft_client_sub_app *sa =
        raft_client_sub_app_lookup(rci, sa, __func__, __LINE__);

    if (!sa)
        return -ENOENT;

    if (!sa->rcsa_rh.rcrh_rpc)
        return -EINPROGRESS;

    if (sa->rcsa_rh.rcrh_reply_buf != reply_buf)
        return -ESTALE;

    raft_client_sub_app_cancel_pending_req(rci, sa, true);

    raft_client_sub_app_put(rci, sa, __func__, __LINE__);
    raft_client_sub_app_done(rci, sa, __func__, __LINE__);

    return 0;
}

static raft_client_app_ctx_int_t
raft_client_request_submit_enqueue(struct raft_client_instance *rci,
                                   struct raft_client_sub_app *sa,
                                   const struct timespec *now)
{
    RCI_LOCK(rci);

    NIOVA_ASSERT(sa && sa->rcsa_rh.rcrh_initializing);

    sa->rcsa_rh.rcrh_initializing = 0;

    raft_client_request_send_queue_add_locked(rci, sa, now, __func__,
                                              __LINE__);
    RCI_UNLOCK(rci);

    // Done after the lock is released.
    ev_pipe_notify(&RCI_2_RI(rci)->ri_evps[RAFT_CLIENT_EVP_IDX]);
}

static void
raft_client_async_cb_issue(struct raft_client_instance *rci,
                           struct raft_client_sub_app *sa)
{
    NIOVA_ASSERT(rci && sa);

    struct raft_client_request_handle *rcrh = &sa->rcsa_rh;

    bool should_exec = false;

    RCI_LOCK(rci);
    if (!rcrh->rcrh_cb_exec)
    {
        should_exec = true;
        rcrh->rcrh_cb_exec = 1;
    }
    RCI_UNLOCK(rci);

    // Issue the callback if it was specified
    if (should_exec && rcrh->rcrh_async_cb)
    {
        rcrh->rcrh_async_cb(&sa->rcsa_rncui, rcrh->rcrh_arg,
                            rcrh->rcrh_reply_buf,
                            rcrh->rcrh_reply_used_size,
                            rcrh->rcrh_error);
    }
}

raft_client_app_ctx_int_t
raft_client_request_submit(raft_client_instance_t client_instance,
                           const struct raft_net_client_user_id *rncui,
                           const char *request,
                           const size_t request_size,
                           char *reply, const size_t reply_size,
                           const struct timespec timeout,
                           const bool block,
                           (void)(*cb)(const raft_net_client_user_id *,
                                       void *, char *, size_t, int),
                           void *arg)
{
    if (!client_instance || !rncui || !request || !request_size ||
        request_size > RAFT_NET_CLIENT_MAX_RPC_SIZE || (!block && cb == NULL))
        return -EINVAL;

    struct raft_client_instance *rci =
        raft_client_instance_lookup(client_instance);

    if (!rci)
        return -ENOENT;

    struct raft_client_sub_app *sa = raft_client_sub_app_add(rci, rncui);
    if (!sa)
        return -EALREADY; // Each sub-app may only have 1 outstanding request.

    int rc =
        raft_client_sub_app_rpc_request_new(rci, sa, request, request_size);

    if (rc)
    {
        DBG_RAFT_CLIENT_SUB_APP(LL_NOTIFY, sa,
                                  "raft_client_sub_app_rpc_request_new() %s",
                                  strerror(-rc));

        raft_client_sub_app_put(rci, sa, __func__, __LINE__);
        return -ENOMEM;
    }

    struct raft_client_request_handle *rcrh = &sa->rcsa_rh;

    struct timespec now;
    niova_unstable_coarse_clock(&now);

    raft_client_request_handle_init(rcrh, reply, reply_size, now, timeout,
                                    block, cb, arg);

    // Place the 'sa' onto the sendq and mark that initialization is complete.
    raft_client_request_submit_enqueue(rci, sa, &now);

    if (block)
    {
        rc = raft_client_sub_app_wait(sa);

        if (rc == -ETIMEDOUT)
        {
            raft_client_sub_app_cancel_pending_req(rci, sa, false);

            // If the msg completed after the timeout, unset the rc.
            rc = rci->rcrh_cancel ? rc : 0;

            if (!rcrh->rcrh_error)
                rcrh->rcrh_error = -ETIMEDOUT;
        }

        // Issue the callback if it was specified
        raft_client_async_cb_issue(rci, sa);

        /* Important!  This put may not free the 'sa'.  It's possible that the
         * sa is in the process of being retried and the timercb thread has
         * taken a ref on it.  The sa will not be retried again (since it has
         * been marked as canceled, however, there may be some delay in the
         * timercb thread releasing its reference.
         */
        raft_client_sub_app_done(rci, sa, __func__, __LINE__);
    }

    return rc;
}

static raft_net_udp_cb_ctx_t
raft_client_reply_complete(
    struct raft_client_instance *rci, const uint64_t msg_id, int16_t app_err,
    const struct raft_client_rpc_raft_entry_data *rcrred,
    const struct sockaddr_in *from)
{
    NIOVA_ASSERT(rci && rcrred && from);

    struct raft_client_sub_app *sa =
        raft_client_sub_app_lookup(rci, &rcrred->rcrred_rncui);

    if (!sa)
        return;

    DBG_RAFT_CLIENT_SUB_APP(LL_DEBUG, sa, "");

    if (!sa->rcsa_rh.rcrh_rpc ||
        msg_id != raft_client_sub_app_2_msg_id(sa))
    {
        DBG_RAFT_CLIENT_SUB_APP(
            (sa->rcsa_rh.rcrh_rpc ? LL_NOTIFY : LL_WARN),
            sa, "non matching msg_id=%lx", msg_id);

        raft_client_sub_app_put(rci, sa, __func__, __LINE__);
        return;
    }

    struct raft_client_request_handle *rcrh = &sa->rcsa_rh;

    RCI_LOCK(rci);
    if (rcrh->rcrh_ready)
    {
        RCI_UNLOCK(rci);
        DBG_RAFT_CLIENT_SUB_APP(LL_NOTIFY, sa, "rcrh_ready is already set");
        return;
    }
    else if (rcrh->rcrh_completing)
    {
        RCI_UNLOCK(rci);
        DBG_RAFT_CLIENT_SUB_APP(LL_FATAL, sa,
                                  "rcrh_completing may not be set here");
        return;
    }
    // if the request is canceled then we no longer own the reply buffer
    else if (rcrh->rcrh_cancel)
    {
        if (!rcrh->rcrh_error)
            rcrh->rcrh_error = -ECANCELED;
    }
    else
    {
        rcrh->rcrh_sin_reply_addr = from ? from->sin_addr;
        rcrh->rcrh_sin_reply_port = from ? from->sin_port;
        rcrh->rcrh_reply_used_size = rcrred->rcrred_data_size;
        rcrh->rcrh_error = app_err;

        if (!rcrh->rcrh_error &&
            (rcrh->rcrh_reply_used_size > rcrh->rcrh_reply_size))
            rcrh->rcrh_error = -E2BIG;

        if (!rcrh->rcrh_error && rcrh->rcrh_reply_used_size)
            rcrh->rcrh_completing = 1;

        RCI_UNLOCK(rci);

        if (!rcrh->rcrh_error && rcrh->rcrh_completing)
            memcpy(rcrh->rcrh_reply_buf, rcrred->rcrred_data,
                   rcrh->rcrh_reply_used_size);

        RCI_LOCK(rci);
        rcrh->rcrh_completing = 0;
        rcrh->rcrh_ready = 1;
    }

    RCI_UNLOCK(rci);

    if (rcrh->rcrh_blocking)
        raft_client_sub_app_wake(rci, sa);
    else
        raft_client_async_cb_issue(rci, sa);

    raft_client_sub_app_done(rci, sa, __func__, __LINE__);
}

/**
 * raft_client_udp_recv_handler_process_reply - handler for non-ping replies.
 */
static raft_net_udp_cb_ctx_t
raft_client_udp_recv_handler_process_reply(
    struct raft_client_instance *rci, const struct raft_client_rpc_msg *rcrm,
    const struct ctl_svc_node *sender_csn, const struct sockaddr_in *from)
{
    NIOVA_ASSERT(ri && RCI_2_RI(rci) && rcrm && sender_csn && from);

    if (sender_csn != ri->ri_csn_leader)
    {
        DBG_RAFT_CLIENT_RPC(LL_NOTIFY, rcrm, from, "reply is not from leader");
        return;
    }
    else if (rcrm->rcrm_sys_error)
    {
        DBG_RAFT_CLIENT_RPC(LL_NOTIFY, rcrm, from, "sys-err=%s",
                            strerror(-rcrm->rcrm_sys_error));
        return;
    }
    else if (raft_client_rpc_reply_validate(rcrm))
    {
        return; // RPC contents were deemed invalid
    }

    // raft_client_rpc_reply_validate() has performed the size checks
    const struct raft_client_rpc_raft_entry_data *rcrred =
        (const struct raft_client_rpc_raft_entry_data *)reply->rcrm_data;

    raft_client_reply_complete(rci, rcrm->rcrm_msg_id, rcrm->rcrm_app_error,
                               rcrred, from);
}

/**
 * raft_client_udp_recv_handler - callback which is registered with the raft
 *    net subsystem.  It's issued each time a UDP msg arrives on this node's
 *    listener socket.  raft_client_udp_recv_handler() handles 3 types of
 *    messages at this time:
 *    - RAFT_CLIENT_RPC_MSG_TYPE_PING_REPLY
 *    - RAFT_CLIENT_RPC_MSG_TYPE_REDIRECT
 *    - RAFT_CLIENT_RPC_MSG_TYPE_REPLY
 */
static raft_net_udp_cb_ctx_t
raft_client_udp_recv_handler(struct raft_instance *ri, const char *recv_buffer,
                             ssize_t recv_bytes,
                             const struct sockaddr_in *from)
{
    if (!ri || !ri->ri_csn_leader || !recv_buffer || !recv_bytes || !from ||
        recv_bytes > RAFT_ENTRY_MAX_DATA_SIZE)
	return;

    struct raft_client_instance *rci =
        raft_client_raft_instance_to_client_instance(ri);

    const struct raft_client_rpc_msg *rcrm =
	(const struct raft_client_rpc_msg *)recv_buffer;

    struct ctl_svc_node *sender_csn =
        raft_net_verify_sender_server_msg(ri, rcrm->rcrm_sender_id,
                                          rcrm->rcrm_raft_id, from);
    if (!sender_csn)
        return;

    DBG_RAFT_CLIENT_RPC(
	(rcrm->rcrm_sys_error ? LL_NOTIFY : LL_DEBUG), rcrm, from, "%s",
        rcrm->rcrm_sys_error ?
        // Xxx rcrm_sys_error should replaced here with rcrm_raft_error
        raft_net_client_rpc_sys_error_2_string(rcrm->rcrm_sys_error) : "");

    raft_net_update_last_comm_time(ri, rcrm->rcrm_sender_id, false);

    /* Copy the last_recv timestamp taken above.  This is used to track
     * the liveness of the raft cluster.
     */
    int rc = raft_net_comm_get_last_recv(ri, rcrm->rcrm_sender_id,
                                         &rci->rci_last_msg_recvd);

    FATAL_IF((rc), "raft_net_comm_get_last_recv(): %s", strerror(-rc));

    if (rcrm->rcrm_type == RAFT_CLIENT_RPC_MSG_TYPE_PING_REPLY)
        raft_client_process_ping_reply(rci, rcrm, sender_csn);

    else if (rcrm->rcrm_type == RAFT_CLIENT_RPC_MSG_TYPE_REDIRECT)
        raft_client_update_leader_from_redirect(rci, rcm, from);

    else if (!rcrm->rcrm_sys_error &&
             rcrm->rcrm_type == RAFT_CLIENT_RPC_MSG_TYPE_REPLY)
        raft_client_udp_recv_handler_process_reply(ri, rcrm, sender_csn, from);
}

/**
 * raft_client_rpc_launch - sends non-ping RPCs, which were queued on
 *    rci->rci_sendq, to the raft service.  This call is always performed from
 *    epoll context.
 */
static raft_client_epoll_t
raft_client_rpc_launch(struct raft_client_instance *rci,
                       struct raft_client_sub_app *sa)
{
    NIOVA_ASSERT(rci && sa);
    NIOVA_ASSERT(sa->rcsa_rh.rcrh_rpc);
    NIOVA_ASSERT(!sa->rcsa_rh.rcrh_sendq);

    // Launch the udp msg.
    int rc = raft_net_send_client_msg(rci->rcr_ri,
                                      sa->rcsa_rh.rcrh_rpc);
    if (rc)
    {
        DBG_RAFT_CLIENT_SUB_APP(LL_NOTIFY, sa,
                                  "raft_net_send_client_msg(): %s",
                                  strerror(-rc));

        DBG_RAFT_CLIENT_RPC_LEADER(LL_DEBUG, RCI_2_RI(rci),
                                   &sa->rcsa_rh.rcrh_rpc,
                                   "raft_net_send_client_msg(): %s",
                                   strerror(-rc));
    }
}

/**
 * raft_client_rpc_sendq_dequeue_head_and_send - takes the first 'sa' from the
 *    sendq, removes it from the list, and launches its RPC if the 'sa' still
 *    requires an RPC operation.
 */
static raft_client_epoll_int_t
raft_client_rpc_sendq_dequeue_head_and_send(struct raft_client_instance *rci,
                                            const struct timespec *now)
{
    NIOVA_ASSERT(rci && now);

    int rc = 0;

    RCI_LOCK(rci);
    struct raft_client_sub_app *sa = STAILQ_FIRST(&rci->rci_sendq);

    if (sa)
        rc = raft_client_request_send_queue_remove_prep_locked(
            rci, sa, now, __func__, __LINE__);

    RCI_UNLOCK(rci);

    if (!sa)
        return 0;

    else if (!rc)
        raft_client_rpc_launch(rci, sa);

    // Drop the sendq reference
    raft_client_request_send_queue_remove_done(rci, sa, __func__, __LINE__);

    return rc;
}

/**
 * raft_client_rpc_sender - called from evp / epoll context when an 'sa' object
 *    has been newly placed onto the sendq or the sendq has not been completely
 *    processed.  raft_client_rpc_sender() implements msg throttling on a per-
 *    second granularity.  At this time, all requests, except pings, may be
 *    throttled here.  It may be prudent to differentiate read and write
 *    requests at some point and allow for more targeted policies since it
 *    should be the case that read operations have a lower overhead than
 *    raft writes (which are synchronous and replicated).
 */
static raft_client_epoll_t
raft_client_rpc_sender(struct raft_client_instance *rci, struct ev_pipe *evp)
{
    static struct timespec interval_start;
    static size_t interval_rpc_cnt;

    NIOVA_ASSERT(rci && evp);

    struct timespec now;

    niova_unstable_coarse_clock(&now);

    if (now.tv_sec > interval_start.tv_sec)
    {
        start = now;
        interval_rpc_cnt = 0;
    }

    const ssize_t remaining_rpcs_this_interval =
        raftClientRequestRatePerSec - interval_rpc_cnt;

    LOG_MSG(LL_DEBUG, "remaining_rpcs_this_interval=%zd",
            remaining_rpcs_this_interval);

    if (remaining_rpcs_this_interval <= 0)
        return;

    size_t remaining_sends = MIN(RAFT_CLIENT_RPC_SENDER_MAX,
                                 remaining_rpcs_this_interval);

    while (remaining_sends)
    {
        int rc = raft_client_rpc_sendq_dequeue_head_and_send(rci, &now);
        if (!rc)
            remaining_sends--;
    }

    if (!STAILQ_EMPTY(&rci->rci_sendq))
        ev_pipe_notify(evp); /* Reschedule ourselves if there's remaining
                              * slots in this interval */
}

static raft_client_epoll_t
raft_client_evp_cb(const struct epoll_handle *eph)
{
    NIOVA_ASSERT(eph && eph->eph_arg);

    FUNC_ENTRY(LL_DEBUG);

    struct raft_instance *ri = eph->eph_arg;

    struct ev_pipe *evp = &ri->ri_evps[RAFT_CLIENT_EVP_IDX];

    NIOVA_ASSERT(eph->eph_fd == evp_read_fd_get(evp));

    struct raft_client_instance *rci =
        raft_client_raft_instance_to_client_instance(ri);

    ev_pipe_drain(evp);
    evp_increment_reader_cnt(evp);//Xxx this is a mess
    // should be inside ev_pipe.c!

    raft_client_rpc_sender(rci, evp);
}

static raft_client_thread_t
raft_client_thread(void *arg)
{
    struct thread_ctl *tc = arg;

    SIMPLE_LOG_MSG(LL_DEBUG, "hello");

    struct raft_client_instance *rci =
        (struct raft_client_instance *)thread_ctl_get_arg(tc);

    NIOVA_ASSERT(rci && RCI_2_RI(rci) && rci == RCI_2_RI(rci)->ri_client_arg);

    struct raft_instance *ri = RCI_2_RI(rci);

    // Startup the raft client instance.
    int rc = raft_net_instance_startup(ri, true);
    FATAL_IF((rc), "raft_net_instance_startup(): %s", strerror(-rc));

    int rc = raft_net_evp_add(ri, raft_client_evp_cb);
    FATAL_IF((rc != RAFT_CLIENT_EVP_IDX), "raft_net_evp_add(): %s (idx=%d)",
             strerror(-rc), rc);

    // Called after raft_net_instance_startup() so that ri_csn_this_peer is set
    raft_client_msg_internals_init(rci);

    THREAD_LOOP_WITH_CTL(tc)
    {
        raft_client_timerfd_settime(ri);

        rc = epoll_mgr_wait_and_process_events(&ri->ri_epoll_mgr, -1);
	if (rc == -EINTR)
            rc = 0;

        else if (rc)
            break;
    }

    SIMPLE_LOG_MSG((rc ? LL_WARN : LL_DEBUG), "goodbye (rc=%s)",
                   strerror(-rc));

    return (void *)0;
}

static void
raft_client_instance_init(struct raft_client_instance *rci,
                          struct raft_instance *ri)
{
    REF_TREE_INIT(&rci->rci_sub_apps, raft_client_sub_app_construct,
                  raft_client_sub_app_destruct);

    STAILQ_INIT(&rci->rci_sendq);

    RCI_2_RI(rci) = ri;

    FATAL_IF((pthread_cond_init(&rci->rci_cond, NULL),
              "pthread_cond_init() failed: %s", strerror(errno));
}

static struct raft_client_instance_handle *
raft_client_instance_assign(void)
{
    struct raft_client_instance *rci = NULL;

    pthread_mutex_lock(&raftClientMutex);

    for (size_t i = 0; i < RAFT_CLIENT_MAX_INSTANCES; i++)
    {
        if (raftClientInstances[i] != NULL)
            continue;

        rci = raftClientInstances[i] =
            niova_calloc_can_fail(1, sizeof(struct raft_client_instance));

        if (!rci)
        {
            int rc = errno;
            LOG_MSG(LL_WARN, "calloc failure: %s", strerror(rc));
            return NULL;
        }
    }

    pthread_mutex_unlock(&raftClientMutex);
    return rcih;
}

int
raft_client_init(const char *raft_uuid_str, const char *raft_client_uuid_str,
                 raft_client_instance_t *raft_client_instance)
{
    if (!raft_uuid_str || !raft_client_uuid_str || !raft_client_instance)
        return -EINVAL;

    // Assign an instance before obtaining the raft_instance object.
    struct raft_client_instance *rci = raft_client_instance_assign();
    if (!rci)
        return -ENOSPC;

    struct raft_instance *ri = raft_net_get_instance();
    if (!ri)
    {
        raft_client_destroy((void *)rci);
        return -ENOENT;
    }

    ri->ri_raft_uuid_str = raft_uuid_str;
    ri->ri_this_peer_uuid_str = raft_client_uuid_str;

    NIOVA_ASSERT(!ri->ri_client_arg);

    /* Set this so that raft-instance callbacks, such as
     * raft_client_timerfd_cb, may access the raft_client_instance.
     */
    ri->ri_client_arg = rci;

    raft_client_instance_init(rci, ri);

    raft_net_instance_apply_callbacks(ri, raft_client_timerfd_cb,
                                      raft_client_udp_recv_handler, NULL);

    int rc = thread_create_watched(raft_client_thread, &rci->rci_thr_ctl,
                                   "raft_client", (void *)rci, NULL);

    /* raft_client_thread() does some initializations - wait for these to
     * complete before proceeding.
     */
    thread_creator_wait_until_ctl_loop_reached(&rci->rci_thr_ctl);

    FATAL_IF(rc, "pthread_create(): %s", strerror(errno));

    *raft_client_instance = (void *)rci;

    return 0;
}

int
raft_client_destroy(raft_client_instance_t client_instance)
{
    if (!client_instance)
        return -EINVAL;

    struct raft_client_instance *rci =
        raft_client_instance_lookup(client_instance);

    if (!rci)
        return -ENOENT;

    int rc = thread_halt_and_destroy(&rci->rci_thr_ctl);

    // Don't reuse the instance slot if the thread destruction has failed.
    return rc ? rc : raft_client_instance_release(rci);
}