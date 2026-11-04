/*
 * SPDX-FileCopyrightText: Copyright (c) 1999-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */


/*
 * Pinned user memory and externally registered pages.
 */

#ifdef NV_UMEM_USERLAND_TEST
#include "nvidia_umem_test_kernel.h"
#else
#include "nv-illumos.h"
#include "nv-reg.h"
#include "nvidia_mem.h"
#endif
#include "nvidia_umem_registry_impl.h"

#ifndef NV_UMEM_USERLAND_TEST
#include <sys/vmem.h>
#include <sys/vmsystm.h>
#include <sys/gfx_private.h>
#include <sys/ddi_impldefs.h>
#include <sys/pci_impl.h>
#include <sys/disp.h>
#include <vm/hat.h>
#include <vm/as.h>
#include <vm/seg.h>
#include <vm/seg_kmem.h>
#include <vm/page.h>
#endif

/*
 * User memory pinning.  umem_lockmemory(DDI_UMEMLOCK_LONGTERM) keeps the
 * pages locked beyond the ioctl and enforces locked-memory resource
 * controls.  Physical addresses come from the pagelock's shadow list, or from
 * the process HAT when the segment softlocked the range instead.
 */
typedef struct nv_user_pages_s {
    list_node_t          registry_link;
    taskq_ent_t          unlock_ent;
    list_t               imports;
    struct as           *as;
    uint64_t             seq;
    uint_t               refs;
    boolean_t            pending;
    boolean_t            indexed;
    boolean_t            callback_seen;
    boolean_t            quarantined;
    void                *alloc_private;
    void                *mem_desc;
    nv_umem_original_state_t original;
    NvU32                original_client;
    NvU32                original_handle;
    NV_STATUS          (*original_free)(NvU32, NvU32, void *);
    nv_umem_pin_state_t  state;
    ddi_umem_cookie_t   cookie;
    caddr_t             addr;
    size_t              len;
    NvU64               page_count;
    NvU64              *phys;
} nv_user_pages_t;

typedef struct nv_umem_import_token {
    list_node_t          link;
    nv_user_pages_t     *up;
    void               *mem_desc;
    NvU32               import_client;
    void               *owner;
    NV_STATUS          (*revoke)(void *);
    uint_t               refs;
    boolean_t            linked;
    boolean_t            published;
    boolean_t            hardware_complete;
    boolean_t            in_callback;
    kthread_t            *revoke_thread;
    nv_umem_copy_state_t copy;
} nv_umem_import_token_t;

static kmutex_t nv_umem_lock;
static kcondvar_t nv_umem_cv;
static list_t nv_umem_pins;
static uint64_t nv_umem_seq;
static boolean_t nv_umem_used;
static taskq_t *nv_umem_unlock_tq;

NV_STATUS NV_API_CALL
os_umem_original_publish(void *page_array, void *mem_desc,
    NvU32 hClient, NvU32 hMemory,
    NV_STATUS (*checked_free)(NvU32, NvU32, void *))
{
    nv_user_pages_t *up;
    NV_STATUS status = NV_ERR_INVALID_STATE;

    if (page_array == NULL || mem_desc == NULL || checked_free == NULL)
        return NV_ERR_INVALID_ARGUMENT;
    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up == page_array)
            break;
    }
    if (up != NULL && !up->state.unlocked &&
        nv_umem_original_publish(&up->original)) {
        up->mem_desc = mem_desc;
        up->original_client = hClient;
        up->original_handle = hMemory;
        up->original_free = checked_free;
        status = NV_OK;
        cv_broadcast(&nv_umem_cv);
    }
    mutex_exit(&nv_umem_lock);
    return status;
}

void NV_API_CALL
os_umem_original_begin_close(void *mem_desc, NvU32 hClient, NvU32 hMemory)
{
    nv_user_pages_t *up;

    if (mem_desc == NULL)
        return;
    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up->mem_desc == mem_desc) {
            nv_umem_original_begin_close_if_match(&up->original,
                up->original_client, up->original_handle, hClient, hMemory);
            cv_broadcast(&nv_umem_cv);
            break;
        }
    }
    mutex_exit(&nv_umem_lock);
}

int
nv_umem_init(void)
{
    mutex_init(&nv_umem_lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&nv_umem_cv, NULL, CV_DRIVER, NULL);
    list_create(&nv_umem_pins, sizeof (nv_user_pages_t),
        offsetof(nv_user_pages_t, registry_link));
    nv_umem_unlock_tq = taskq_create("nvidia_umem_unlock", 1,
        minclsyspri, 1, INT_MAX, TASKQ_PREPOPULATE);
    if (nv_umem_unlock_tq == NULL) {
        list_destroy(&nv_umem_pins);
        cv_destroy(&nv_umem_cv);
        mutex_destroy(&nv_umem_lock);
        return ENOMEM;
    }
    return 0;
}

