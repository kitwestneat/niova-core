/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */

#include "tree.h"
#include "lock.h"
#include "log.h"
#include "vblkdev_handle.h"

static inline int
vbh_tree_cmp(const struct vblkdev_handle *left,
             const struct vblkdev_handle *right)
{
    int i;
    for (i = 0; i < VBLKDEV_ID_WORDS; i++)
    {
        if (left->vbh_id.vdb_id[i] < right->vbh_id.vdb_id[i])
            return -1;
        else if (left->vbh_id.vdb_id[i] > right->vbh_id.vdb_id[i])
            return 1;
    }
    return 0;
}

static inline int
vbch_tree_cmp(const struct vblkdev_chunk_handle *left,
              const struct vblkdev_chunk_handle *right)
{
    if (left->vbch_id < right->vbch_id)
        return -1;
    else if (left->vbch_id > right->vbch_id)
        return 1;

    return 0;
}

RB_GENERATE(vblkdev_handle_tree, vblkdev_handle, vbh_tenry, vbh_tree_cmp);
RB_GENERATE(vblkdev_chunk_handle_tree, vblkdev_chunk_handle, vbch_tenry,
            vbch_tree_cmp);

struct vblkdev_handle_tree vbhTree;
spinlock_t                 vbhSubsysLock;
ssize_t                    vbhNumHandles;
bool                       vbhInitialized = false;

#define VBH_LOCK   spinlock_lock(&vbhSubsysLock)
#define VBH_UNLOCK spinlock_unlock(&vbhSubsysLock)

static void
vbh_num_handles_inc(void)
{
    NIOVA_ASSERT(vbhNumHandles >= 0)
    vbhNumHandles++;
}

static void
vbh_num_handles_dec(void)
{
    NIOVA_ASSERT(vbhNumHandles > 0)
    vbhNumHandles--;
}

static struct vblkdev_handle *
vbh_new(void)
{
    struct vblkdev_handle *vbh = niova_malloc(sizeof(struct vblkdev_handle));

    return vbh;
}


static void
vbh_init(struct vblkdev_handle *vbh, const vblkdev_id_t vbh_id)
{
    vbh->vbh_id = vbh_id;
    vbh->vbh_ref = 0;
    spinlock_init(&vbh->vbh_lock);
    RB_INIT(&vbh->vbh_chunk_handle_tree);

    DBG_VBLKDEV_HNDL(LL_DEBUG, vbh, "");
}

static void
vbh_destroy(struct vblkdev_handle *vbh)
{
    DBG_VBLKDEV_HNDL(LL_DEBUG, vbh, "");

    NIOVA_ASSERT(!vbh->vbh_ref);
    NIOVA_ASSERT(RB_EMPTY(&vbh->vbh_chunk_handle_tree));
    spinlock_destroy(&vbh->vbh_lock);
    niova_free(vbh);
}

static struct vblkdev_handle *
vbh_lookup_locked(const vblkdev_id_t vbh_id)
{
    struct vblkdev_handle *vbh = RB_FIND(vblkdev_handle_tree, &vbhTree,
                                         (struct vblkdev_handle *)&vbh_id);
    if (vbh)
    {
        NIOVA_ASSERT(vbh->vbh_ref > 0);
        vbh->vbh_ref++;
    }

    return vbh;
}

static struct vblkdev_handle *
vbh_add(const vblkdev_id_t vbh_id)
{
    struct vblkdev_handle *vbh = vbh_new();
    if (!vbh)
        return NULL;

    vbh_init(vbh, vbh_id);

    struct vblkdev_handle *vbh_already;

    VBH_LOCK;

    vbh_already = RB_INSERT(vblkdev_handle_tree, &vbhTree, vbh);
    if (!vbh_already)
        vbh->vbh_ref = 1;

    vbh_num_handles_inc();

    VBH_UNLOCK;

    if (vbh_already) // The vbh has already been added
    {
        vbh_destroy(vbh);
        vbh = vbh_already;
    }

    return vbh;
}

void
vbh_put(struct vblkdev_handle *vbh)
{
    DBG_VBLKDEV_HNDL(LL_DEBUG, vbh, "");

    bool destroy = false;

    VBH_LOCK;

    vbh->vbh_ref--;
    NIOVA_ASSERT(vbh->vbh_ref >= 0);

    if (!vbh->vbh_ref)
    {
        destroy = true;
        struct vblkdev_handle *removed =
            RB_REMOVE(vblkdev_handle_tree, &vbhTree, vbh);

        NIOVA_ASSERT(removed == vbh);

        vbh_num_handles_dec();
    }

    VBH_UNLOCK;

    if (destroy)
        vbh_destroy(vbh);
}

struct vblkdev_handle *
vbh_get(const vblkdev_id_t vbh_id, const bool add)
{
    struct vblkdev_handle *vbh;

    VBH_LOCK;
    vbh = vbh_lookup_locked(vbh_id);
    VBH_UNLOCK;

    bool was_added = false;
    if (!vbh && add)
    {
        vbh = vbh_add(vbh_id);
        if (vbh)
            was_added = true;
    }

    if (vbh)
        DBG_VBLKDEV_HNDL(LL_DEBUG, vbh, "added=%d", was_added);

    return vbh;
}

void
vbh_subsystem_init(void)
{
    NIOVA_ASSERT(!vbhInitialized);
    RB_INIT(&vbhTree);
    spinlock_init(&vbhSubsysLock);
    vbhNumHandles = 0;
    vbhInitialized = true;

    LOG_MSG(LL_DEBUG, "done");
}

void
vbh_subsystem_destroy(void)
{
    NIOVA_ASSERT(vbhInitialized);
    NIOVA_ASSERT(!vbhNumHandles);
    spinlock_destroy(&vbhSubsysLock);
    vbhInitialized = false;

    LOG_MSG(LL_DEBUG, "done");
}
