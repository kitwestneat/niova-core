/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2020
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h> // Must precede dirent.h
#ifndef __USE_GNU // scandirat
#define __USE_GNU
#endif
#include <dirent.h>
#include <regex.h>
#include <unistd.h>

#include <rocksdb/c.h>

#include "alloc.h"
#include "common.h"
#include "file_util.h"
#include "log.h"
#include "raft.h"
#include "raft_server_backend_rocksdb.h"
#include "regex_defines.h"
#include "registry.h"

#define RAFT_ROCKSDB_KEY_LEN_MAX 256UL

#define RAFT_LOG_HEADER_ROCKSDB "a0_hdr."
#define RAFT_LOG_HEADER_ROCKSDB_STRLEN 7
#define RAFT_LOG_HEADER_FMT RAFT_LOG_HEADER_ROCKSDB"%s__%s"

#define RAFT_LOG_HEADER_ROCKSDB_END "a1_hdr."
#define RAFT_LOG_HEADER_ROCKSDB_END_STRLEN 7

#define RAFT_LOG_HEADER_ROCKSDB_LAST_SYNC       \
    RAFT_LOG_HEADER_ROCKSDB_END"last_sync"
#define RAFT_LOG_HEADER_ROCKSDB_LAST_SYNC_STRLEN 16

#define RAFT_LOG_HEADER_LAST_APPLIED_ROCKSDB    \
    RAFT_LOG_HEADER_ROCKSDB_END"last_applied"
#define RAFT_LOG_HEADER_LAST_APPLIED_ROCKSDB_STRLEN 19

#define RAFT_LOG_HEADER_UUID RAFT_LOG_HEADER_ROCKSDB_END"UUID"
#define RAFT_LOG_HEADER_UUID_STRLEN 11

#define RAFT_LOG_LASTENTRY_ROCKSDB "z0_last."
#define RAFT_LOG_LASTENTRY_ROCKSDB_STRLEN 8
#define RAFT_LOG_LASTENTRY_FMT RAFT_LOG_LASTENTRY_ROCKSDB"%s__%s"

#define RAFT_HEADER_ENTRY_KEY_FMT "%016zuh"
#define RAFT_ENTRY_KEY_FMT        "%016zue"

#define RAFT_ENTRY_KEY_PREFIX_ROCKSDB "e0."
#define RAFT_ENTRY_KEY_PREFIX_ROCKSDB_STRLEN 3
#define RAFT_ENTRY_KEY_PRINTF RAFT_ENTRY_KEY_PREFIX_ROCKSDB RAFT_ENTRY_KEY_FMT

#define RAFT_ENTRY_HEADER_KEY_PREFIX_ROCKSDB RAFT_ENTRY_KEY_PREFIX_ROCKSDB
#define RAFT_ENTRY_HEADER_KEY_PREFIX_ROCKSDB_STRLEN \
    RAFT_ENTRY_KEY_PREFIX_ROCKSDB_STRLEN
#define RAFT_ENTRY_HEADER_KEY_PRINTF \
    RAFT_ENTRY_KEY_PREFIX_ROCKSDB RAFT_HEADER_ENTRY_KEY_FMT

/* The recovery marker filename will appear as
 * ".recovery_marker.<peer-uuid>_<db-uuid>"
 */
#define RECOVERY_MARKER_NAME "recovery_marker"
#define RECOVERY_MARKER_REGEX \
    "^\\."RECOVERY_MARKER_NAME"\\."UUID_REGEX_BASE"_"UUID_REGEX_BASE"$"
#define RECOVERY_MARKER_NAME_LEN_WITH_PERIODS 17

static regex_t recovery_marker_regex;

REGISTRY_ENTRY_FILE_GENERATE;

struct raft_instance_rocks_db
{
    int                                  rir_log_fd; //dirfd to ri->ri_log
    rocksdb_t                           *rir_db;
    rocksdb_options_t                   *rir_options;
    rocksdb_writeoptions_t              *rir_writeoptions_sync;
    rocksdb_writeoptions_t              *rir_writeoptions_async;
    rocksdb_readoptions_t               *rir_readoptions;
    rocksdb_writebatch_t                *rir_writebatch;
    struct raft_server_rocksdb_cf_table *rir_cf_table;
};

static void
rsbr_entry_write(struct raft_instance *, const struct raft_entry *,
                 const struct raft_net_sm_write_supplements *);

static ssize_t
rsbr_entry_read(struct raft_instance *, struct raft_entry *);

static int
rsbr_entry_header_read(struct raft_instance *, struct raft_entry_header *);

static void
rsbr_log_truncate(struct raft_instance *, const raft_entry_idx_t);

static void // runs in checkpoint thread context
rsbr_log_reap(struct raft_instance *, const raft_entry_idx_t);

static int
rsbr_header_load(struct raft_instance *);

static int
rsbr_header_write(struct raft_instance *);

static int
rsbr_setup(struct raft_instance *);

static int
rsbr_destroy(struct raft_instance *);

static void
rsbr_sm_apply_opt(struct raft_instance *,
                  const struct raft_net_sm_write_supplements *);

static int
rsbr_sync(struct raft_instance *);

static int64_t
rsbr_checkpoint(struct raft_instance *);

static int
rsbr_bulk_recover(struct raft_instance *);

static struct raft_instance_backend ribRocksDB = {
    .rib_entry_write        = rsbr_entry_write,
    .rib_entry_read         = rsbr_entry_read,
    .rib_entry_header_read  = rsbr_entry_header_read,
    .rib_log_truncate       = rsbr_log_truncate,
    .rib_log_reap           = rsbr_log_reap,
    .rib_header_write       = rsbr_header_write,
    .rib_header_load        = rsbr_header_load,
    .rib_backend_setup      = rsbr_setup,
    .rib_backend_shutdown   = rsbr_destroy,
    .rib_backend_checkpoint = rsbr_checkpoint,
    .rib_backend_recover    = rsbr_bulk_recover,
    .rib_sm_apply_opt       = rsbr_sm_apply_opt,
    .rib_backend_sync       = rsbr_sync,
};

enum raft_instance_rocks_db_subdirs
{
    RIR_SUBDIR_DB = 0,
    RIR_SUBDIR_CHKPT_ROOT,
    RIR_SUBDIR_CHKPT_SELF,
    RIR_SUBDIR_CHKPT_PEERS,
    RIR_SUBDIR_TRASH,
    RIR_SUBDIR__MAX,
    RIR_SUBDIR__MIN = RIR_SUBDIR_DB,
};

static const char * ribSubDirs[] = {
    "db", "chkpt", "chkpt/self", "chkpt/peers", "trash",
};

static inline struct raft_instance_rocks_db *
rsbr_ri_to_rirdb(struct raft_instance *ri)
{
    NIOVA_ASSERT(ri && ri->ri_backend == &ribRocksDB && ri->ri_backend_arg);

    struct raft_instance_rocks_db *rir =
        (struct raft_instance_rocks_db *)ri->ri_backend_arg;

    NIOVA_ASSERT(rir->rir_writeoptions_sync && rir->rir_writeoptions_async &&
                 rir->rir_writebatch && rir->rir_readoptions);

    return rir;
}