boolean_t
nv_umem_busy(void)
{
    boolean_t busy;

    mutex_enter(&nv_umem_lock);
    busy = nv_umem_used || !list_is_empty(&nv_umem_pins);
    mutex_exit(&nv_umem_lock);
    return busy;
}

void
nv_umem_fini(void)
{
    VERIFY(!nv_umem_busy());
    taskq_destroy(nv_umem_unlock_tq);
    nv_umem_unlock_tq = NULL;
    list_destroy(&nv_umem_pins);
    cv_destroy(&nv_umem_cv);
    mutex_destroy(&nv_umem_lock);
}

static void
nv_umem_pin_free(nv_user_pages_t *up)
{
    VERIFY(!up->indexed);
    VERIFY(list_is_empty(&up->imports));
    kmem_free(up->phys, up->page_count * sizeof (NvU64));
    list_destroy(&up->imports);
    kmem_free(up, sizeof (*up));
}

static boolean_t
nv_umem_pin_rele_locked(nv_user_pages_t *up)
{
    ASSERT(MUTEX_HELD(&nv_umem_lock));
    VERIFY(up->refs != 0);
    return (--up->refs == 0);
}

static nv_user_pages_t *
nv_umem_find_cookie_locked(ddi_umem_cookie_t cookie)
{
    nv_user_pages_t *up;

    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (!up->pending && !up->state.unlocked && up->cookie == cookie)
            return up;
    }
    return NULL;
}

static boolean_t
nv_umem_pending_on_locked(struct as *as, uint64_t seq)
{
    nv_user_pages_t *up;

    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up->pending && up->as == as && up->seq <= seq)
            return B_TRUE;
    }
    return B_FALSE;
}

static void
nv_umem_token_unlink_locked(nv_umem_import_token_t *token)
{
    nv_user_pages_t *up = token->up;

    ASSERT(MUTEX_HELD(&nv_umem_lock));
    VERIFY(token->linked);
    list_remove(&up->imports, token);
    token->linked = B_FALSE;
    if (!token->hardware_complete)
        nv_umem_pin_cancel(&up->state, !token->published);
    if (up->state.unlocked && list_is_empty(&up->imports) && up->indexed) {
        list_remove(&nv_umem_pins, up);
        up->indexed = B_FALSE;
    }
    cv_broadcast(&nv_umem_cv);
}

/*
 * Reserve before RM duplicates the descriptor. A pointer absent from this
 * registry belongs to another OS descriptor type and is not tracked here.
 * Pending tokens are canceled by RM under its locks; published tokens are
 * completed by UVM outside RM locks.
 */
NV_STATUS NV_API_CALL
os_umem_import_reserve(void *page_array, void *mem_desc,
    NvU32 import_client, void **token_out)
{
    nv_umem_import_token_t *token;
    nv_user_pages_t *up;

    if (token_out == NULL)
        return NV_ERR_INVALID_ARGUMENT;
    *token_out = NULL;
    if (page_array == NULL)
        return NV_OK;
    token = kmem_zalloc(sizeof (*token), KM_NOSLEEP);
    if (token == NULL)
        return NV_ERR_NO_MEMORY;

    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (!up->pending && nv_umem_pin_matches(up,
            up->alloc_private, page_array))
            break;
    }
    if (up == NULL) {
        mutex_exit(&nv_umem_lock);
        kmem_free(token, sizeof (*token));
        return NV_OK;
    }
    if (!nv_umem_pin_reserve(&up->state)) {
        mutex_exit(&nv_umem_lock);
        kmem_free(token, sizeof (*token));
        return NV_ERR_INVALID_STATE;
    }
    if (up->mem_desc != NULL && up->mem_desc != mem_desc) {
        nv_umem_pin_cancel(&up->state, 1);
        mutex_exit(&nv_umem_lock);
        kmem_free(token, sizeof (*token));
        return NV_ERR_INVALID_STATE;
    }
    up->mem_desc = mem_desc;
    token->up = up;
    token->mem_desc = mem_desc;
    token->import_client = import_client;
    token->refs = 2; /* registry and caller */
    token->linked = B_TRUE;
    up->refs++;
    list_insert_tail(&up->imports, token);
    mutex_exit(&nv_umem_lock);
    *token_out = token;
    return NV_OK;
}

