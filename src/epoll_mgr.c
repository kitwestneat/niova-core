/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2019
 */

#include <sys/epoll.h>
#include <unistd.h>

#include "common.h"
#include "ctor.h"
#include "env.h"
#include "epoll_mgr.h"
#include "log.h"
#include "registry.h"

REGISTRY_ENTRY_FILE_GENERATE;

typedef int epoll_mgr_thread_ctx_int_t;

static size_t epollMgrNumEvents = EPOLL_MGR_DEF_EVENTS;
static pthread_mutex_t epollMgrInstallLock = PTHREAD_MUTEX_INITIALIZER;

int
epoll_mgr_setup(struct epoll_mgr *epm)
{
    if (!epm)
        return -EINVAL;

    pthread_mutex_lock(&epollMgrInstallLock);

    if (epm->epm_ready)
    {
        pthread_mutex_unlock(&epollMgrInstallLock);
        return -EALREADY;
    }

    pthread_mutex_init(&epm->epm_mutex, NULL);

    epm->epm_epfd = epoll_create1(0);
    if (epm->epm_epfd < 0)
    {
        pthread_mutex_unlock(&epollMgrInstallLock);
        return -errno;
    }

    CIRCLEQ_INIT(&epm->epm_active_list);
    CIRCLEQ_INIT(&epm->epm_destroy_list);

    epm->epm_ready = 1;

    pthread_mutex_unlock(&epollMgrInstallLock);
    return 0;
}

int
epoll_mgr_close(struct epoll_mgr *epm)
{
    if (!epm || !epm->epm_ready)
        return -EINVAL;

    else if (epm->epm_epfd < 0)
        return -EBADF;

    // Xxx check for attached handles?
    pthread_mutex_destroy(&epm->epm_mutex);

    epm->epm_ready = 0;

    return close(epm->epm_epfd);
}

int
epoll_handle_init(struct epoll_handle *eph, int fd, int events,
                  void (*cb)(const struct epoll_handle *), void *arg)
{
    if (!eph || !cb)
        return -EINVAL;

    else if (fd < 0)
        return -EBADF;

    eph->eph_installed = 0;
    eph->eph_fd        = fd;
    eph->eph_events    = events;
    eph->eph_cb        = cb;
    eph->eph_arg       = arg;
    eph->eph_ref_cb = NULL;

    return 0;
}

int
epoll_handle_add(struct epoll_mgr *epm, struct epoll_handle *eph)
{
    if (!epm || !eph || !eph->eph_cb || !epm->epm_ready)
        return -EINVAL;

    else if (eph->eph_fd < 0 || epm->epm_epfd < 0)
        return -EBADF;

    else if (eph->eph_installed || eph->eph_installing)
        return -EALREADY;

    if (eph->eph_ref_cb) // take user ref in advance of handle install
        eph->eph_ref_cb(eph->eph_arg, true);

    pthread_mutex_lock(&epm->epm_mutex);
    NIOVA_ASSERT(epm->epm_num_handles >= 0);

    CIRCLEQ_INSERT_HEAD(&epm->epm_active_list, eph, eph_lentry);
    eph->eph_installing = 1;
    pthread_mutex_unlock(&epm->epm_mutex);

    struct epoll_event ev = {.events = eph->eph_events, .data.ptr = eph};

    int rc = epoll_ctl(epm->epm_epfd, EPOLL_CTL_ADD, eph->eph_fd, &ev);

    pthread_mutex_lock(&epm->epm_mutex);
    eph->eph_installing = 0;

    if (rc) // 'installing' bit prevents removal from the active list
    {
        CIRCLEQ_REMOVE(&epm->epm_active_list, eph, eph_lentry);
    }
    else
    {
        eph->eph_installed = 1;
        epm->epm_num_handles++;
    }

    pthread_mutex_unlock(&epm->epm_mutex);

    if (rc && eph->eph_ref_cb) // release user ref if there was a problem
        eph->eph_ref_cb(eph->eph_arg, false);

    return rc;
}