static int
rsbr_move_item_to_trash(struct raft_instance *ri, const char *path)
{
    if (!ri || !path || !ri->ri_backend_arg)
        return -EINVAL;

    struct raft_instance_rocks_db *rir =
        (struct raft_instance_rocks_db *)ri->ri_backend_arg;

    if (rir->rir_log_fd < 0)
        return -EBADF;

    uuid_t dir_name_uuid;
    uuid_generate_time(dir_name_uuid);
    DECLARE_AND_INIT_UUID_STR(dir_name, dir_name_uuid);

    char tmp_path[PATH_MAX + 1];
    int rc = snprintf(tmp_path, PATH_MAX, "%s/%s",
                      ribSubDirs[RIR_SUBDIR_TRASH], dir_name);

    if (rc > PATH_MAX)
        return -ENAMETOOLONG;

    // Make a dir to hold the trash item to avoid name conflicts
    rc = mkdirat(rir->rir_log_fd, tmp_path, 0750);
    if (rc)
    {
        rc = -errno;
        SIMPLE_LOG_MSG(LL_ERROR, "mkdirat(`%s'): %s", tmp_path, strerror(-rc));
        return rc;
    }

    rc = snprintf(tmp_path, PATH_MAX, "%s/%s/%s",
                  ribSubDirs[RIR_SUBDIR_TRASH], dir_name, path);

    if (rc > PATH_MAX)
        return -ENAMETOOLONG;

    // renameat() handles case where 'path' is absolute
    rc = renameat(rir->rir_log_fd, path, rir->rir_log_fd, tmp_path);
    if (rc)
    {
        rc = -errno;
        SIMPLE_LOG_MSG(LL_ERROR, "renameat(`%s' -> `%s'): %s", path, tmp_path,
                       strerror(-rc));
        return rc;
    }

    LOG_MSG(LL_NOTIFY, "path=%s moved to trash", path);

    return 0;
}

static inline struct rocksdb_t *
rsbr_ri_to_rocksdb(struct raft_instance *ri)
{
    return rsbr_ri_to_rirdb(ri)->rir_db;
}

static rocksdb_iterator_t *
rsbr_create_iterator(struct raft_instance_rocks_db *rir)
{
    if (!rir || !rir->rir_db || !rir->rir_readoptions)
        return NULL;

    rocksdb_iterator_t *iter =
        rocksdb_create_iterator(rir->rir_db, rir->rir_readoptions);

    if (!iter)
        return NULL;

    if (rocksdb_iter_valid(iter)) // The iterator should *not* yet be valid
    {
        rocksdb_iter_destroy(iter);
        return NULL;
    }

    return iter;
}

static int
rsbr_iter_check_error(rocksdb_iterator_t *iter, bool expect_valid)
{
    char *err = NULL;

    rocksdb_iter_get_error(iter, &err);
    if (err)
        return -EIO;

    if (!!rocksdb_iter_valid(iter) == !!expect_valid)
        return 0;

    return expect_valid ? -ENOENT : -EEXIST;
}

static int
rsbr_iter_seek(rocksdb_iterator_t *iter, const char *seek_str,
               size_t seek_strlen, bool expect_valid)
{
    if (!iter || !seek_str || !seek_strlen)
        return -EINVAL;

    rocksdb_iter_seek(iter, seek_str, seek_strlen);

    return rsbr_iter_check_error(iter, expect_valid);
}

static int
rsbr_iter_next_or_prev(rocksdb_iterator_t *iter, bool expect_valid,
                       bool next_or_prev)
{
    if (!iter)
        return -EINVAL;

    next_or_prev ? rocksdb_iter_next(iter) : rocksdb_iter_prev(iter);

    return rsbr_iter_check_error(iter, expect_valid);
}

static bool
rsbr_string_matches_iter_key(const char *str, size_t str_len,
                             rocksdb_iterator_t *iter, bool exact_len)
{
    size_t iter_key_len = 0;
    const char *iter_key = rocksdb_iter_key(iter, &iter_key_len);

    if (!iter_key)
    {
        SIMPLE_LOG_MSG(LL_ERROR, "rocksdb_iter_key(): returns NULL");
        return false;
    }

    SIMPLE_LOG_MSG(LL_DEBUG, "match key='%s', found key='%.*s'",
                   str, (int)iter_key_len, iter_key);

    if ((exact_len && str_len != iter_key_len) ||
        strncmp(str, iter_key, str_len))
    {
        LOG_MSG(LL_NOTIFY, "expected key='%s', got key='%.*s'",
                str, (int)iter_key_len, iter_key);

        return false;
    }

    return true;
}

static void
rsbr_write_supplements_put(const struct raft_net_sm_write_supplements *ws,
                           rocksdb_writebatch_t *wb)
{
    if (!ws || !wb)
        return;

    for (size_t i = 0; i < ws->rnsws_nitems; i++)
    {
        const struct raft_net_wr_supp *supp = &ws->rnsws_ws[i];
        supp->rnws_handle ?
        rocksdb_writebatch_putv_cf(
            wb, (rocksdb_column_family_handle_t *)supp->rnws_handle,
            supp->rnws_nkv, (const char * const *)supp->rnws_keys,
            supp->rnws_key_sizes, supp->rnws_nkv,
            (const char * const *)supp->rnws_values,
            supp->rnws_value_sizes)
        :
        rocksdb_writebatch_putv(wb, supp->rnws_nkv,
                                (const char * const *)supp->rnws_keys,
                                supp->rnws_key_sizes, supp->rnws_nkv,
                                (const char * const *)supp->rnws_values,
                                supp->rnws_value_sizes);
    }
}

static void
rsb_sm_apply_add_last_applied_kv(struct raft_instance_rocks_db *rir,
                                 const raft_entry_idx_t apply_idx,
                                 uint64_t apply_cumu_crc)
{
    NIOVA_ASSERT(rir);

    uint64_t vals[2] = {apply_idx, apply_cumu_crc};

    rocksdb_writebatch_put(rir->rir_writebatch,
                           RAFT_LOG_HEADER_LAST_APPLIED_ROCKSDB,
                           RAFT_LOG_HEADER_LAST_APPLIED_ROCKSDB_STRLEN,
                           (const char *)vals, sizeof(uint64_t) * 2);
}

static int
rsbr_get_exact_val_size(struct raft_instance_rocks_db *rir,
                        const char *key, size_t key_len,
                        void *value, size_t expected_value_len);

static void
rsb_sm_get_last_applied_kv_idx(struct raft_instance *ri)
{
    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);
    uint64_t vals[2] = {0};

    int rc =
        rsbr_get_exact_val_size(rir, RAFT_LOG_HEADER_LAST_APPLIED_ROCKSDB,
                                RAFT_LOG_HEADER_LAST_APPLIED_ROCKSDB_STRLEN,
                                (void *)vals, sizeof(uint64_t) * 2);
    if (rc)
    {
        DBG_RAFT_INSTANCE((rc == -ENOENT ? LL_NOTIFY : LL_ERROR), ri,
                          "failed: %s", strerror(-rc));
    }
    else
    {
        DBG_RAFT_INSTANCE(LL_WARN, ri, "rsbr-last-applied-idx=%ld crc=%x",
                          vals[0], (crc32_t)vals[1]);

        raft_server_backend_setup_last_applied(ri, (raft_entry_idx_t)vals[0],
                                               (crc32_t)vals[1]);
    }
}

static void
rsb_sm_get_instance_uuid(struct raft_instance *ri)
{
    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    uuid_t instance_uuid = {0};

    int rc = rsbr_get_exact_val_size(rir, RAFT_LOG_HEADER_UUID,
                                     RAFT_LOG_HEADER_UUID_STRLEN,
                                     (char *)instance_uuid, sizeof(uuid_t));

    DBG_RAFT_INSTANCE_FATAL_IF(rc, ri, "rsbr_get_exact_val_size(): %s",
                               strerror(-rc));

    uuid_copy(ri->ri_db_uuid, instance_uuid);
}