void NV_API_CALL
os_umem_import_guard_acquired(void *opaque)
{
    nv_umem_import_token_t *token = opaque;

    if (token == NULL)
        return;
    mutex_enter(&nv_umem_lock);
    VERIFY(token->linked && token->up->mem_desc == token->mem_desc);
    nv_umem_pin_guard_acquired(&token->up->state);
    mutex_exit(&nv_umem_lock);
}

/* Keep the copy exception confined to the synchronous tracked DupObject. */
void NV_API_CALL
os_umem_import_copy_window(void *opaque, NvBool open)
{
    nv_umem_import_token_t *token = opaque;

    if (token == NULL)
        return;
    mutex_enter(&nv_umem_lock);
    VERIFY(token->linked);
    if (open)
        nv_umem_copy_authorize(&token->copy, curthread);
    else
        nv_umem_copy_revoke(&token->copy);
    mutex_exit(&nv_umem_lock);
}

NvBool NV_API_CALL
os_umem_copy_allowed(void *mem_desc)
{
    nv_user_pages_t *up;
    nv_umem_import_token_t *token;
    NvBool allowed = NV_TRUE;

    if (mem_desc == NULL)
        return NV_TRUE;
    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up->mem_desc == mem_desc)
            break;
    }
    if (up != NULL) {
        allowed = NV_FALSE;
        for (token = list_head(&up->imports); token != NULL;
            token = list_next(&up->imports, token)) {
            if (token->mem_desc == mem_desc && token->linked &&
                nv_umem_copy_allowed(1, &token->copy, curthread)) {
                allowed = NV_TRUE;
                break;
            }
        }
    }
    mutex_exit(&nv_umem_lock);
    return allowed;
}

NvBool NV_API_CALL
os_umem_map_allowed(void *mem_desc, NvU32 hClient)
{
    nv_user_pages_t *up;
    nv_umem_import_token_t *token;
    NvBool allowed = NV_TRUE;

    if (mem_desc == NULL)
        return NV_TRUE;
    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up->mem_desc == mem_desc)
            break;
    }
    if (up != NULL) {
        allowed = NV_FALSE;
        for (token = list_head(&up->imports); token != NULL;
            token = list_next(&up->imports, token)) {
            if (token->mem_desc == mem_desc && token->linked &&
                nv_umem_map_allowed(1, token->import_client, hClient)) {
                allowed = NV_TRUE;
                break;
            }
        }
    }
    mutex_exit(&nv_umem_lock);
    return allowed;
}

NvU32 NV_API_CALL
os_umem_import_guard_count(void *mem_desc)
{
    nv_user_pages_t *up;
    NvU32 count = 0;

    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up->mem_desc == mem_desc) {
            count = up->state.guards;
            break;
        }
    }
    mutex_exit(&nv_umem_lock);
    return count;
}

NV_STATUS NV_API_CALL
os_umem_import_guard_released(void *mem_desc)
{
    nv_user_pages_t *up;

    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up->mem_desc == mem_desc)
            break;
    }
    if (up == NULL || up->state.guards == 0) {
        mutex_exit(&nv_umem_lock);
        return NV_ERR_INVALID_STATE;
    }
    nv_umem_pin_guard_released(&up->state);
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    return NV_OK;
}

/*
 * Called after checked GPU, PTE, RM copy and IOMMU barriers.  A normal UVM
 * release must retry after dropping its locks if another thread is running
 * the callback; a call from the callback itself proceeds.  The token stays
 * indexed for RM's final owner/guard cleanup.
 */
NV_STATUS NV_API_CALL
os_umem_import_hardware_complete(void *opaque)
{
    nv_umem_import_token_t *token = opaque;

    if (token == NULL)
        return NV_OK;
    mutex_enter(&nv_umem_lock);
    if (token->in_callback && token->revoke_thread != curthread) {
        mutex_exit(&nv_umem_lock);
        return NV_ERR_BUSY_RETRY;
    }
    if (!token->linked) {
        mutex_exit(&nv_umem_lock);
        return NV_ERR_INVALID_STATE;
    }
    if (!token->hardware_complete) {
        token->hardware_complete = B_TRUE;
        if (token->published)
            nv_umem_pin_hardware_complete(&token->up->state);
        else
            nv_umem_pin_hardware_complete_pending(&token->up->state);
        token->owner = NULL;
        token->revoke = NULL;
    }
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    return NV_OK;
}

void *NV_API_CALL
os_umem_import_memdesc(void *opaque)
{
    nv_umem_import_token_t *token = opaque;

    return token == NULL ? NULL : token->mem_desc;
}

