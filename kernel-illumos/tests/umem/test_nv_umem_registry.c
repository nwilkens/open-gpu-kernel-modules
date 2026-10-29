/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Carlos Neira
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

#ifdef NDEBUG
#undef NDEBUG /* Assertions are the test checks and must not compile away. */
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "../../nvidia/nvidia_umem_registry_impl.h"

static void
test_pending_publication_and_revoke(void)
{
    nv_umem_pin_state_t pin = {0};

    nv_umem_pin_state_init(&pin);
    assert(nv_umem_pin_reserve(&pin));
    assert(pin.pending == 1);
    nv_umem_pin_begin_revoke(&pin);
    assert(!nv_umem_pin_reserve(&pin));
    assert(!nv_umem_pin_can_unlock(&pin));
    nv_umem_pin_publish(&pin);
    assert(pin.pending == 0 && pin.imports == 1);
    assert(!nv_umem_pin_can_unlock(&pin));
    nv_umem_pin_cancel(&pin, 0);
    assert(nv_umem_pin_can_unlock(&pin));
}

static void
test_normal_release_waits_for_pending_owner(void)
{
    nv_umem_pin_state_t pin = {0};

    nv_umem_pin_state_init(&pin);
    assert(nv_umem_pin_reserve(&pin));
    assert(!nv_umem_pin_can_unlock(&pin));
    nv_umem_pin_cancel(&pin, 1);
    assert(nv_umem_pin_can_unlock(&pin));
    nv_umem_pin_mark_unlocked(&pin);
    assert(!nv_umem_pin_reserve(&pin));
}

static void
test_unlock_claim_is_once_and_blocks_new_imports(void)
{
    nv_umem_pin_state_t pin = {0};

    nv_umem_pin_state_init(&pin);
    assert(nv_umem_pin_claim_unlock(&pin));
    assert(pin.unlocked);
    assert(!nv_umem_pin_reserve(&pin));
    assert(!nv_umem_pin_claim_unlock(&pin));
}

static void
test_unlock_claim_retains_live_pin_until_import_completes(void)
{
    nv_umem_pin_state_t pin = {0};

    nv_umem_pin_state_init(&pin);
    assert(nv_umem_pin_reserve(&pin));
    assert(!nv_umem_pin_claim_unlock(&pin));
    assert(!pin.unlocked);
    nv_umem_pin_cancel(&pin, 1);
    assert(nv_umem_pin_claim_unlock(&pin));
}

static void
test_failed_revoke_preserves_import(void)
{
    nv_umem_pin_state_t pin = {0};

    nv_umem_pin_state_init(&pin);
    assert(nv_umem_pin_reserve(&pin));
    nv_umem_pin_publish(&pin);
    nv_umem_pin_begin_revoke(&pin);
    assert(pin.imports == 1);
    assert(!nv_umem_pin_can_unlock(&pin));
}

static void
test_hardware_complete_retains_guard_until_final_release(void)
{
    nv_umem_pin_state_t pin = {0};

    nv_umem_pin_state_init(&pin);
    assert(nv_umem_pin_reserve(&pin));
    nv_umem_pin_guard_acquired(&pin);
    nv_umem_pin_publish(&pin);
    nv_umem_pin_begin_revoke(&pin);
    nv_umem_pin_hardware_complete(&pin);
    assert(pin.imports == 0);
    assert(!nv_umem_pin_can_unlock(&pin));
    /* A failed RM guard drop leaves the pin held and owner discoverable. */
    assert(pin.guards == 1);
    nv_umem_pin_guard_released(&pin);
    assert(nv_umem_pin_can_unlock(&pin));
}

typedef struct race_context {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    nv_umem_pin_state_t pin;
    int callback_claimed;
    int allow_completion;
    int hardware_done;
} race_context_t;