static void
rsbr_sm_apply_opt(struct raft_instance *ri,
                  const struct raft_net_sm_write_supplements *ws)
{
    NIOVA_ASSERT(ri);
    if (!ws)
        return;

    DBG_RAFT_INSTANCE(LL_NOTIFY, ri, "idx=%ld cumu-crc=%x",
                      ri->ri_last_applied_idx,
                      ri->ri_last_applied_cumulative_crc);

    const uint64_t la_crc = ri->ri_last_applied_cumulative_crc;
    const raft_entry_idx_t la_idx = ri->ri_last_applied_idx;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    rocksdb_writebatch_clear(rir->rir_writebatch);

    rsb_sm_apply_add_last_applied_kv(rir, la_idx, la_crc);

    // Attach any supplemental writes to the rocksdb-writebatch
    rsbr_write_supplements_put(ws, rir->rir_writebatch);

    char *err = NULL;

    /* Apply_opt does not force a sync of the WAL, this is because in the case
     * of a failure, the raft entry will be re-applied.
     * The api may need to accept the write options from the SM at some point,
     * however, the sync WAL option generally be avoided here.
     */
    rocksdb_write(rir->rir_db, rir->rir_writeoptions_async,
                  rir->rir_writebatch, &err);

    DBG_RAFT_INSTANCE_FATAL_IF((err), ri, "rocksdb_write():  %s", err);

    rocksdb_writebatch_clear(rir->rir_writebatch);
}

static void
rsbr_entry_write(struct raft_instance *ri, const struct raft_entry *re,
                 const struct raft_net_sm_write_supplements *ws)
{
    NIOVA_ASSERT(ri && re && re->re_header.reh_index >= 0);

    size_t entry_size = re->re_header.reh_data_size;
    raft_entry_idx_t entry_idx = re->re_header.reh_index;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    rocksdb_writebatch_clear(rir->rir_writebatch);

    /* There are 2 items to write here:
     * 1) raft entry header KV
     * 2) raft entry KV
     */
    size_t entry_header_key_len = 0;
    DECL_AND_FMT_STRING_RET_LEN(entry_header_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&entry_header_key_len,
                                RAFT_ENTRY_HEADER_KEY_PRINTF, entry_idx);

    rocksdb_writebatch_put(rir->rir_writebatch, entry_header_key,
                           entry_header_key_len, (const char *)&re->re_header,
                           sizeof(struct raft_entry_header));

    size_t entry_key_len = 0;
    DECL_AND_FMT_STRING_RET_LEN(entry_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&entry_key_len,
                                RAFT_ENTRY_KEY_PRINTF, entry_idx);

    // Store an entry for every header, even if the entry is empty.
    const char x = '\0';
    const char *entry_val = entry_size ? re->re_data : &x;
    if (!entry_size)
        entry_size = 1;

    rocksdb_writebatch_put(rir->rir_writebatch, entry_key, entry_key_len,
                           entry_val, entry_size);

    // Attach any supplemental writes to the rocksdb-writebatch
    rsbr_write_supplements_put(ws, rir->rir_writebatch);

    char *err = NULL;
    rocksdb_write(rir->rir_db,
                  (raft_server_does_synchronous_writes(ri) ?
                   rir->rir_writeoptions_sync : rir->rir_writeoptions_async),
                  rir->rir_writebatch, &err);

    DBG_RAFT_INSTANCE_FATAL_IF((err), ri, "rocksdb_write():  %s", err);

    rocksdb_writebatch_clear(rir->rir_writebatch);
}

static int
rsbr_get(struct raft_instance_rocks_db *rir, const char *key, size_t key_len,
         void *value, size_t max_value_len, size_t *ret_value_len)
{
    if (!rir || !key || !key_len || !value || !max_value_len)
        return -EINVAL;

    char *err = NULL;
    size_t val_len = 0;

    if (ret_value_len)
        *ret_value_len = 0;

    char *get_value =
        rocksdb_get(rir->rir_db, rir->rir_readoptions, key, key_len, &val_len,
                    &err);

    if (err || !get_value)
        return -ENOENT;  //Xxx need a proper error code intepreter

    memcpy((char *)value, get_value, MIN(val_len, max_value_len));

    free(get_value);

    if (ret_value_len)
        *ret_value_len = val_len;

    return 0;
}

static int
rsbr_get_exact_val_size(struct raft_instance_rocks_db *rir,
                        const char *key, size_t key_len,
                        void *value, size_t expected_value_len)
{
    if (!rir || !key || !key_len || !value || !expected_value_len)
        return -EINVAL;

    size_t ret_value_len = 0;
    int rc = rsbr_get(rir, key, key_len, value,
                      expected_value_len, &ret_value_len);
    if (rc)
    {
        return rc;
    }
    else if (ret_value_len != expected_value_len)
    {
        LOG_MSG(
            LL_NOTIFY,
            "rsbr_get('%.*s') expected-sz(%zu), ret-sz(%zu)",
            (int)key_len, key, expected_value_len, ret_value_len);

        return ret_value_len > expected_value_len ? -ENOSPC : -EMSGSIZE;
    }

    return 0;
}

static int
rsbr_entry_header_read(struct raft_instance *ri, struct raft_entry_header *reh)
{
    if (!ri || !reh || reh->reh_index < 0)
        return -EINVAL;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    size_t entry_header_key_len = 0;
    DECL_AND_FMT_STRING_RET_LEN(entry_header_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&entry_header_key_len,
                                RAFT_ENTRY_HEADER_KEY_PRINTF, reh->reh_index);

    int rc = rsbr_get_exact_val_size(rir, entry_header_key,
                                     entry_header_key_len,
                                     (void *)reh,
                                     sizeof(struct raft_entry_header));
    if (rc)
        LOG_MSG(LL_ERROR, "rsbr_get_exact_val_size('%s'): %s",
                entry_header_key, strerror(rc));

    return rc;
}

static ssize_t
rsbr_entry_read(struct raft_instance *ri, struct raft_entry *re)
{
    if (!ri || !re)
        return -EINVAL;

    int rc = rsbr_entry_header_read(ri, &re->re_header);
    if (rc)
        return rc;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    size_t entry_key_len = 0;
    DECL_AND_FMT_STRING_RET_LEN(entry_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&entry_key_len,
                                RAFT_ENTRY_KEY_PRINTF,
                                re->re_header.reh_index);

    rc = rsbr_get_exact_val_size(rir, entry_key, entry_key_len,
                                 (void *)re->re_data,
                                 re->re_header.reh_data_size);
    if (rc)
        LOG_MSG(LL_ERROR, "rsbr_get_exact_val_size('%s'): %s",
                entry_key, strerror(rc));

    return rc < 0 ? rc :
        re->re_header.reh_data_size + sizeof(struct raft_entry_header);
//Xxx this is wonky
}

static int
rsbr_header_load(struct raft_instance *ri)
{
    if (!ri || !ri->ri_raft_uuid_str || !ri->ri_this_peer_uuid_str)
        return -EINVAL;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    size_t header_key_len = 0;
    DECL_AND_FMT_STRING_RET_LEN(header_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&header_key_len,
                                RAFT_LOG_HEADER_FMT,
                                ri->ri_raft_uuid_str,
                                ri->ri_this_peer_uuid_str);

    int rc = rsbr_get_exact_val_size(rir, header_key, header_key_len,
                                     (void *)&ri->ri_log_hdr,
                                     sizeof(struct raft_log_header));
    if (!rc)
    {
        if (ri->ri_log_hdr.rlh_magic != RAFT_HEADER_MAGIC)
            rc = -EBADMSG;
        else
            DBG_RAFT_INSTANCE(LL_NOTIFY, ri, "");
    }
    return rc;
}

static int
rsbr_header_write(struct raft_instance *ri)
{
    if (!ri || !ri->ri_raft_uuid_str || !ri->ri_this_peer_uuid_str)
        return -EINVAL;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    NIOVA_ASSERT(rir->rir_writeoptions_sync && rir->rir_writebatch);
    rocksdb_writebatch_clear(rir->rir_writebatch);

    size_t key_len;
    DECL_AND_FMT_STRING_RET_LEN(header_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&key_len, RAFT_LOG_HEADER_FMT,
                                ri->ri_raft_uuid_str,
                                ri->ri_this_peer_uuid_str);

    rocksdb_writebatch_put(rir->rir_writebatch, header_key, key_len,
                           (const char *)&ri->ri_log_hdr,
                           sizeof(struct raft_log_header));

    char *err = NULL;
    // Log header writes are always synchronous
    rocksdb_write(rir->rir_db, rir->rir_writeoptions_sync, rir->rir_writebatch,
                  &err);

    DBG_RAFT_INSTANCE_FATAL_IF((err), ri, "rocksdb_write():  %s", err);

    rocksdb_writebatch_clear(rir->rir_writebatch);

    return 0;
}