static epoll_mgr_thread_ctx_int_t
epoll_handle_del_complete(struct epoll_mgr *epm, struct epoll_handle *eph)
{
    if (!epm || !eph || !epm->epm_ready)
        return -EINVAL;

    else if (epm->epm_epfd < 0 || eph->eph_fd < 0)
        return -EBADF;

    // It's still 'installed' since it's in the epoll set
    else if (!eph->eph_installed || !eph->eph_destroying)
        return -EAGAIN;

    if (epm->epm_thread_id == pthread_self())
        NIOVA_ASSERT(!eph->eph_ref_cb);

    struct epoll_event ev = {.events = 0, .data.fd = -1};

    int rc = epoll_ctl(epm->epm_epfd, EPOLL_CTL_DEL, eph->eph_fd, &ev);
    if (rc)
    {
        rc = -errno;
        LOG_MSG(LL_WARN, "epoll_ctl(fd=%d, EPOLL_CTL_DEL): %s",
                epm->epm_epfd, strerror(-rc));
    }

    pthread_mutex_lock(&epm->epm_mutex);
    NIOVA_ASSERT(epm->epm_num_handles > 0);
    epm->epm_num_handles--;
    pthread_mutex_unlock(&epm->epm_mutex);

    eph->eph_installed = 0;

    if (eph->eph_ref_cb)
        eph->eph_ref_cb(eph->eph_arg, false);

    return rc;
}

int
epoll_handle_del(struct epoll_mgr *epm, struct epoll_handle *eph)
{
    if (!epm || !eph || !epm->epm_ready)
        return -EINVAL;

    else if (epm->epm_epfd < 0 || eph->eph_fd < 0)
        return -EBADF;

    else if (!eph->eph_installed)
        return -EAGAIN;

    struct epoll_handle *tmp;
    bool found = false;
    pthread_t tid = epm->epm_thread_id;
    bool complete_here = false;

    pthread_mutex_lock(&epm->epm_mutex);
    CIRCLEQ_FOREACH(tmp, &epm->epm_active_list, eph_lentry)
    {
        if (tmp == eph)
        {
            found = true;
            break;
        }
    }

    if (found)
    {
        // Signify that the 'eph' is being placed onto the destroy list
        eph->eph_destroying = 1;
        CIRCLEQ_REMOVE(&epm->epm_active_list, eph, eph_lentry);

        if (!eph->eph_ref_cb || epm->epm_thread_id == pthread_self())
            complete_here = true;
    }

    pthread_mutex_unlock(&epm->epm_mutex);

    int rc = found ? 0 : -ENOENT;

    if (complete_here)
        rc = epoll_handle_del_complete(epm, eph);

    else // Wake up the epoll-mgr thread blocked in epoll_wait()
        thread_issue_sig_alarm_to_thread(tid);

    return rc;
}

static void
epoll_mgr_reap_destroy_list(struct epoll_mgr *epm)
{
    struct epoll_handle *destroy = NULL;

    if (!CIRCLEQ_EMPTY(&epm->epm_destroy_list))
        do
        {
            pthread_mutex_lock(&epm->epm_mutex);

            destroy = CIRCLEQ_EMPTY(&epm->epm_destroy_list) ? NULL :
                CIRCLEQ_FIRST(&epm->epm_destroy_list);

            if (destroy)
                CIRCLEQ_REMOVE(&epm->epm_destroy_list, destroy, eph_lentry);

            pthread_mutex_unlock(&epm->epm_mutex);

            if (destroy)
            {
                int rc = epoll_handle_del_complete(epm, destroy);
                if (rc)
                    LOG_MSG(LL_WARN,
                            "epoll_handle_del_complete(eph=%p): %s",
                            destroy, strerror(-rc));
            }

        } while (destroy);
}

int
epoll_mgr_wait_and_process_events(struct epoll_mgr *epm, int timeout)
{
    if (!epm || !epm->epm_ready)
        return -EINVAL;

    if (!epm->epm_thread_id)
        epm->epm_thread_id = pthread_self();
    else
        NIOVA_ASSERT(epm->epm_thread_id == pthread_self());

    int maxevents = MAX(1, MIN(epollMgrNumEvents, epm->epm_num_handles));

    struct epoll_event evs[maxevents];

    const int nevents =
        epoll_wait(epm->epm_epfd, evs, maxevents, timeout);

    const int rc = -errno;

    if (nevents > 0)
    {
        for (int i = 0; i < nevents; i++)
        {
            struct epoll_handle *eph = evs[i].data.ptr;

            if (eph->eph_installed && eph->eph_cb)
                eph->eph_cb(eph);
        }
    }

    epoll_mgr_reap_destroy_list(epm);

    return nevents < 0 ? rc : nevents;
}

void
epoll_mgr_env_var_cb(const struct niova_env_var *nev)
{
    if (nev && nev->nev_present)
        epollMgrNumEvents = nev->nev_long_value;
}
