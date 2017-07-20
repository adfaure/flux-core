/*****************************************************************************\
 *  Copyright (c) 2015 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <czmq.h>
#include <flux/core.h>

#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/xzmalloc.h"
#include "src/common/libutil/log.h"
#include "src/common/libkvs/json_dirent.h"

#include "commit.h"
#include "json_util.h"

struct commit_mgr {
    struct cache *cache;
    const char *hash_name;
    int noop_stores;            /* for kvs.stats.get, etc.*/
    zhash_t *fences;
    zlist_t *ready;
    void *aux;
};

struct commit {
    int errnum;
    fence_t *f;
    int blocked:1;
    json_object *rootcpy;   /* working copy of root dir */
    href_t newroot;
    zlist_t *item_callback_list;
    commit_mgr_t *cm;
    enum {
        COMMIT_STATE_INIT = 1,
        COMMIT_STATE_LOAD_ROOT = 2,
        COMMIT_STATE_APPLY_OPS = 3,
        COMMIT_STATE_STORE = 4,
        COMMIT_STATE_PRE_FINISHED = 5,
        COMMIT_STATE_FINISHED = 6,
    } state;
};

static void commit_destroy (commit_t *c)
{
    if (c) {
        Jput (c->rootcpy);
        if (c->item_callback_list)
            zlist_destroy (&c->item_callback_list);
        /* fence destroyed through management of fence, not commit_t's
         * responsibility */
        free (c);
    }
}

static commit_t *commit_create (fence_t *f, commit_mgr_t *cm)
{
    commit_t *c;

    if (!(c = calloc (1, sizeof (*c)))) {
        errno = ENOMEM;
        goto error;
    }
    c->f = f;
    if (!(c->item_callback_list = zlist_new ())) {
        errno = ENOMEM;
        goto error;
    }
    c->cm = cm;
    c->state = COMMIT_STATE_INIT;
    return c;
error:
    commit_destroy (c);
    return NULL;
}

int commit_get_errnum (commit_t *c)
{
    return c->errnum;
}

fence_t *commit_get_fence (commit_t *c)
{
    return c->f;
}

void *commit_get_aux (commit_t *c)
{
    return c->cm->aux;
}

const char *commit_get_newroot_ref (commit_t *c)
{
    if (c->state == COMMIT_STATE_FINISHED)
        return c->newroot;
    return NULL;
}

/* Store object 'o' under key 'ref' in local cache. */
static int store_cache (commit_t *c, int current_epoch, json_object *o,
                        href_t ref, struct cache_entry **hpp)
{
    struct cache_entry *hp;
    int rc = -1;

    if (json_hash (c->cm->hash_name, o, ref) < 0) {
        log_err ("json_hash");
        goto done;
    }
    if (!(hp = cache_lookup (c->cm->cache, ref, current_epoch))) {
        hp = cache_entry_create (NULL);
        cache_insert (c->cm->cache, ref, hp);
    }
    if (cache_entry_get_valid (hp)) {
        c->cm->noop_stores++;
        json_object_put (o);
    } else {
        cache_entry_set_json (hp, o);
        cache_entry_set_dirty (hp, true);
        cache_entry_set_content_store_flag (hp, true);
    }
    *hpp = hp;
    rc = 0;
 done:
    return rc;
}

/* Store DIRVAL objects, converting them to DIRREFs.
 * Store (large) FILEVAL objects, converting them to FILEREFs.
 * Return false and enqueue wait_t on cache object(s) if any are dirty.
 */
static int commit_unroll (commit_t *c, int current_epoch, json_object *dir)
{
    json_object_iter iter;
    json_object *subdir, *value;
    const char *s;
    href_t ref;
    int rc = -1;
    struct cache_entry *hp;

    json_object_object_foreachC (dir, iter) {
        if (json_object_object_get_ex (iter.val, "DIRVAL", &subdir)) {
            if (commit_unroll (c, current_epoch, subdir) < 0) /* depth first */
                goto done;
            json_object_get (subdir);
            if (store_cache (c, current_epoch, subdir, ref, &hp) < 0)
                goto done;
            if (cache_entry_get_dirty (hp)) {
                if (zlist_push (c->item_callback_list, hp) < 0)
                    oom ();
            }
            json_object_object_add (dir, iter.key,
                                    dirent_create ("DIRREF", ref));
        }
        else if (json_object_object_get_ex (iter.val, "FILEVAL", &value)
                                && (s = Jtostr (value))
                                && strlen (s) > BLOBREF_MAX_STRING_SIZE) {
            json_object_get (value);
            if (store_cache (c, current_epoch, value, ref, &hp) < 0)
                goto done;
            if (cache_entry_get_dirty (hp)) {
                if (zlist_push (c->item_callback_list, hp) < 0)
                    oom ();
            }
            json_object_object_add (dir, iter.key,
                                    dirent_create ("FILEREF", ref));
        }
    }
    rc = 0;
done:
    return rc;
}