static int // Call from rib_backend_setup()
rsbr_init_header(struct raft_instance *ri)
{
    if (!ri || !ri->ri_raft_uuid_str || !ri->ri_this_peer_uuid_str)
        return -EINVAL;

    memset(&ri->ri_log_hdr, 0, sizeof(struct raft_log_header));

    // Since we're initializing the header block this is ok
    ri->ri_log_hdr.rlh_magic = RAFT_HEADER_MAGIC;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    NIOVA_ASSERT(rir->rir_writeoptions_sync && rir->rir_writebatch);
    rocksdb_writebatch_clear(rir->rir_writebatch);

    size_t key_len;
    DECL_AND_FMT_STRING_RET_LEN(last_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&key_len, RAFT_LOG_LASTENTRY_FMT,
                                ri->ri_raft_uuid_str,
                                ri->ri_this_peer_uuid_str);

    rocksdb_writebatch_put(rir->rir_writebatch, last_key, key_len,
                           (const char *)&ri->ri_log_hdr,
                           sizeof(struct raft_log_header));

    // Generate and store the db-instance UUID
    uuid_t instance_uuid;
    uuid_generate(instance_uuid);
    rocksdb_writebatch_put(rir->rir_writebatch, RAFT_LOG_HEADER_UUID,
                           RAFT_LOG_HEADER_UUID_STRLEN,
                           (const char *)instance_uuid, sizeof(uuid_t));

    char *err = NULL;
    // Log header writes are always synchronous
    rocksdb_write(rir->rir_db, rir->rir_writeoptions_sync, rir->rir_writebatch,
                  &err);

    DBG_RAFT_INSTANCE_FATAL_IF((err), ri, "rocksdb_write():  %s", err);

    rocksdb_writebatch_clear(rir->rir_writebatch);

    return rsbr_header_write(ri);
}

static int
rsbr_lowest_entry_get(struct raft_instance *ri, raft_entry_idx_t *lowest_idx)
{
    if (!ri || !lowest_idx)
        return -EINVAL;

    *lowest_idx = -1ULL;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    rocksdb_iterator_t *iter = rsbr_create_iterator(rir);
    if (!iter)
        return -ENOMEM;

    int rc = rsbr_iter_seek(iter, RAFT_LOG_HEADER_ROCKSDB_END,
                            RAFT_LOG_HEADER_ROCKSDB_END_STRLEN, true);
    if (rc)
    {
        DBG_RAFT_INSTANCE(LL_ERROR, ri,
                          "rsbr_iter_seek(%s): %s",
                          RAFT_LOG_HEADER_ROCKSDB_END,
                          strerror(-rc));
        rocksdb_iter_destroy(iter);
        return rc;
    }

    size_t iter_key_len = 0;
    for (bool found = false; !found;)
    {
        // Iterate forward looking for the first entry key
        rc = rsbr_iter_next_or_prev(iter, true, true);
        if (rc)
        {
            DBG_RAFT_INSTANCE(LL_ERROR, ri,
                              "rsbr_iter_next_or_prev(): %s",
                              strerror(-rc));
            break;
        }
        else if (rsbr_string_matches_iter_key(
                     RAFT_LOG_LASTENTRY_ROCKSDB,
                     RAFT_LOG_LASTENTRY_ROCKSDB_STRLEN, iter, false))
        {
            break; // no entries found in the keyspace
        }
        else if (rsbr_string_matches_iter_key(
                     RAFT_ENTRY_KEY_PREFIX_ROCKSDB,
                     RAFT_ENTRY_KEY_PREFIX_ROCKSDB_STRLEN, iter, false))
        {
            const char *key = rocksdb_iter_key(iter, &iter_key_len);

            FATAL_IF(((strncmp(key, RAFT_ENTRY_KEY_PREFIX_ROCKSDB,
                               RAFT_ENTRY_KEY_PREFIX_ROCKSDB_STRLEN) &&
                       key[iter_key_len - 1] != 'e')),
                     "unexpected key (`%s'), len=%zu", key, iter_key_len);

            unsigned long long val = 0;
            // The above FATAL_IF guaranteed a non-numeric trailing char

            rc = niova_string_to_unsigned_long_long(
                &key[RAFT_ENTRY_KEY_PREFIX_ROCKSDB_STRLEN], &val);

            NIOVA_ASSERT(!rc);

            *lowest_idx = (raft_entry_idx_t)val;
            found = true;
        }
    }

    SIMPLE_LOG_MSG(LL_NOTIFY, "key='%.*s' lowest-idx=%zd rc=%d",
                   (int)iter_key_len, rocksdb_iter_key(iter, &iter_key_len),
                   *lowest_idx, rc);

    rocksdb_iter_destroy(iter);

    return rc;
}

static ssize_t
rsbr_num_entries_calc(struct raft_instance *ri)
{
    if (!ri)
        return -EINVAL;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    rocksdb_iterator_t *iter = rsbr_create_iterator(rir);
    if (!iter)
        return -ENOMEM;

    ssize_t rrc =
        rsbr_iter_seek(iter, RAFT_LOG_LASTENTRY_ROCKSDB,
                       RAFT_LOG_LASTENTRY_ROCKSDB_STRLEN, true);
    if (rrc)
    {
        DBG_RAFT_INSTANCE(LL_ERROR, ri,
                          "rsbr_iter_seek(%s): %s",
                          RAFT_ENTRY_HEADER_KEY_PREFIX_ROCKSDB,
                          strerror(-rrc));
        rocksdb_iter_destroy(iter);
        return rrc;
    }

    size_t iter_key_len = 0;

    SIMPLE_LOG_MSG(LL_NOTIFY, "last-key='%.*s'",
                   (int)iter_key_len, rocksdb_iter_key(iter, &iter_key_len));

    rrc = rsbr_iter_next_or_prev(iter, true, false);
    if (rrc)
    {
        DBG_RAFT_INSTANCE(LL_ERROR, ri,
                          "rsbr_iter_next_or_prev(%s): %s",
                          RAFT_ENTRY_HEADER_KEY_PREFIX_ROCKSDB,
                          strerror(-rrc));
        rocksdb_iter_destroy(iter);
        return rrc;
    }

    SIMPLE_LOG_MSG(LL_NOTIFY, "prev-last-key='%.*s'",
                   (int)iter_key_len, rocksdb_iter_key(iter, &iter_key_len));

    // There's no key entry or header key here.
    if (rsbr_string_matches_iter_key(RAFT_LOG_HEADER_ROCKSDB_END,
                                     RAFT_LOG_HEADER_ROCKSDB_END_STRLEN,
                                     iter, false))
    {
        rocksdb_iter_destroy(iter);
        return 0;
    }
    else if (!rsbr_string_matches_iter_key(
                 RAFT_ENTRY_HEADER_KEY_PREFIX_ROCKSDB,
                 RAFT_ENTRY_HEADER_KEY_PREFIX_ROCKSDB_STRLEN, iter, false))
    {
        SIMPLE_LOG_MSG(LL_ERROR,
                       "key='%.*s' does not have expected prefix: %s",
                       (int)iter_key_len,
                       rocksdb_iter_key(iter, &iter_key_len),
                       RAFT_ENTRY_HEADER_KEY_PREFIX_ROCKSDB);

        rocksdb_iter_destroy(iter);
        return (ssize_t)-ENOKEY;
    }

    iter_key_len = 0;
    const char *iter_key = rocksdb_iter_key(iter, &iter_key_len);

    if (iter_key_len <= RAFT_ENTRY_KEY_PREFIX_ROCKSDB_STRLEN)
        return (ssize_t)-EBADMSG;

    ssize_t last_entry_idx =
        strtoull(&iter_key[RAFT_ENTRY_KEY_PREFIX_ROCKSDB_STRLEN], NULL, 10);

    SIMPLE_LOG_MSG(LL_NOTIFY, "last-entry-index=%zd", last_entry_idx + 1);

    return last_entry_idx >= 0UL ? last_entry_idx + 1 : last_entry_idx;
}