NvBool NV_API_CALL
os_umem_import_guard_pending(void *opaque)
{
    nv_umem_import_token_t *token = opaque;
    NvBool pending;

    if (token == NULL)
        return NV_FALSE;
    mutex_enter(&nv_umem_lock);
    pending = token->linked && token->up->state.guards != 0;
    mutex_exit(&nv_umem_lock);
    return pending;
}

void NV_API_CALL
os_umem_import_put(void *opaque)
{
    nv_umem_import_token_t *token = opaque;
    boolean_t free_token;

    if (token == NULL)
        return;
    mutex_enter(&nv_umem_lock);
    VERIFY(token->hardware_complete || !token->linked);
    VERIFY(token->refs != 0);
    free_token = (--token->refs == 0); /* caller ownership */
    mutex_exit(&nv_umem_lock);
    if (free_token)
        kmem_free(token, sizeof (*token));
}

NvBool NV_API_CALL
os_umem_import_all_hardware_complete(void *mem_desc)
{
    nv_user_pages_t *up;
    nv_umem_import_token_t *token;
    NvBool complete = NV_TRUE;

    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up->mem_desc != mem_desc)
            continue;
        for (token = list_head(&up->imports); token != NULL;
            token = list_next(&up->imports, token)) {
            if (!token->hardware_complete) {
                complete = NV_FALSE;
                break;
            }
        }
        break;
    }
    mutex_exit(&nv_umem_lock);
    return complete;
}

/* RM holds its own locks here, so this never waits for a callback borrower. */
void NV_API_CALL
os_umem_import_cancel_completed(void *mem_desc)
{
    nv_user_pages_t *up;
    nv_umem_import_token_t *token, *next;
    boolean_t free_up;
    boolean_t free_token;

    mutex_enter(&nv_umem_lock);
    for (up = list_head(&nv_umem_pins); up != NULL;
        up = list_next(&nv_umem_pins, up)) {
        if (up->mem_desc == mem_desc)
            break;
    }
    if (up == NULL) {
        mutex_exit(&nv_umem_lock);
        return;
    }
    up->refs++; /* Stable even if the final token unindexes the pin. */
    for (token = list_head(&up->imports); token != NULL; token = next) {
        next = list_next(&up->imports, token);
        if (!token->hardware_complete)
            continue;
        nv_umem_token_unlink_locked(token);
        free_token = (--token->refs == 0);
        (void) nv_umem_pin_rele_locked(up);
        if (free_token)
            kmem_free(token, sizeof (*token));
    }
    free_up = nv_umem_pin_rele_locked(up);
    mutex_exit(&nv_umem_lock);
    if (free_up)
        nv_umem_pin_free(up);
}