/* link (key, dirent) into directory 'dir'.
 */
static int commit_link_dirent (commit_t *c, int current_epoch,
                               json_object *rootdir, const char *key,
                               json_object *dirent, const char **missing_ref)
{
    char *cpy = xstrdup (key);
    char *next, *name = cpy;
    json_object *dir = rootdir;
    json_object *o, *subdir = NULL, *subdirent;
    int rc = -1;

    /* Special case root
     */
    if (strcmp (name, ".") == 0) {
        errno = EINVAL;
        goto done;
    }

    /* This is the first part of a key with multiple path components.
     * Make sure that it is a directory in DIRVAL form, then recurse
     * on the remaining path components.
     */
    while ((next = strchr (name, '.'))) {
        *next++ = '\0';
        if (!json_object_object_get_ex (dir, name, &subdirent)) {
            if (!dirent) /* key deletion - it doesn't exist so return */
                goto success;
            if (!(subdir = json_object_new_object ()))
                oom ();
            json_object_object_add (dir, name, dirent_create ("DIRVAL",subdir));
            json_object_put (subdir);
        } else if (json_object_object_get_ex (subdirent, "DIRVAL", &o)) {
            subdir = o;
        } else if (json_object_object_get_ex (subdirent, "DIRREF", &o)) {
            const char *ref = json_object_get_string (o);
            if (!(subdir = cache_lookup_and_get_json (c->cm->cache,
                                                      ref,
                                                      current_epoch))) {
                *missing_ref = ref;
                goto success; /* stall */
            }
            /* do not corrupt store by modify orig. */
            subdir = json_object_copydir (subdir);
            json_object_object_add (dir, name, dirent_create ("DIRVAL",subdir));
            json_object_put (subdir);
        } else if (json_object_object_get_ex (subdirent, "LINKVAL", &o)) {
            assert (json_object_get_type (o) == json_type_string);
            char *nkey = xasprintf ("%s.%s", json_object_get_string (o), next);
            if (commit_link_dirent (c,
                                    current_epoch,
                                    rootdir,
                                    nkey,
                                    dirent,
                                    missing_ref) < 0) {
                free (nkey);
                goto done;
            }
            free (nkey);
            goto success;
        } else {
            if (!dirent) /* key deletion - it doesn't exist so return */
                goto success;
            if (!(subdir = json_object_new_object ()))
                oom ();
            json_object_object_add (dir, name, dirent_create ("DIRVAL",subdir));
            json_object_put (subdir);
        }
        name = next;
        dir = subdir;
    }
    /* This is the final path component of the key.  Add it to the directory.
     */
    if (dirent)
        json_object_object_add (dir, name, json_object_get (dirent));
    else
        json_object_object_del (dir, name);
success:
    rc = 0;
done:
    free (cpy);
    return rc;
}