static void *
callback_worker(void *arg)
{
    race_context_t *ctx = arg;

    pthread_mutex_lock(&ctx->lock);
    nv_umem_pin_begin_revoke(&ctx->pin);
    ctx->callback_claimed = 1;
    pthread_cond_broadcast(&ctx->cv);
    while (!ctx->allow_completion)
        pthread_cond_wait(&ctx->cv, &ctx->lock);
    nv_umem_pin_hardware_complete(&ctx->pin);
    ctx->hardware_done = 1;
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

static void
test_callback_and_guard_drop_interleave(void)
{
    race_context_t ctx = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    pthread_t callback;
    int thread_status;

    nv_umem_pin_state_init(&ctx.pin);
    assert(nv_umem_pin_reserve(&ctx.pin));
    nv_umem_pin_publish(&ctx.pin);
    nv_umem_pin_guard_acquired(&ctx.pin);
    thread_status = pthread_create(&callback, NULL, callback_worker, &ctx);
    assert(thread_status == 0);

    pthread_mutex_lock(&ctx.lock);
    while (!ctx.callback_claimed)
        pthread_cond_wait(&ctx.cv, &ctx.lock);
    assert(!nv_umem_pin_reserve(&ctx.pin));
    assert(!nv_umem_pin_can_unlock(&ctx.pin));
    ctx.allow_completion = 1;
    pthread_cond_broadcast(&ctx.cv);
    while (!ctx.hardware_done)
        pthread_cond_wait(&ctx.cv, &ctx.lock);
    assert(!nv_umem_pin_can_unlock(&ctx.pin));
    nv_umem_pin_guard_released(&ctx.pin);
    assert(nv_umem_pin_can_unlock(&ctx.pin));
    pthread_mutex_unlock(&ctx.lock);
    thread_status = pthread_join(callback, NULL);
    assert(thread_status == 0);
    pthread_cond_destroy(&ctx.cv);
    pthread_mutex_destroy(&ctx.lock);
}

static void
test_unpublished_duplicate_checked_unwind(void)
{
    nv_umem_pin_state_t pin = {0};

    nv_umem_pin_state_init(&pin);
    assert(nv_umem_pin_reserve(&pin));
    nv_umem_pin_guard_acquired(&pin);
    nv_umem_pin_begin_revoke(&pin);
    assert(pin.pending == 1);
    assert(!nv_umem_pin_can_unlock(&pin));
    /* Only after checked duplicate cleanup may the unpublished token finish. */
    nv_umem_pin_hardware_complete_pending(&pin);
    assert(pin.pending == 0 && pin.imports == 0);
    assert(!nv_umem_pin_can_unlock(&pin));
    nv_umem_pin_guard_released(&pin);
    assert(nv_umem_pin_can_unlock(&pin));
}

static void
test_rm_private_wrapper_identity(void)
{
    int pin;
    int rm_private;
    int unrelated;

    assert(nv_umem_pin_matches(&pin, &rm_private, &pin));
    assert(nv_umem_pin_matches(&pin, &rm_private, &rm_private));
    assert(!nv_umem_pin_matches(&pin, &rm_private, &unrelated));
    assert(!nv_umem_pin_matches(&pin, &rm_private, NULL));
    assert(!nv_umem_pin_matches(&pin, NULL, &rm_private));
}

static void
test_pinned_descriptor_copy_admission(void)
{
    nv_umem_copy_state_t copy = {0};
    int importer;
    int other;

    assert(nv_umem_copy_allowed(0, &copy, &other));
    assert(!nv_umem_copy_allowed(1, &copy, &importer));
    nv_umem_copy_authorize(&copy, &importer);
    assert(nv_umem_copy_allowed(1, &copy, &importer));
    assert(!nv_umem_copy_allowed(1, &copy, &other));
    nv_umem_copy_revoke(&copy);
    assert(!nv_umem_copy_allowed(1, &copy, &importer));
}

static void
test_pinned_descriptor_rm_map_admission(void)
{
    assert(nv_umem_map_allowed(0, 0, 0x20));
    assert(nv_umem_map_allowed(1, 0x10, 0x10));
    assert(!nv_umem_map_allowed(1, 0x10, 0x20));
    assert(!nv_umem_map_allowed(1, 0, 0x20));
}

static void
test_original_owner_claims_once_after_publication(void)
{
    nv_umem_original_state_t owner;

    nv_umem_original_init(&owner);
    assert(!nv_umem_original_claim(&owner));
    assert(nv_umem_original_publish(&owner));
    assert(nv_umem_original_claim(&owner));
    assert(!nv_umem_original_claim(&owner));
    nv_umem_original_begin_close(&owner);
    assert(!nv_umem_original_claim(&owner));
}

static void
test_original_owner_normal_close_beats_callback(void)
{
    nv_umem_original_state_t owner;

    nv_umem_original_init(&owner);
    assert(nv_umem_original_publish(&owner));
    nv_umem_original_begin_close(&owner);
    assert(!nv_umem_original_claim(&owner));
}

static void
test_duplicate_close_does_not_retire_original(void)
{
    nv_umem_original_state_t owner;

    nv_umem_original_init(&owner);
    assert(nv_umem_original_publish(&owner));
    nv_umem_original_begin_close_if_match(&owner, 0x10, 0x20,
                                          0x11, 0x20);
    nv_umem_original_begin_close_if_match(&owner, 0x10, 0x20,
                                          0x10, 0x21);
    assert(nv_umem_original_claim(&owner));
    nv_umem_original_begin_close_if_match(&owner, 0x10, 0x20,
                                          0x10, 0x20);
    assert(!nv_umem_original_claim(&owner));
}

static void
test_failed_original_allocation_cancels_callback_claim(void)
{
    nv_umem_original_state_t owner;

    nv_umem_original_init(&owner);
    nv_umem_original_cancel(&owner);
    assert(!nv_umem_original_publish(&owner));
    assert(!nv_umem_original_claim(&owner));
}

int
main(void)
{
    test_pending_publication_and_revoke();
    test_normal_release_waits_for_pending_owner();
    test_unlock_claim_is_once_and_blocks_new_imports();
    test_unlock_claim_retains_live_pin_until_import_completes();
    test_failed_revoke_preserves_import();
    test_hardware_complete_retains_guard_until_final_release();
    test_callback_and_guard_drop_interleave();
    test_unpublished_duplicate_checked_unwind();
    test_rm_private_wrapper_identity();
    test_pinned_descriptor_copy_admission();
    test_pinned_descriptor_rm_map_admission();
    test_original_owner_claims_once_after_publication();
    test_original_owner_normal_close_beats_callback();
    test_duplicate_close_does_not_retire_original();
    test_failed_original_allocation_cancels_callback_claim();
    puts("nv_umem registry state: 15 tests passed");
    return 0;
}