NV_STATUS NV_API_CALL
os_umem_import_publish(void *opaque, void *owner,
    NV_STATUS (*revoke)(void *))
{
    nv_umem_import_token_t *token = opaque;

    if (token == NULL)
        return NV_OK;
    if (owner == NULL || revoke == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    mutex_enter(&nv_umem_lock);
    if (!token->linked || token->published || token->hardware_complete) {
        mutex_exit(&nv_umem_lock);
        return NV_ERR_INVALID_STATE;
    }
    token->owner = owner;
    token->revoke = revoke;
    token->published = B_TRUE;
    nv_umem_pin_publish(&token->up->state);
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    return NV_OK;
}

void NV_API_CALL
os_umem_import_cancel(void *opaque)
{
    nv_umem_import_token_t *token = opaque;
    nv_user_pages_t *up;
    boolean_t free_up;
    boolean_t free_token;
    boolean_t unlinked = B_FALSE;

    if (token == NULL)
        return;
    mutex_enter(&nv_umem_lock);
    token->refs++;
    up = token->up;
    up->refs++; /* Keep the pin alive while waiting for callback borrower. */
    while (token->in_callback)
        cv_wait(&nv_umem_cv, &nv_umem_lock);
    if (token->linked) {
        nv_umem_token_unlink_locked(token);
        unlinked = B_TRUE;
        VERIFY(token->refs > 2);
        token->refs--; /* registry ownership */
    }
    VERIFY(token->refs > 1);
    token->refs--; /* caller ownership */
    token->refs--; /* this call */
    if (unlinked)
        (void) nv_umem_pin_rele_locked(up); /* token ownership */
    free_up = nv_umem_pin_rele_locked(up); /* this call */
    free_token = (token->refs == 0);
    mutex_exit(&nv_umem_lock);
    if (free_token)
        kmem_free(token, sizeof (*token));
    if (free_up)
        nv_umem_pin_free(up);
}

/*
 * Called with no AS lock.  A callback can beat publication of the cookie by
 * umem_lockmemory(), so wait for earlier pending pins of this address space.
 * Revoke only published owners; their callbacks must retire GPU PTEs, RM
 * duplicate and any DMA mappings before reporting success.  This callback
 * never unlocks the user pages itself: the guarded RM descriptor destructor
 * remains the single owner of ddi_umem_unlock().
 */
static void
nv_umem_lock_cleanup(ddi_umem_cookie_t *arg)
{
    ddi_umem_cookie_t cookie = (ddi_umem_cookie_t)arg;
    struct as *as = ((struct ddi_umem_cookie *)cookie)->asp;
    nv_user_pages_t *up;
    nv_umem_import_token_t *token;
    NV_STATUS status;
    NvU32 original_client;
    NvU32 original_handle;
    NV_STATUS (*original_free)(NvU32, NvU32, void *);
    void *original_mem_desc;
    uint64_t seq;
    boolean_t free_up;
    boolean_t free_token;

    mutex_enter(&nv_umem_lock);
    seq = nv_umem_seq;
    while ((up = nv_umem_find_cookie_locked(cookie)) == NULL &&
        nv_umem_pending_on_locked(as, seq))
        cv_wait(&nv_umem_cv, &nv_umem_lock);
    if (up == NULL) {
        mutex_exit(&nv_umem_lock);
        return; /* Normal unlock may have removed it before this lookup. */
    }
    up->refs++;
    up->callback_seen = B_TRUE;
    nv_umem_pin_begin_revoke(&up->state);

    for (;;) {
        while (up->state.pending != 0)
            cv_wait(&nv_umem_cv, &nv_umem_lock);
        for (token = list_head(&up->imports); token != NULL;
            token = list_next(&up->imports, token)) {
            if (!token->hardware_complete)
                break;
        }
        if (token == NULL) {
            /*
             * UVM can finish while the original RM Memory still owns the
             * descriptor.  The same thread may be blocked in this mmap,
             * so waiting for userspace to free that handle is cyclic.
             */
            while (!up->state.unlocked) {
                if (nv_umem_original_claim(&up->original)) {
                    original_client = up->original_client;
                    original_handle = up->original_handle;
                    original_free = up->original_free;
                    original_mem_desc = up->mem_desc;
                    mutex_exit(&nv_umem_lock);
                    status = original_free(original_client,
                        original_handle, original_mem_desc);
                    mutex_enter(&nv_umem_lock);
                    if (!up->state.unlocked) {
                        up->quarantined = B_TRUE;
                        nv_printf(NV_DBG_ERRORS,
                            "NVRM: checked original OS descriptor free "
                            "did not unlock pin: 0x%x\n", status);
                    }
                    continue;
                }
                cv_wait(&nv_umem_cv, &nv_umem_lock);
            }
            break;
        }
        if (!token->published || token->in_callback) {
            cv_wait(&nv_umem_cv, &nv_umem_lock);
            continue;
        }

        token->in_callback = B_TRUE;
        token->revoke_thread = curthread;
        token->refs++;
        mutex_exit(&nv_umem_lock);
        status = token->revoke(token->owner);
        mutex_enter(&nv_umem_lock);
        token->in_callback = B_FALSE;
        token->revoke_thread = NULL;
        free_up = B_FALSE;
        cv_broadcast(&nv_umem_cv);
        if (status == NV_ERR_BUSY_RETRY) {
            /*
             * Normal UVM release may own this token. Hold its callback
             * reference while it completes, then resume the outer loop so
             * other imports on the same pin are still revoked here.
             */
            while (token->linked && !token->hardware_complete)
                cv_wait(&nv_umem_cv, &nv_umem_lock);
            VERIFY(token->refs != 0);
            free_token = (--token->refs == 0);
            mutex_exit(&nv_umem_lock);
            if (free_token)
                kmem_free(token, sizeof (*token));
            mutex_enter(&nv_umem_lock);
            continue;
        }
        VERIFY(token->refs != 0);
        free_token = (--token->refs == 0); /* callback borrow */
        if (status != NV_OK || !token->hardware_complete) {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: checked umem import revoke failed: "
                "pin_seq=%llu client=0x%x status=0x%x\n",
                (unsigned long long)up->seq, token->import_client, status);
            /*
             * as_free resumes as soon as this callback returns.  An RM or
             * IOMMU failure cannot let it release anonymous backing while
             * the GPU might still reach the pin. Sleep until another owner
             * completes every import; a permanent failure retains the AS.
             */
            while (!nv_umem_pin_can_unlock(&up->state))
                cv_wait(&nv_umem_cv, &nv_umem_lock);
            mutex_exit(&nv_umem_lock);
            if (free_token)
                kmem_free(token, sizeof (*token));
            if (free_up)
                nv_umem_pin_free(up);
            goto done;
        }
        mutex_exit(&nv_umem_lock);
        if (free_token)
            kmem_free(token, sizeof (*token));
        if (free_up)
            nv_umem_pin_free(up);
        mutex_enter(&nv_umem_lock);
    }
    mutex_exit(&nv_umem_lock);

done:
    mutex_enter(&nv_umem_lock);
    free_up = nv_umem_pin_rele_locked(up);
    mutex_exit(&nv_umem_lock);
    if (free_up)
        nv_umem_pin_free(up);
}