commit_process_t commit_process (commit_t *c,
                                 int current_epoch,
                                 const href_t rootdir_ref)
{
    /* Incase user calls commit_process() again */
    if (c->errnum)
        return COMMIT_PROCESS_ERROR;

    switch (c->state) {
        case COMMIT_STATE_INIT:
        case COMMIT_STATE_LOAD_ROOT:
        {
            /* Make a copy of the root directory.
             */
            json_object *rootdir;

            /* Caller didn't call commit_iter_missing_refs() */
            if (zlist_first (c->item_callback_list))
                goto stall_load;

            c->state = COMMIT_STATE_LOAD_ROOT;

            if (!(rootdir = cache_lookup_and_get_json (c->cm->cache,
                                                       rootdir_ref,
                                                       current_epoch))) {
                if (zlist_push (c->item_callback_list, (void *)rootdir_ref) < 0)
                    oom ();
                goto stall_load;
            }

            c->rootcpy = json_object_copydir (rootdir);

            c->state = COMMIT_STATE_APPLY_OPS;
            /* fallthrough */
        }
        case COMMIT_STATE_APPLY_OPS:
        {
            /* Apply each op (e.g. key = val) in sequence to the root
             * copy.  A side effect of walking key paths is to convert
             * DIRREFs to DIRVALs in the copy. This allows the commit
             * to be self-contained in the rootcpy until it is
             * unrolled later on.
             */
            if (fence_get_json_ops (c->f)) {
                json_object *op, *dirent;
                const char *missing_ref = NULL;
                const char *key;
                json_object *ops = fence_get_json_ops (c->f);
                int i, len = json_object_array_length (ops);

                /* Caller didn't call commit_iter_missing_refs() */
                if (zlist_first (c->item_callback_list))
                    goto stall_load;

                for (i = 0; i < len; i++) {
                    missing_ref = NULL;
                    if (!(op = json_object_array_get_idx (ops, i))
                        || !Jget_str (op, "key", &key))
                        continue;
                    dirent = NULL;
                    /* can be NULL for unlink */
                    (void)Jget_obj (op, "dirent", &dirent);
                    if (commit_link_dirent (c,
                                            current_epoch,
                                            c->rootcpy,
                                            key,
                                            dirent,
                                            &missing_ref) < 0) {
                        c->errnum = errno;
                        break;
                    }
                    if (missing_ref) {
                        if (zlist_push (c->item_callback_list,
                                        (void *)missing_ref) < 0)
                            oom ();
                    }
                }

                if (c->errnum != 0)
                    return COMMIT_PROCESS_ERROR;

                if (zlist_first (c->item_callback_list))
                    goto stall_load;

            }
            c->state = COMMIT_STATE_STORE;
            /* fallthrough */
        }
        case COMMIT_STATE_STORE:
        {
            /* Unroll the root copy.
             * When a DIRVAL is found, store an object and replace it
             * with a DIRREF.  Finally, store the unrolled root copy
             * as an object and keep its reference in c->newroot.
             * Flushes to content cache are asynchronous but we don't
             * proceed until they are completed.
             */
            struct cache_entry *hp;

            if (commit_unroll (c, current_epoch, c->rootcpy) < 0)
                c->errnum = errno;
            else if (store_cache (c,
                                  current_epoch,
                                  c->rootcpy,
                                  c->newroot,
                                  &hp) < 0)
                c->errnum = errno;
            else if (cache_entry_get_dirty (hp)
                     && zlist_push (c->item_callback_list, hp) < 0)
                oom ();

            if (c->errnum)
                return COMMIT_PROCESS_ERROR;

            /* cache took ownership of rootcpy, we're done, but
             * may still need to stall user.
             */
            c->state = COMMIT_STATE_PRE_FINISHED;
            c->rootcpy = NULL;

            /* fallthrough */
        }
        case COMMIT_STATE_PRE_FINISHED:
            /* If we did not fall through to here, caller didn't call
             * commit_iter_dirty_cache_entries()
             */
            if (zlist_first (c->item_callback_list))
                goto stall_store;

            c->state = COMMIT_STATE_FINISHED;
            /* fallthrough */
        case COMMIT_STATE_FINISHED:
            break;
        default:
            log_msg ("invalid commit state: %d", c->state);
            c->errnum = EPERM;
            return COMMIT_PROCESS_ERROR;
    }

    return COMMIT_PROCESS_FINISHED;

stall_load:
    c->blocked = 1;
    return COMMIT_PROCESS_LOAD_MISSING_REFS;

stall_store:
    c->blocked = 1;
    return COMMIT_PROCESS_DIRTY_CACHE_ENTRIES;
}

int commit_iter_missing_refs (commit_t *c, commit_ref_cb cb, void *data)
{
    const char *ref;
    int rc = 0;

    if (c->state != COMMIT_STATE_LOAD_ROOT
        && c->state != COMMIT_STATE_APPLY_OPS)
        return -1;

    while ((ref = zlist_pop (c->item_callback_list))) {
        if (cb (c, ref, data) < 0) {
            rc = -1;
            break;
        }
    }

    if (rc < 0)
        while ((ref = zlist_pop (c->item_callback_list)));

    return rc;
}

int commit_iter_dirty_cache_entries (commit_t *c,
                                     commit_cache_entry_cb cb,
                                     void *data)
{
    struct cache_entry *hp;
    int rc = 0;

    if (c->state != COMMIT_STATE_PRE_FINISHED)
        return -1;

    while ((hp = zlist_pop (c->item_callback_list))) {
        if (cb (c, hp, data) < 0) {
            rc = -1;
            break;
        }
    }

    if (rc < 0)
        while ((hp = zlist_pop (c->item_callback_list)));

    return rc;
}