static void
rsbr_log_truncate(struct raft_instance *ri, const raft_entry_idx_t entry_idx)
{
    NIOVA_ASSERT(ri);

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    // Log truncate ops are always synchronous
    NIOVA_ASSERT(rir->rir_writeoptions_sync && rir->rir_writebatch);

    rocksdb_writebatch_clear(rir->rir_writebatch);

    size_t entry_header_key_len = 0;
    DECL_AND_FMT_STRING_RET_LEN(entry_header_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&entry_header_key_len,
                                RAFT_ENTRY_KEY_PRINTF, entry_idx);

    rocksdb_writebatch_delete_range(rir->rir_writebatch,
                                    entry_header_key, entry_header_key_len,
                                    RAFT_LOG_LASTENTRY_ROCKSDB,
                                    RAFT_LOG_LASTENTRY_ROCKSDB_STRLEN);

    char *err = NULL;
    rocksdb_write(rir->rir_db, rir->rir_writeoptions_sync, rir->rir_writebatch,
                  &err);

    DBG_RAFT_INSTANCE_FATAL_IF((err), ri, "rocksdb_write(): %s", err);

    rocksdb_writebatch_clear(rir->rir_writebatch);
}

static void // runs in checkpoint thread context
rsbr_log_reap(struct raft_instance *ri, const raft_entry_idx_t entry_idx)
{
    NIOVA_ASSERT(ri && rsbr_ri_to_rirdb(ri));
    NIOVA_ASSERT(entry_idx >= 0);

    /* Create our own rocksdb_writebatch_t since this runs outside of the
     * main raft thread.
     */
    rocksdb_writebatch_t *wb = rocksdb_writebatch_create();
    NIOVA_ASSERT(wb);

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    NIOVA_ASSERT(rir->rir_writeoptions_sync);

    /* Take care to remove (and preserve) headers with their entries.  Entry
     * keys end in 'e' (versus 'h' for headers) so this operation should use
     * the lower key suffix ('e') for this operation.
     */

    size_t start_entry_key_len = 0;
    DECL_AND_FMT_STRING_RET_LEN(start_entry_key,
                                RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&start_entry_key_len,
                                RAFT_ENTRY_KEY_PRINTF, (raft_entry_idx_t)0);

    size_t end_entry_key_len = 0;
    DECL_AND_FMT_STRING_RET_LEN(end_entry_key, RAFT_ROCKSDB_KEY_LEN_MAX,
                                (ssize_t *)&end_entry_key_len,
                                RAFT_ENTRY_KEY_PRINTF, entry_idx);

    rocksdb_writebatch_delete_range(wb, start_entry_key, start_entry_key_len,
                                    end_entry_key, end_entry_key_len);

    char *err = NULL;
    rocksdb_write(rir->rir_db, rir->rir_writeoptions_sync, wb, &err);

    DBG_RAFT_INSTANCE_FATAL_IF((err), ri, "rocksdb_write(): %s", err);
    rocksdb_writebatch_destroy(wb);
}

static int // runs in sync thread context
rsbr_sync(struct raft_instance *ri)
{
    if (!ri || !rsbr_ri_to_rirdb(ri))
        return -EINVAL;

    rocksdb_writebatch_t *wb = rocksdb_writebatch_create();
    NIOVA_ASSERT(wb);

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    NIOVA_ASSERT(rir->rir_writeoptions_sync);

    struct timespec ts;
    niova_realtime_coarse_clock(&ts);

    rocksdb_writebatch_put(wb,
                           RAFT_LOG_HEADER_ROCKSDB_LAST_SYNC,
                           RAFT_LOG_HEADER_ROCKSDB_LAST_SYNC_STRLEN,
                           (const char *)&ts, sizeof(struct timespec));

    char *err = NULL;
    rocksdb_write(rir->rir_db, rir->rir_writeoptions_sync, wb, &err);

    DBG_RAFT_INSTANCE_FATAL_IF((err), ri, "rocksdb_write(): %s", err);

    rocksdb_writebatch_destroy(wb);

    return 0;
}

static int
rsbr_checkpoint_path_build(const char *base, const uuid_t peer_id,
                           const uuid_t db_id, raft_entry_idx_t sync_idx,
                           bool local, bool initial, char *chkpt_path,
                           size_t len)
{
    if (!base || uuid_is_null(peer_id) || uuid_is_null(db_id) || !chkpt_path ||
        !len || sync_idx < 0)
        return -EINVAL;

    DECLARE_AND_INIT_UUID_STR(peer_uuid, peer_id);
    DECLARE_AND_INIT_UUID_STR(db_uuid, db_id);

    int rc = snprintf(chkpt_path, len, "%s/%s/%s%s_%s_%ld",
                      base, (local ?
                             ribSubDirs[RIR_SUBDIR_CHKPT_SELF] :
                             ribSubDirs[RIR_SUBDIR_CHKPT_PEERS]),
                      initial ? ".in-progress_" : "", db_uuid, peer_uuid,
                      sync_idx);

    return rc > len ? -ENAMETOOLONG : 0;
}

static int64_t // checkpoint thread context
rsbr_checkpoint(struct raft_instance *ri)
{
    if (!ri || !rsbr_ri_to_rirdb(ri))
        return -EINVAL;

    const raft_entry_idx_t sync_idx =
        raft_server_get_current_raft_entry_index(ri, RI_NEHDR_SYNC);

    if (sync_idx < 0) // Don't checkpoint if the db is empty
        return -ENODATA;

    else if (sync_idx == ri->ri_checkpoint_last_idx)
        return -EALREADY;

    char chkpt_path[PATH_MAX] = {0};
    char chkpt_tmp_path[PATH_MAX] = {0};

    int64_t rc = rsbr_checkpoint_path_build(ri->ri_log,
                                            RAFT_INSTANCE_2_SELF_UUID(ri),
                                            ri->ri_db_uuid, sync_idx,
                                            true, true, chkpt_tmp_path,
                                            PATH_MAX);
    if (rc)
    {
        DBG_RAFT_INSTANCE(LL_ERROR, ri, "rsbr_checkpoint_path_build(): %s",
                          strerror(-rc));
        return -rc;
    }

    rc = rsbr_checkpoint_path_build(ri->ri_log,
                                    RAFT_INSTANCE_2_SELF_UUID(ri),
                                    ri->ri_db_uuid, sync_idx, true, false,
                                    chkpt_path, PATH_MAX);
    if (rc)
    {
        DBG_RAFT_INSTANCE(LL_ERROR, ri, "rsbr_checkpoint_path_build(): %s",
                          strerror(-rc));
        return -rc;
    }

    SIMPLE_LOG_MSG(LL_DEBUG, "tmp-path=%s final-path=%s",
                   chkpt_tmp_path, chkpt_path);

    struct stat stb;
    // Stale tmp path is placed into the trash
    rc = stat(chkpt_tmp_path, &stb);
    if (!rc)
    {
        rc = rsbr_move_item_to_trash(ri, chkpt_tmp_path);
        if (rc)
        {
            SIMPLE_LOG_MSG(LL_ERROR, "rsbr_move_dir_to_trash(`%s'): %s",
                           chkpt_tmp_path, strerror(-rc));
            return rc;
        }
    }

    /* The rename below atomically moves the completed checkpoint into
     * 'chkpt_path', therefore, if 'chkpt_path' exists we can assume it's
     * valid.
     */
    rc = stat(chkpt_path, &stb);
    if (!rc)
    {
        SIMPLE_LOG_MSG(LL_WARN, "chkpt_path=%s already exsits", chkpt_path);
        return -EALREADY;
    }

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    char *err = NULL;

    rocksdb_checkpoint_t *cp =
        rocksdb_checkpoint_object_create(rir->rir_db, &err);

    if (err)
    {
        DBG_RAFT_INSTANCE(LL_ERROR, ri,
                          "rocksdb_checkpoint_object_create(): %s", err);
        return -ENOMEM;
    }

    rocksdb_checkpoint_create(cp, chkpt_tmp_path, 0, &err);

    rocksdb_checkpoint_object_destroy(cp);

    if (err)
    {
        DBG_RAFT_INSTANCE(LL_ERROR, ri, "rocksdb_checkpoint_create(): %s",
                          err);
        return -ENOMEM;
    }

    // Move the directory to its intended location.
    rc = rename(chkpt_tmp_path, chkpt_path);
    if (rc)
        rc = -errno;

    DBG_RAFT_INSTANCE((rc ? LL_ERROR : LL_NOTIFY), ri, "checkpoint@%s: %s",
                      chkpt_path, strerror(-rc));

    return rc ? rc : sync_idx;
}