static struct umem_callback_ops nv_umem_callbacks = {
    .cbo_umem_callback_version = UMEM_CALLBACK_VERSION,
    .cbo_umem_lock_cleanup = nv_umem_lock_cleanup,
};

static void
nv_umem_unlock_task(void *arg)
{
    nv_user_pages_t *up = arg;
    boolean_t free_up;

    /* This thread cannot hold the caller's AS writer or RM locks. */
    ddi_umem_unlock(up->cookie);

    mutex_enter(&nv_umem_lock);
    if (list_is_empty(&up->imports)) {
        list_remove(&nv_umem_pins, up);
        up->indexed = B_FALSE;
    }
    cv_broadcast(&nv_umem_cv);
    free_up = nv_umem_pin_rele_locked(up); /* descriptor ownership */
    mutex_exit(&nv_umem_lock);
    if (free_up)
        nv_umem_pin_free(up);
}

#ifndef NV_UMEM_USERLAND_TEST
NV_STATUS NV_API_CALL os_lock_user_pages(
    void   *address,
    NvU64   page_count,
    void  **page_array,
    NvU32   flags
)
{
    nv_user_pages_t *up;
    caddr_t addr = (caddr_t)((uintptr_t)address & MMU_PAGEMASK);
    int lflags = DDI_UMEMLOCK_READ | DDI_UMEMLOCK_LONGTERM;
    page_t **pparray;
    NvU64 i;
    int err;

    if (!nv_may_sleep())
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s(): invalid context!\n", __func__);
        return NV_ERR_NOT_SUPPORTED;
    }

    if (page_count == 0 || page_count > (SIZE_MAX >> MMU_PAGESHIFT) ||
        (uintptr_t)addr + mmu_ptob(page_count) < (uintptr_t)addr)
        return NV_ERR_INVALID_ARGUMENT;

    if (DRF_VAL(_LOCK_USER_PAGES, _FLAGS, _WRITE, flags))
        lflags |= DDI_UMEMLOCK_WRITE;

    up = kmem_zalloc(sizeof (*up), KM_SLEEP);
    up->addr = addr;
    up->len = mmu_ptob(page_count);
    up->page_count = page_count;
    up->phys = kmem_alloc(page_count * sizeof (NvU64), KM_NOSLEEP | KM_NORMALPRI);
    if (up->phys == NULL)
    {
        kmem_free(up, sizeof (*up));
        return NV_ERR_NO_MEMORY;
    }

    up->as = curproc->p_as;
    up->refs = 1;
    up->pending = B_TRUE;
    up->indexed = B_TRUE;
    nv_umem_original_init(&up->original);
    nv_umem_pin_state_init(&up->state);
    list_create(&up->imports, sizeof (nv_umem_import_token_t),
        offsetof(nv_umem_import_token_t, link));
    mutex_enter(&nv_umem_lock);
    up->seq = ++nv_umem_seq;
    nv_umem_used = B_TRUE;
    list_insert_tail(&nv_umem_pins, up);
    mutex_exit(&nv_umem_lock);

    err = umem_lockmemory(addr, up->len, lflags, &up->cookie,
        &nv_umem_callbacks, curproc);
    if (err != 0)
    {
        mutex_enter(&nv_umem_lock);
        list_remove(&nv_umem_pins, up);
        up->indexed = B_FALSE;
        cv_broadcast(&nv_umem_cv);
        mutex_exit(&nv_umem_lock);
        nv_umem_pin_free(up);
        return NV_ERR_INVALID_ADDRESS;
    }

    mutex_enter(&nv_umem_lock);
    up->pending = B_FALSE;
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);

    pparray = ((struct ddi_umem_cookie *)up->cookie)->pparray;

    for (i = 0; i < page_count; i++)
    {
        pfn_t pfn = (pparray != NULL) ? page_pptonum(pparray[i]) :
            hat_getpfnum(curproc->p_as->a_hat, addr + mmu_ptob(i));

        if (pfn == PFN_INVALID)
        {
            (void) os_unlock_user_pages(page_count, up, flags);
            return NV_ERR_INVALID_ADDRESS;
        }
        up->phys[i] = mmu_ptob((NvU64)pfn);
    }

    *page_array = up;
    return NV_OK;
}