commit_mgr_t *commit_mgr_create (struct cache *cache,
                                 const char *hash_name,
                                 void *aux)
{
    commit_mgr_t *cm;

    if (!(cm = calloc (1, sizeof (*cm)))) {
        errno = ENOMEM;
        goto error;
    }
    cm->cache = cache;
    cm->hash_name = hash_name;
    if (!(cm->fences = zhash_new ())) {
        errno = ENOMEM;
        goto error;
    }
    if (!(cm->ready = zlist_new ())) {
        errno = ENOMEM;
        goto error;
    }
    cm->aux = aux;
    return cm;

 error:
    commit_mgr_destroy (cm);
    return NULL;
}

void commit_mgr_destroy (commit_mgr_t *cm)
{
    if (cm) {
        if (cm->fences)
            zhash_destroy (&cm->fences);
        if (cm->ready)
            zlist_destroy (&cm->ready);
        free (cm);
    }
}

int commit_mgr_add_fence (commit_mgr_t *cm, fence_t *f)
{
    const char *name;

    if (!Jget_ar_str (fence_get_json_names (f), 0, &name)) {
        errno = EINVAL;
        goto error;
    }
    if (zhash_insert (cm->fences, name, f) < 0) {
        errno = EEXIST;
        goto error;
    }
    zhash_freefn (cm->fences, name, (zhash_free_fn *)fence_destroy);
    return 0;
error:
    return -1;
}

fence_t *commit_mgr_lookup_fence (commit_mgr_t *cm, const char *name)
{
    return zhash_lookup (cm->fences, name);
}

int commit_mgr_process_fence_request (commit_mgr_t *cm, fence_t *f)
{
    if (fence_count_reached (f)) {
        commit_t *c;

        if (!(c = commit_create (f, cm)))
            return -1;

        if (zlist_append (cm->ready, c) < 0)
            oom ();
        zlist_freefn (cm->ready, c, (zlist_free_fn *)commit_destroy, true);
    }

    return 0;
}

bool commit_mgr_commits_ready (commit_mgr_t *cm)
{
    commit_t *c;

    if ((c = zlist_first (cm->ready)) && !c->blocked)
        return true;
    return false;
}

commit_t *commit_mgr_get_ready_commit (commit_mgr_t *cm)
{
    if (commit_mgr_commits_ready (cm))
        return zlist_first (cm->ready);
    return NULL;
}

void commit_mgr_remove_commit (commit_mgr_t *cm, commit_t *c)
{
    zlist_remove (cm->ready, c);
}

void commit_mgr_remove_fence (commit_mgr_t *cm, const char *name)
{
    zhash_delete (cm->fences, name);
}

int commit_mgr_get_noop_stores (commit_mgr_t *cm)
{
    return cm->noop_stores;
}

void commit_mgr_clear_noop_stores (commit_mgr_t *cm)
{
    cm->noop_stores = 0;
}

/* Merge ready commits that are mergeable, where merging consists of
 * popping the "donor" commit off the ready list, and appending its
 * ops to the top commit.  The top commit can be appended to if it
 * hasn't started, or is still building the rootcpy, e.g. stalled
 * walking the namespace.
 *
 * Break when an unmergeable commit is discovered.  We do not wish to
 * merge non-adjacent fences, as it can create undesireable out of
 * order scenarios.  e.g.
 *
 * commit #1 is mergeable:     set A=1
 * commit #2 is non-mergeable: set A=2
 * commit #3 is mergeable:     set A=3
 *
 * If we were to merge commit #1 and commit #3, A=2 would be set after
 * A=3.
 */
void commit_mgr_merge_ready_commits (commit_mgr_t *cm)
{
    commit_t *c = zlist_first (cm->ready);

    /* commit must still be in state where merged in ops can be
     * applied */
    if (c
        && c->errnum == 0
        && c->state <= COMMIT_STATE_APPLY_OPS
        && !(fence_get_flags (c->f) & FLUX_KVS_NO_MERGE)) {
        commit_t *nc;
        nc = zlist_pop (cm->ready);
        assert (nc == c);
        while ((nc = zlist_first (cm->ready))) {

            /* if return == 0, we've merged as many as we currently
             * can */
            if (!fence_merge (c->f, nc->f))
                break;

            /* Merged fence, remove off ready list */
            zlist_remove (cm->ready, nc);
        }
        if (zlist_push (cm->ready, c) < 0)
            oom ();
        zlist_freefn (cm->ready, c, (zlist_free_fn *)commit_destroy, false);
    }
}