static int
rsbr_log_dir_open_fd(const struct raft_instance *ri)
{
    return ri ? open(ri->ri_log, O_DIRECTORY | O_RDONLY) : -EINVAL;
}

static int rsbr_scandir_recovery_marker_cb(const struct dirent *dent)
{
    SIMPLE_LOG_MSG(LL_NOTIFY, "d_name=%s", dent->d_name);

    return !regexec(&recovery_marker_regex, dent->d_name, 0, NULL, 0);
}

static int
rsbr_recovery_marker_scan(struct raft_instance *ri)
{
    if (!ri || !ri->ri_backend_arg)
        return -EINVAL;

    struct raft_instance_rocks_db *rir = ri->ri_backend_arg;
    if (rir->rir_log_fd < 0)
        return -EBADF;

    // ri_incomplete_recovery conveys this function's result
    ri->ri_incomplete_recovery = false;

    /* Open the log dir and ensure it's the same inode number as the currently
     * open ri_log_fd.
     */
    struct stat stb = {0};

    int rc = fstat(rir->rir_log_fd, &stb);
    if (rc)
    {
        rc = -errno;
        SIMPLE_LOG_MSG(LL_ERROR, "fstat(log_fd): %s", strerror(-rc));
        return rc;
    }

    struct dirent **recovery_marker_dents = NULL;
    int nents = scandirat(rir->rir_log_fd, ".", &recovery_marker_dents,
                          rsbr_scandir_recovery_marker_cb, alphasort);
    if (nents < 0)
    {
        rc = -errno;
        SIMPLE_LOG_MSG(LL_ERROR, "scandirat(): %s", strerror(-rc));
        return rc;
    }
    else if (nents > 0)
    {
        int n = nents;
        if (n > 1)
        {
            rc = -E2BIG;
            LOG_MSG(LL_ERROR, "Multiple recovery markers detected");
        }
        else
        {
            LOG_MSG(LL_WARN, "Found lingering recovery marker `%s'",
                    recovery_marker_dents[0]->d_name);

            const char *dname = recovery_marker_dents[0]->d_name;
            char peer_uuid_str[UUID_STR_LEN] = {0};
            char db_uuid_str[UUID_STR_LEN] = {0};

            /* These should be safe since rsbr_scandir_recovery_marker_cb()
             * performed a regex check on the dname.
             */
            strncpy(peer_uuid_str,
                    &dname[RECOVERY_MARKER_NAME_LEN_WITH_PERIODS],
                    UUID_STR_LEN - 1);

            strncpy(
                db_uuid_str,                              // includes "_"
                &dname[RECOVERY_MARKER_NAME_LEN_WITH_PERIODS + UUID_STR_LEN],
                UUID_STR_LEN - 1);

            rc = raft_server_init_recovery_handle_from_marker(ri,
                                                              peer_uuid_str,
                                                              db_uuid_str);
            if (rc)
                LOG_MSG(
                    LL_ERROR,
                    "raft_server_init_recovery_handle_from_marker(%s): %s (%s:%s)",
                    recovery_marker_dents[0]->d_name, strerror(-rc),
                    peer_uuid_str, db_uuid_str);
            else
                ri->ri_incomplete_recovery = true; // found valid marker
        }

        // Cleanup scandirat memory allocations
        while (nents--)
            free(recovery_marker_dents[nents]);
        free(recovery_marker_dents);
    }

    return rc;
}

#if 0
static int
rsbr_bulk_recovery_marker_mkpath(const struct raft_recovery_handle *rrh,
                                 char *recovery_marker_path, const size_t len)
{
    if (!rrh || !len || !recovery_marker_path)
        return -EINVAL;

    DECLARE_AND_INIT_UUID_STR(peer_uuid, rrh->rrh_peer_uuid);
    DECLARE_AND_INIT_UUID_STR(db_uuid, rrh->rrh_peer_db_uuid);

    if (uuid_is_null(rrh->rrh_peer_uuid) ||
        uuid_is_null(rrh->rrh_peer_db_uuid))
    {
        LOG_MSG(LL_ERROR, "null uuid (peer=%s, db=%s)", peer_uuid, db_uuid);

        return -EINVAL;
    }

    int rc = snprintf(recovery_marker_path, len, "%s/%s/%s.%s_%s",
                      db_path, ribSubDirs[RIR_SUBDIR_DB],
                      RECOVERY_MARKER_NAME, peer_uuid, db_uuid);
    if (rc >= len)
    {
        LOG_MSG(LL_ERROR, "path requires at least %d bytes (len=%zu)"
                rc, len);

        rc = -ENAMETOOLONG;
    }
    else
    {
        SIMPLE_LOG_MSG(LL_DEBUG, "recovery_marker_path=`%s'",
                       recovery_marker_path);
    }
    return rc;
}
#endif

static int
rsbr_bulk_recover_prepare(struct raft_instance *ri,
                          const struct raft_recovery_handle *rrh)
{
    if (!ri || !rrh || ri->ri_incomplete_recovery ||
        rrh->rrh_from_recovery_marker || rrh->rrh_peer_chkpt_idx < 0)
        return -EINVAL;

    int64_t rrc = rsbr_checkpoint(ri);
    int rc = (rrc < 0 && rrc != -EALREADY && rrc != -ENODATA) ? rrc : 0;

    LOG_MSG((rc < 0 ? LL_ERROR : LL_WARN), "rsbr_checkpoint(%d): %s",
             rc, strerror(-rc));

    return rc;
}

static int //XXX todo
rsbr_bulk_recover_import_remote_db(struct raft_instance *ri,
                                   const struct raft_recovery_handle *rrh)
{
    if (!ri || !rrh || ri->ri_incomplete_recovery ||
        rrh->rrh_from_recovery_marker || rrh->rrh_peer_chkpt_idx < 0)
        return -EINVAL;

    return 0;
}

static int //XXX todo
rsbr_bulk_recover_finalize_and_cleanup(struct raft_instance *ri,
                                       const struct raft_recovery_handle *rrh)
{
    return 0;
}