NV_STATUS NV_API_CALL os_unlock_user_pages(
    NvU64  page_count,
    void  *page_array,
    NvU32  flags
)
{
    nv_user_pages_t *up = page_array;

    if (up == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    mutex_enter(&nv_umem_lock);
    if (up->state.unlocked) {
        mutex_exit(&nv_umem_lock);
        return NV_OK;
    }
    if (!nv_umem_pin_can_unlock(&up->state)) {
        /*
         * RM currently discards this function's error status.  Keep the
         * cookie and pin live if it ever destroys a descriptor while a
         * tracked duplicate still owns GPU access.  A checked RM guard
         * must prevent this path during normal operation.
         */
        up->quarantined = B_TRUE;
        nv_printf(NV_DBG_ERRORS,
            "NVRM: retaining umem pin %p: %u imports, %u guards remain\n",
            (void *)up->cookie, up->state.imports, up->state.guards);
        mutex_exit(&nv_umem_lock);
        return NV_OK;
    }
    nv_umem_original_cancel(&up->original);
    VERIFY(nv_umem_pin_claim_unlock(&up->state));
    /*
     * A selected AS callback may enter after this transition. It then finds
     * no live pin and returns, while as_execute_callback still waits for the
     * worker's as_delete_callback before the VM can unmap the pages.
     */
    cv_broadcast(&nv_umem_cv);
    taskq_dispatch_ent(nv_umem_unlock_tq, nv_umem_unlock_task, up, 0,
        &up->unlock_ent);
    mutex_exit(&nv_umem_lock);
    return NV_OK;
}

/*
 * Resolve a user range that maps device memory (another driver's mmap) to
 * physical addresses.  The range must lie in one device segment and be
 * physically contiguous, as on Linux.
 */
NV_STATUS NV_API_CALL os_lookup_user_io_memory(
    void   *address,
    NvU64   page_count,
    NvU64 **pte_array
)
{
    struct as *as = curproc->p_as;
    caddr_t start = (caddr_t)address;
    extern struct seg_ops segdev_ops;
    struct seg *seg;
    NvU64 *ptes;
    NV_STATUS status = NV_OK;
    NvU64 i;

    if (!nv_may_sleep())
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s(): invalid context!\n", __func__);
        return NV_ERR_NOT_SUPPORTED;
    }

    if (page_count == 0 || page_count > (SIZE_MAX >> MMU_PAGESHIFT) ||
        ((uintptr_t)start & MMU_PAGEOFFSET) != 0)
        return NV_ERR_INVALID_ADDRESS;

    if (os_alloc_mem((void **)&ptes, page_count * sizeof (NvU64)) != NV_OK)
        return NV_ERR_NO_MEMORY;

    AS_LOCK_ENTER(as, RW_READER);

    seg = as_segat(as, start);
    if (seg == NULL || seg->s_ops != &segdev_ops ||
        start + mmu_ptob(page_count) > seg->s_base + seg->s_size ||
        start + mmu_ptob(page_count) < start)
    {
        nv_printf(NV_DBG_ERRORS,
            "Cannot map memory with base addr 0x%p and size of 0x%llx pages\n",
            (void *)start, page_count);
        status = NV_ERR_INVALID_ADDRESS;
        goto done;
    }

    for (i = 0; i < page_count; i++)
    {
        pfn_t pfn = hat_getpfnum(as->a_hat, start + mmu_ptob(i));

        if (pfn == PFN_INVALID || pf_is_memory(pfn))
        {
            status = NV_ERR_INVALID_ADDRESS;
            goto done;
        }

        ptes[i] = mmu_ptob((NvU64)pfn);
        if (i > 0 && ptes[i] != ptes[i - 1] + MMU_PAGESIZE)
        {
            status = NV_ERR_INVALID_ADDRESS;
            goto done;
        }
    }

done:
    AS_LOCK_EXIT(as);

    if (status != NV_OK)
        os_free_mem(ptes);
    else
        *pte_array = ptes;

    return status;
}