static int
rsbr_bulk_recover(struct raft_instance *ri)
{
    if (!ri)
        return -EINVAL;

    struct raft_recovery_handle *rrh = raft_instance_2_recovery_handle(ri);
    if (!rrh)
        return -ENOENT;

    if (uuid_is_null(rrh->rrh_peer_uuid) ||
        uuid_is_null(rrh->rrh_peer_db_uuid))
    {
        SIMPLE_LOG_MSG(LL_ERROR, "null peer or db-uuid");
        return -EINVAL;
    }

    // Remove files from "db" dir

    // Dry rsync
    // Calculate local capacity and required rsync capacity
    //   . potentially remove stale local and remote checkpoints?

    // Create chkpt remote dir
    // Rsync data (capturing status along the way..)
    // FAIL:  Retry the same rsync several times before giving up

    // Add recovery marker to "db" following a successful rsync
    // Restore checkpoint contents to the "db" dir - use hardlinks for the
    //     sst files and copy the others

    // Prepare 'db' for use
    //   1. make a new db-UUID
    //   2. reset the peer-uuid in the raft entry headers

    // Remove the recovery marker
    // Cleanup all checkpoints since they're stale

    int rc = 0;
    if (!rrh->rrh_from_recovery_marker)
    {
        rc = rsbr_bulk_recover_prepare(ri, rrh);
        if (rc)
        {
            SIMPLE_LOG_MSG(LL_ERROR, "rsbr_bulk_recover_prepare(): %s",
                           strerror(-rc));
            return rc;
        }

        rc = rsbr_bulk_recover_import_remote_db(ri, rrh);
        if (rc)
        {
            SIMPLE_LOG_MSG(LL_ERROR,
                           "rsbr_bulk_recover_import_remote_db(): %s",
                           strerror(-rc));
            return rc;
        }
    }

    return rsbr_bulk_recover_finalize_and_cleanup(ri, rrh);
}

static int
rsbr_destroy(struct raft_instance *ri)
{
    if (!ri)
        return -EINVAL;

    else if (!ri->ri_backend_arg)
        return -EALREADY;

    struct raft_instance_rocks_db *rir = rsbr_ri_to_rirdb(ri);

    if (rir->rir_log_fd < 0)
    {
        int rc = close(rir->rir_log_fd);
        if (rc)
            SIMPLE_LOG_MSG(LL_WARN, "close(rir_log_fd): %s",
                           strerror(-errno));
        else
            rir->rir_log_fd = -1;
    }

    if (rir->rir_db)
    {
        rocksdb_close(rir->rir_db);

        // User must call raft_server_rocksdb_release_cf_table()
        if (rir->rir_cf_table) // Handles seem to be freed in rocksdb_close()
        {
            struct raft_server_rocksdb_cf_table *cft = rir->rir_cf_table;

            for (size_t i = 0; i < cft->rsrcfe_num_cf; i++)
                if (cft->rsrcfe_cf_handles[i])
                    cft->rsrcfe_cf_handles[i] = NULL;
        }
    }

    if (rir->rir_writeoptions_sync)
        rocksdb_writeoptions_destroy(rir->rir_writeoptions_sync);

    if (rir->rir_writeoptions_async)
        rocksdb_writeoptions_destroy(rir->rir_writeoptions_async);

    if (rir->rir_readoptions)
        rocksdb_readoptions_destroy(rir->rir_readoptions);

    if (rir->rir_options)
        rocksdb_options_destroy(rir->rir_options);

    if (rir->rir_writebatch)
        rocksdb_writebatch_destroy(rir->rir_writebatch);

    regfree(&recovery_marker_regex);

    ri->ri_backend_arg = NULL;

    return 0;
}

static int
rsbr_subdirs_setup(struct raft_instance *ri)
{
    if (!ri || !ri->ri_backend_arg)
        return -EINVAL;

    struct raft_instance_rocks_db *rir = ri->ri_backend_arg;

    int rc = file_util_pathname_build(ri->ri_log);
    if (rc)
    {
         SIMPLE_LOG_MSG(LL_ERROR, "file_util_pathname_build(%s): %s",
                        ri->ri_log, strerror(-rc));
         return rc;
    }

    rir->rir_log_fd = rsbr_log_dir_open_fd(ri);

    if (rir->rir_log_fd < 0)
    {
        int rc = -errno;
        SIMPLE_LOG_MSG(LL_ERROR, "open(%s): %s", ri->ri_log, strerror(-rc));

        return rc;
    }

    for (enum raft_instance_rocks_db_subdirs i = RIR_SUBDIR__MIN;
         i < RIR_SUBDIR__MAX; i++)
    {
        int rc = mkdirat(rir->rir_log_fd, ribSubDirs[i], 0700);
        if (rc)
        {
            rc = -errno;

            if (rc == -EEXIST)
            {
                struct stat stb;
                rc = fstatat(rir->rir_log_fd, ribSubDirs[i], &stb,
                             AT_SYMLINK_NOFOLLOW);
                if (rc)
                {
                    rc = -errno;
                    SIMPLE_LOG_MSG(LL_ERROR, "fstatat(%s): %s",
                                   ribSubDirs[i], strerror(-rc));
                    return rc;
                }
                else if (!S_ISDIR(stb.st_mode))
                {
                    SIMPLE_LOG_MSG(LL_ERROR, "Path %s: %s",
                                   ribSubDirs[i], strerror(ENOTDIR));
                    return -ENOTDIR;
                }
            }
            else
            {
                SIMPLE_LOG_MSG(LL_ERROR, "mkdirat(%s): %s",
                               ribSubDirs[i], strerror(-rc));
                return rc;
            }
        }
    }

    return 0;
}

static int
rsbr_setup_detect_recovery(struct raft_instance *ri)
{
    if (!ri)
        return -EINVAL;

    int rc = rsbr_recovery_marker_scan(ri);

    if (!rc && ri->ri_incomplete_recovery)
    {
        const struct raft_recovery_handle *rrh =
            raft_instance_2_recovery_handle(ri);

        if (!rrh->rrh_from_recovery_marker ||
            uuid_is_null(rrh->rrh_peer_uuid) ||
            uuid_is_null(rrh->rrh_peer_db_uuid))
        {
            SIMPLE_LOG_MSG(LL_ERROR, "invalid incomplete recovery state");
            return -ENXIO;
        }

        return -EUCLEAN; // Special 'rc', caller will try to resume recovery
    }

    NIOVA_ASSERT(rc != -EUCLEAN); // Reserved rc cannot be used here

    return rc;
}