NV_STATUS NV_API_CALL nv_register_user_pages(
    nv_state_t *nv,
    NvU64       page_count,
    NvU64      *phys_addr,
    void       *import_priv,
    void      **priv_data,
    NvBool      unencrypted
)
{
    nv_user_pages_t *up = *priv_data;
    nv_illumos_alloc_t *at;
    NvU64 i;

    if (up == NULL || page_count > up->page_count)
        return NV_ERR_INVALID_ARGUMENT;

    at = nvos_create_alloc(nv_dma_dip(nv), page_count);
    if (at == NULL)
        return NV_ERR_NO_MEMORY;

    /* Anonymous memory must be write-back cacheable. */
    at->cache_type = NV_MEMORY_CACHED;
    at->flags.user = NV_TRUE;

    for (i = 0; i < page_count; i++)
        at->page_table[i].phys_addr = phys_addr[i] = up->phys[i];

    at->user_pages = up;
    at->import_priv = import_priv;
    mutex_enter(&nv_umem_lock);
    up->alloc_private = at;
    mutex_exit(&nv_umem_lock);
    *priv_data = at;

    return NV_OK;
}

void NV_API_CALL nv_unregister_user_pages(
    nv_state_t *nv,
    NvU64       page_count,
    void      **import_priv,
    void      **priv_data
)
{
    nv_illumos_alloc_t *at = *priv_data;
    nv_user_pages_t *up = at->user_pages;

    mutex_enter(&nv_umem_lock);
    if (up->alloc_private == at)
        up->alloc_private = NULL;
    mutex_exit(&nv_umem_lock);
    *priv_data = up;
    if (import_priv != NULL)
        *import_priv = at->import_priv;

    at->user_pages = NULL;
    nv_alloc_rele(at);
}

NV_STATUS NV_API_CALL nv_register_peer_io_mem(
    nv_state_t *nv,
    NvU64      *phys_addr,
    NvU64       page_count,
    void      **priv_data
)
{
    nv_illumos_alloc_t *at;
    NvU64 i;

    at = nvos_create_alloc(nv_dma_dip(nv), page_count);
    if (at == NULL)
        return NV_ERR_NO_MEMORY;

    at->cache_type = NV_MEMORY_UNCACHED;
    at->flags.contig = NV_TRUE;
    at->flags.peer_io = NV_TRUE;

    for (i = 0; i < page_count; i++)
        at->page_table[i].phys_addr = phys_addr[0] + mmu_ptob(i);

    *priv_data = at;
    return NV_OK;
}

void NV_API_CALL nv_unregister_peer_io_mem(nv_state_t *nv, void *priv_data)
{
    nv_alloc_rele(priv_data);
}

NV_STATUS NV_API_CALL nv_register_phys_pages(
    nv_state_t *nv,
    NvU64      *phys_addr,
    NvU64       page_count,
    NvU32       cache_type,
    void      **priv_data
)
{
    nv_illumos_alloc_t *at;
    NvU64 i;

    if (!nv_cache_type_valid(cache_type))
        return NV_ERR_NOT_SUPPORTED;

    at = nvos_create_alloc(nv_dma_dip(nv), page_count);
    if (at == NULL)
        return NV_ERR_NO_MEMORY;

    at->cache_type = cache_type;
    at->flags.physical = NV_TRUE;

    for (i = 0; i < page_count; i++)
        at->page_table[i].phys_addr = phys_addr[i];

    *priv_data = at;
    return NV_OK;
}

void NV_API_CALL nv_unregister_phys_pages(nv_state_t *nv, void *priv_data)
{
    nv_alloc_rele(priv_data);
}

/* Scatter-gather tables only come from dma-buf imports, which illumos lacks. */
NV_STATUS NV_API_CALL nv_register_sgt(
    nv_state_t *nv,
    NvU64      *phys_addr,
    NvU64       page_count,
    NvU32       cache_type,
    void      **priv_data,
    struct sg_table *import_sgt,
    void       *import_priv,
    NvBool      is_peer_mmio
)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_unregister_sgt(
    nv_state_t *nv,
    struct sg_table **import_sgt,
    void **import_priv,
    void *priv_data
)
{
}

NV_STATUS NV_API_CALL nv_get_num_phys_pages(void *pAllocPrivate, NvU32 *pNumPages)
{
    nv_illumos_alloc_t *at = pAllocPrivate;

    if (pNumPages == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    *pNumPages = at->num_pages;
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_get_phys_pages(void *pAllocPrivate, void *pPages,
    NvU32 *pNumPages)
{
    nv_illumos_alloc_t *at = pAllocPrivate;
    page_t **pages = pPages;
    NvU32 i, count;

    if (pNumPages == NULL || pPages == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    count = MIN(*pNumPages, at->num_pages);
    for (i = 0; i < count; i++)
        pages[i] = page_numtopp_nolock(mmu_btop(at->page_table[i].phys_addr));

    *pNumPages = count;
    return NV_OK;
}
#endif /* NV_UMEM_USERLAND_TEST */