static int
rsbr_setup(struct raft_instance *ri)
{
    if (!ri || ri->ri_backend != &ribRocksDB)
        return -EINVAL;

    else if (ri->ri_backend_arg)
        return -EALREADY;

    int rc = regcomp(&recovery_marker_regex, RECOVERY_MARKER_REGEX, 0);
    if (rc)
    {
        char regerr_str[63] = {0};
        regerror(rc, &recovery_marker_regex, regerr_str, 63);
        SIMPLE_LOG_MSG(LL_ERROR, "regcomp(): %s", regerr_str);
        return -EINVAL;
    }

    ri->ri_backend_arg =
        niova_calloc(1UL, sizeof(struct raft_instance_rocks_db));

    if (!ri->ri_backend_arg)
        return -ENOMEM;

    struct raft_instance_rocks_db *rir = ri->ri_backend_arg;
    rir->rir_log_fd = -1;

    rc = rsbr_subdirs_setup(ri);
    if (rc)
    {
         rsbr_destroy(ri);
         return rc;
    }

    // Check for an existing recovery marker
    rc = rsbr_setup_detect_recovery(ri);
    if (rc)
    {
        rsbr_destroy(ri);
        return rc;
    }

    // The db will live in a subdir of 'ri->ri_log'
    char rocksdb_dir[PATH_MAX] = {0};
    rc = snprintf(rocksdb_dir, PATH_MAX, "%s/%s", ri->ri_log,
                  ribSubDirs[RIR_SUBDIR_DB]);
    if (rc > PATH_MAX)
    {
        rsbr_destroy(ri);
        return -ENAMETOOLONG;
    }
    // Reset return code
    rc = 0;

    rir->rir_options = rocksdb_options_create();
    if (!rir->rir_options)
    {
        rsbr_destroy(ri);
        return -ENOMEM;
    }

    /* The user may have passed in a list of column family names which are to
     * be opened.  These must be specified at db-open() time.
     */
    if (ri->ri_backend_init_arg)
        rir->rir_cf_table =
            (struct raft_server_rocksdb_cf_table *)ri->ri_backend_init_arg;


//     const long int cpus = sysconf(_SC_NPROCESSORS_ONLN);
//    rocksdb_options_increase_parallelism(rir->rir_options, (int)(cpus));

//    rocksdb_options_set_use_direct_reads(rir->rir_options, 1);

//    rocksdb_options_set_use_direct_io_for_flush_and_compaction(
//        rir->rir_options, 1);

    rir->rir_writeoptions_sync = rocksdb_writeoptions_create();
    if (!rir->rir_writeoptions_sync)
    {
        rsbr_destroy(ri);
        return -ENOMEM;
    }

    rocksdb_writeoptions_set_sync(rir->rir_writeoptions_sync, 1);

    // Make a non-sync option as well.
    rir->rir_writeoptions_async = rocksdb_writeoptions_create();
    if (!rir->rir_writeoptions_async)
    {
        rsbr_destroy(ri);
        return -ENOMEM;
    }

    rir->rir_readoptions = rocksdb_readoptions_create();
    if (!rir->rir_readoptions)
    {
        rc = -ENOMEM;
        goto out;
    }

    rir->rir_writebatch = rocksdb_writebatch_create();
    if (!rir->rir_writebatch)
    {
        rc = -ENOMEM;
        goto out;
    }

    char *err = NULL;

    rocksdb_options_set_create_if_missing(rir->rir_options, 0);
    rocksdb_options_set_create_missing_column_families(rir->rir_options, 1);
    /* The documentation around this option is a bit confusing.  At this time,
     * I don't think the option is needed for pumiceDB (which uses multiple
     * CFs) since there's no explicit flushing of WALs or disabling of WALs for
     * specific CF operations.
     */
//    rocksdb_options_set_atomic_flush(rir->rir_options, 1);

    /* See https://github.com/facebook/rocksdb/wiki/Atomic-flush
     * Users of this backend are expected to use column families.
     */
//    rocksdb_options_set_atomic_flush(rir->rir_options, 1);

    struct raft_server_rocksdb_cf_table *cft = rir->rir_cf_table;

    const rocksdb_options_t *cft_opts[RAFT_ROCKSDB_MAX_CF];
    if (cft && cft->rsrcfe_num_cf)
    {
        NIOVA_ASSERT(cft->rsrcfe_num_cf <= RAFT_ROCKSDB_MAX_CF);
        for (int i = 0; i < cft->rsrcfe_num_cf; i++)
            cft_opts[i] = rir->rir_options;
    }

    rir->rir_db = (cft && cft->rsrcfe_num_cf) ?
        rocksdb_open_column_families(rir->rir_options, rocksdb_dir,
                                     cft->rsrcfe_num_cf, cft->rsrcfe_cf_names,
                                     cft_opts, cft->rsrcfe_cf_handles, &err) :
        rocksdb_open(rir->rir_options, rocksdb_dir, &err);

    if (!rir->rir_db || err)
    {
        // DB may not be created
        err = NULL;

        rocksdb_options_set_create_if_missing(rir->rir_options, 1);
//        rir->rir_db = rocksdb_open(rir->rir_options, rocksdb_dir, &err);

        rir->rir_db = (cft && cft->rsrcfe_num_cf) ?
            rocksdb_open_column_families(rir->rir_options, rocksdb_dir,
                                         cft->rsrcfe_num_cf,
                                         cft->rsrcfe_cf_names,
                                         cft_opts, cft->rsrcfe_cf_handles,
                                         &err) :
            rocksdb_open(rir->rir_options, rocksdb_dir, &err);

        if (rir->rir_db && !err)
        {
            rc = rsbr_init_header(ri);
            if (rc)
            {
                SIMPLE_LOG_MSG(LL_ERROR, "rsbr_init_header(): %s",
                               strerror(rc));
                goto out;
            }
        }
        else
        {
            SIMPLE_LOG_MSG(LL_ERROR, "rocksdb_open(): %s", err);
            rc = -ENOTCONN;
            goto out;
        }
    }

    /* If all is well to this point, determine the number of entries which
     * this backend instance contains and write that value into the
     * raft_instance structure.
     */
    if (!rc && !err)
    {
        rsb_sm_get_instance_uuid(ri);

        ri->ri_entries_detected_at_startup = rsbr_num_entries_calc(ri);
        if (ri->ri_entries_detected_at_startup < 0)
            rc = ri->ri_entries_detected_at_startup;

        raft_entry_idx_t lowest_idx = -1;
        if (ri->ri_entries_detected_at_startup > 0)
        {
            rc = rsbr_lowest_entry_get(ri, &lowest_idx);
            FATAL_IF(rc, "rsbr_lowest_entry_get(): %s", strerror(-rc));
        }
        niova_atomic_init(&ri->ri_lowest_idx, lowest_idx);

        /* Applications which store their application data in RocksDB may
         * bypass the entries which have already been applied.
         */
        if (ri->ri_store_type == RAFT_INSTANCE_STORE_ROCKSDB_PERSISTENT_APP)
            rsb_sm_get_last_applied_kv_idx(ri);

        SIMPLE_LOG_MSG(LL_WARN, "entry-idxs: lowest=%ld highest=%ld",
                       lowest_idx, ri->ri_entries_detected_at_startup - 1);
    }
out:
    if (rc || err)
        rsbr_destroy(ri);

    return rc;
}

/**
 * raft_server_backend_use_rocksdb - selects the rocksDB raft driver.
 */
void
raft_server_backend_use_rocksdb(struct raft_instance *ri)
{
    NIOVA_ASSERT(ri && !ri->ri_backend);

    ri->ri_backend = &ribRocksDB;
}

/**
 * raft_server_get_rocksdb_instance - public function used by pumiceDB to
 *    obtain the rocksDB pointer from the raft_instance.
 */
rocksdb_t *
raft_server_get_rocksdb_instance(struct raft_instance *ri)
{
    if (ri &&
        (ri->ri_store_type == RAFT_INSTANCE_STORE_ROCKSDB ||
         ri->ri_store_type == RAFT_INSTANCE_STORE_ROCKSDB_PERSISTENT_APP) &&
        ri->ri_backend && ri->ri_backend_arg)
        return rsbr_ri_to_rocksdb(ri);

    return NULL;
}

void
raft_server_rocksdb_release_cf_table(struct raft_server_rocksdb_cf_table *cft)
{
    if (!cft)
        return;

    for (size_t i = 0; i < cft->rsrcfe_num_cf; i++)
    {
        if (cft->rsrcfe_cf_names[i])
        {
            free((char *)cft->rsrcfe_cf_names[i]);
            cft->rsrcfe_cf_names[i] = NULL;
        }
        if (cft->rsrcfe_cf_handles[i])
        {
            rocksdb_column_family_handle_destroy(cft->rsrcfe_cf_handles[i]);
            cft->rsrcfe_cf_handles[i] = NULL;
        }
    }

    cft->rsrcfe_num_cf = 0;
}

int
raft_server_rocksdb_add_cf_name(struct raft_server_rocksdb_cf_table *cft,
                                const char *cf_name, const size_t cf_name_len)
{
    if (!cft || !cf_name || !cf_name_len ||
        cf_name_len > RAFT_ROCKSDB_MAX_CF_NAME_LEN)
        return -EINVAL;

    if (!cft->rsrcfe_num_cf) // First, add the 'default' CF
    {
        cft->rsrcfe_cf_names[0] = strndup("default", 7);
        if (!cft->rsrcfe_cf_names[0])
            return -ENOMEM;

        cft->rsrcfe_num_cf = 1;
    }

    if (cft->rsrcfe_num_cf >= RAFT_ROCKSDB_MAX_CF)
        return -ENOSPC;

    for (size_t i = 1; i < cft->rsrcfe_num_cf; i++)
        if (!strncmp(cf_name, cft->rsrcfe_cf_names[i], RAFT_ROCKSDB_MAX_CF))
            return -EALREADY;

    cft->rsrcfe_cf_names[cft->rsrcfe_num_cf] = strndup(cf_name, cf_name_len);
    if (!cft->rsrcfe_cf_names[cft->rsrcfe_num_cf])
        return -ENOMEM;

    cft->rsrcfe_num_cf++;

    return 0;
}
