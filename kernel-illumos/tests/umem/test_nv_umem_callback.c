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
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NV_UMEM_USERLAND_TEST 1
#include "../../nvidia/nvidia_umem.c"

typedef struct test_pin {
    nv_user_pages_t *up;
    struct as as;
    struct ddi_umem_cookie cookie;
} test_pin_t;

static void
pin_register(test_pin_t *pin)
{
    pin->up = calloc(1, sizeof (*pin->up));
    assert(pin->up != NULL);
    pin->cookie.asp = &pin->as;
    pin->up->cookie = &pin->cookie;
    pin->up->as = &pin->as;
    pin->up->seq = ++nv_umem_seq;
    pin->up->refs = 1;
    pin->up->indexed = B_TRUE;
    nv_umem_original_init(&pin->up->original);
    nv_umem_pin_state_init(&pin->up->state);
    list_create(&pin->up->imports, sizeof (nv_umem_import_token_t),
        offsetof(nv_umem_import_token_t, link));
    list_insert_tail(&nv_umem_pins, pin->up);
}

static void
pin_start(test_pin_t *pin)
{
    assert(nv_umem_init() == 0);
    pin_register(pin);
}

static void
pin_unregister(test_pin_t *pin)
{
    assert(pin->up->refs == 1);
    assert(list_is_empty(&pin->up->imports));
    if (pin->up->indexed)
        list_remove(&nv_umem_pins, pin->up);
    list_destroy(&pin->up->imports);
    free(pin->up);
}

static void
pin_finish(test_pin_t *pin)
{
    pin_unregister(pin);
    nv_umem_fini();
}

static void
wait_until(int (*predicate)(void *), void *arg)
{
    struct timespec pause = {0, 1000000};
    unsigned int i;

    for (i = 0; i < 2000; i++) {
        if (predicate(arg))
            return;
        nanosleep(&pause, NULL);
    }
    assert(0 && "timed out waiting for callback state");
}

static int
revoking(void *arg)
{
    nv_user_pages_t *up = arg;
    int value;

    mutex_enter(&nv_umem_lock);
    value = up->state.revoking;
    mutex_exit(&nv_umem_lock);
    return value;
}

static int
hardware_complete(void *arg)
{
    nv_user_pages_t *up = arg;
    int value;

    mutex_enter(&nv_umem_lock);
    value = up->state.imports == 0;
    mutex_exit(&nv_umem_lock);
    return value;
}

static int
callback_has_waited(void *arg)
{
    (void)arg;
    return atomic_load(&umem_test_cv_wait_calls) != 0;
}

static int
error_logged(void *arg)
{
    (void)arg;
    return atomic_load(&umem_test_error_logs) != 0;
}

static void *
cleanup_thread(void *arg)
{
    test_pin_t *pin = arg;

    nv_umem_lock_cleanup((ddi_umem_cookie_t *)&pin->cookie);
    return NULL;
}

typedef struct cleanup_call {
    test_pin_t *pin;
    atomic_int done;
} cleanup_call_t;

static void *
cleanup_with_done_thread(void *arg)
{
    cleanup_call_t *call = arg;

    nv_umem_lock_cleanup((ddi_umem_cookie_t *)&call->pin->cookie);
    atomic_store(&call->done, 1);
    return NULL;
}

typedef struct failed_revoke {
    atomic_int calls;
    NV_STATUS status;
} failed_revoke_t;

static NV_STATUS
inject_revoke_failure(void *arg)
{
    failed_revoke_t *failure = arg;

    atomic_fetch_add(&failure->calls, 1);
    return failure->status;
}

static int
revoke_failed(void *arg)
{
    return atomic_load(&((failed_revoke_t *)arg)->calls) != 0;
}

static NV_STATUS
complete_revoke(void *arg)
{
    return os_umem_import_hardware_complete(arg);
}

typedef struct revoke_gate {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    void *token;
    int entered;
    int release;
} revoke_gate_t;

static NV_STATUS
gated_revoke(void *arg)
{
    revoke_gate_t *gate = arg;

    assert(pthread_mutex_lock(&gate->lock) == 0);
    gate->entered = 1;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    while (!gate->release)
        assert(pthread_cond_wait(&gate->cv, &gate->lock) == 0);
    assert(pthread_mutex_unlock(&gate->lock) == 0);
    return os_umem_import_hardware_complete(gate->token);
}

typedef struct cancel_call {
    void *token;
    atomic_int done;
} cancel_call_t;

static void *
cancel_thread(void *arg)
{
    cancel_call_t *call = arg;

    os_umem_import_cancel(call->token);
    atomic_store(&call->done, 1);
    return NULL;
}

static int
cancel_waiting(void *arg)
{
    nv_umem_import_token_t *token = ((cancel_call_t *)arg)->token;
    int waiting;

    mutex_enter(&nv_umem_lock);
    waiting = token->in_callback && token->refs == 4;
    mutex_exit(&nv_umem_lock);
    return waiting;
}

typedef struct original_owner {
    nv_user_pages_t *up;
    int calls;
} original_owner_t;

static NV_STATUS
free_original(NvU32 client, NvU32 handle, void *arg)
{
    original_owner_t *owner = arg;

    assert(client == 0x12 && handle == 0x34);
    owner->calls++;
    os_umem_original_begin_close(owner, client, handle);
    mutex_enter(&nv_umem_lock);
    nv_umem_pin_mark_unlocked(&owner->up->state);
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    return NV_OK;
}

static void
test_publication_and_cancellation_change_real_registry(void)
{
    test_pin_t pin = {0};
    void *token = NULL;
    int descriptor;

    pin_start(&pin);
    assert(os_umem_import_reserve(pin.up, &descriptor, 9, &token) == NV_OK);
    assert(token != NULL);
    assert(pin.up->state.pending == 1 && pin.up->state.imports == 1);
    assert(os_umem_import_publish(token, token, complete_revoke) == NV_OK);
    assert(pin.up->state.pending == 0 && pin.up->state.imports == 1);
    os_umem_import_cancel(token);
    assert(pin.up->state.imports == 0);
    assert(list_is_empty(&pin.up->imports));
    pin_finish(&pin);
}

static void
test_callback_waits_for_publication_and_owner_unlock(void)
{
    test_pin_t pin = {0};
    void *token = NULL;
    pthread_t thread;
    int descriptor;

    pin_start(&pin);
    assert(os_umem_import_reserve(pin.up, &descriptor, 9, &token) == NV_OK);
    assert(pthread_create(&thread, NULL, cleanup_thread, &pin) == 0);
    wait_until(revoking, pin.up);
    assert(pin.up->state.pending == 1 && pin.up->state.imports == 1);
    assert(os_umem_import_publish(token, token, complete_revoke) == NV_OK);
    wait_until(hardware_complete, pin.up);
    assert(pin.up->callback_seen);
    assert(pin.up->state.unlocked == 0);
    mutex_enter(&nv_umem_lock);
    nv_umem_pin_mark_unlocked(&pin.up->state);
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    assert(pthread_join(thread, NULL) == 0);
    os_umem_import_cancel(token);
    pin_finish(&pin);
}

static void
test_callback_waits_for_cookie_publication(void)
{
    test_pin_t pin = {0};
    pthread_t thread;

    pin_start(&pin);
    pin.up->cookie = NULL;
    pin.up->pending = B_TRUE;
    atomic_store(&umem_test_cv_wait_calls, 0);
    assert(pthread_create(&thread, NULL, cleanup_thread, &pin) == 0);
    wait_until(callback_has_waited, NULL);
    mutex_enter(&nv_umem_lock);
    assert(!pin.up->callback_seen);
    pin.up->cookie = &pin.cookie;
    pin.up->pending = B_FALSE;
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    wait_until(revoking, pin.up);
    mutex_enter(&nv_umem_lock);
    nv_umem_pin_mark_unlocked(&pin.up->state);
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    assert(pthread_join(thread, NULL) == 0);
    assert(pin.up->callback_seen);
    pin_finish(&pin);
}

static void
test_cancel_waits_for_callback_borrower(void)
{
    test_pin_t pin = {0};
    revoke_gate_t gate = {0};
    cancel_call_t cancel = {0};
    pthread_t callback_thread, cancellation_thread;
    void *token = NULL;
    int descriptor;

    pin_start(&pin);
    assert(pthread_mutex_init(&gate.lock, NULL) == 0);
    assert(pthread_cond_init(&gate.cv, NULL) == 0);
    assert(os_umem_import_reserve(pin.up, &descriptor, 9, &token) == NV_OK);
    gate.token = token;
    assert(os_umem_import_publish(token, &gate, gated_revoke) == NV_OK);
    assert(pthread_create(&callback_thread, NULL, cleanup_thread, &pin) == 0);
    assert(pthread_mutex_lock(&gate.lock) == 0);
    while (!gate.entered)
        assert(pthread_cond_wait(&gate.cv, &gate.lock) == 0);
    assert(pthread_mutex_unlock(&gate.lock) == 0);

    cancel.token = token;
    assert(pthread_create(&cancellation_thread, NULL,
        cancel_thread, &cancel) == 0);
    wait_until(cancel_waiting, &cancel);
    mutex_enter(&nv_umem_lock);
    assert(((nv_umem_import_token_t *)token)->in_callback);
    assert(((nv_umem_import_token_t *)token)->linked);
    assert(!atomic_load(&cancel.done));
    mutex_exit(&nv_umem_lock);

    assert(pthread_mutex_lock(&gate.lock) == 0);
    gate.release = 1;
    assert(pthread_cond_broadcast(&gate.cv) == 0);
    assert(pthread_mutex_unlock(&gate.lock) == 0);
    assert(pthread_join(cancellation_thread, NULL) == 0);
    mutex_enter(&nv_umem_lock);
    assert(pin.up->state.imports == 0);
    nv_umem_pin_mark_unlocked(&pin.up->state);
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    assert(pthread_join(callback_thread, NULL) == 0);
    assert(pthread_cond_destroy(&gate.cv) == 0);
    assert(pthread_mutex_destroy(&gate.lock) == 0);
    pin_finish(&pin);
}

static void
test_callback_claims_checked_original_once(void)
{
    test_pin_t pin = {0};
    original_owner_t owner = {0};

    pin_start(&pin);
    owner.up = pin.up;
    assert(os_umem_original_publish(pin.up, &owner,
        0x12, 0x34, free_original) == NV_OK);
    nv_umem_lock_cleanup((ddi_umem_cookie_t *)&pin.cookie);
    assert(owner.calls == 1);
    assert(pin.up->state.unlocked);
    assert(pin.up->original.phase == NV_UMEM_ORIGINAL_CLOSING);
    pin_finish(&pin);
}

static NV_STATUS
unused_original_free(NvU32 client, NvU32 handle, void *mem_desc)
{
    (void)client;
    (void)handle;
    (void)mem_desc;
    assert(0 && "admission test must not free the original");
    return NV_ERR_INVALID_STATE;
}

static void *
unrelated_copy_thread(void *arg)
{
    void *mem_desc = arg;

    assert(!os_umem_copy_allowed(mem_desc));
    return NULL;
}

static void
test_copy_and_map_admission_use_real_registry(void)
{
    test_pin_t pin = {0};
    void *token = NULL;
    pthread_t other;
    int descriptor;

    pin_start(&pin);
    assert(os_umem_original_publish(pin.up, &descriptor, 0x12, 0x34,
        unused_original_free) == NV_OK);
    assert(!os_umem_copy_allowed(&descriptor));
    assert(!os_umem_map_allowed(&descriptor, 9));

    assert(os_umem_import_reserve(pin.up, &descriptor, 9, &token) == NV_OK);
    assert(token != NULL);
    os_umem_import_copy_window(token, NV_TRUE);
    assert(os_umem_copy_allowed(&descriptor));
    assert(pthread_create(&other, NULL, unrelated_copy_thread,
        &descriptor) == 0);
    assert(pthread_join(other, NULL) == 0);
    os_umem_import_copy_window(token, NV_FALSE);
    assert(!os_umem_copy_allowed(&descriptor));
    assert(os_umem_map_allowed(&descriptor, 9));
    assert(!os_umem_map_allowed(&descriptor, 10));

    os_umem_import_cancel(token);
    assert(!os_umem_map_allowed(&descriptor, 9));
    pin_finish(&pin);
}

static void
test_failed_revoke_retains_pin_and_isolates_other_owner(void)
{
    test_pin_t failed = {0}, other = {0};
    cleanup_call_t call = {0};
    failed_revoke_t failure = {.status = NV_ERR_INVALID_STATE};
    pthread_t failed_thread, other_thread;
    void *failed_token = NULL, *other_token = NULL;
    int failed_descriptor, other_descriptor;

    pin_start(&failed);
    pin_register(&other);
    assert(os_umem_import_reserve(failed.up, &failed_descriptor, 9,
        &failed_token) == NV_OK);
    assert(os_umem_import_publish(failed_token, &failure,
        inject_revoke_failure) == NV_OK);
    atomic_store(&umem_test_cv_wait_calls, 0);
    atomic_store(&umem_test_error_logs, 0);
    umem_test_last_error[0] = '\0';
    call.pin = &failed;
    assert(pthread_create(&failed_thread, NULL, cleanup_with_done_thread,
        &call) == 0);
    wait_until(revoke_failed, &failure);
    wait_until(callback_has_waited, NULL);
    wait_until(error_logged, NULL);
    assert(strstr(umem_test_last_error, "pin_seq=") != NULL);
    assert(strstr(umem_test_last_error, "client=0x9") != NULL);
    mutex_enter(&nv_umem_lock);
    assert(failed.up->callback_seen && failed.up->indexed);
    assert(failed.up->state.imports == 1);
    assert(!failed.up->state.unlocked);
    assert(!nv_umem_pin_can_unlock(&failed.up->state));
    assert(((nv_umem_import_token_t *)failed_token)->linked);
    mutex_exit(&nv_umem_lock);
    assert(!atomic_load(&call.done));

    /* A failed owner must not prevent an independent owner from finishing. */
    assert(os_umem_import_reserve(other.up, &other_descriptor, 9,
        &other_token) == NV_OK);
    assert(os_umem_import_publish(other_token, other_token,
        complete_revoke) == NV_OK);
    assert(pthread_create(&other_thread, NULL, cleanup_thread, &other) == 0);
    wait_until(hardware_complete, other.up);
    mutex_enter(&nv_umem_lock);
    nv_umem_pin_mark_unlocked(&other.up->state);
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    assert(pthread_join(other_thread, NULL) == 0);
    os_umem_import_cancel(other_token);
    pin_unregister(&other);
    assert(!atomic_load(&call.done));
    assert(nv_umem_busy());

    /* Model a later checked recovery and release the test's retained owner. */
    assert(os_umem_import_hardware_complete(failed_token) == NV_OK);
    mutex_enter(&nv_umem_lock);
    nv_umem_pin_mark_unlocked(&failed.up->state);
    cv_broadcast(&nv_umem_cv);
    mutex_exit(&nv_umem_lock);
    assert(pthread_join(failed_thread, NULL) == 0);
    os_umem_import_cancel(failed_token);
    pin_finish(&failed);
}

int
main(void)
{
    assert(nv_umem_callbacks.cbo_umem_callback_version ==
        UMEM_CALLBACK_VERSION);
    assert(nv_umem_callbacks.cbo_umem_lock_cleanup ==
        nv_umem_lock_cleanup);
    test_publication_and_cancellation_change_real_registry();
    test_callback_waits_for_publication_and_owner_unlock();
    test_callback_waits_for_cookie_publication();
    test_cancel_waits_for_callback_borrower();
    test_callback_claims_checked_original_once();
    test_copy_and_map_admission_use_real_registry();
    test_failed_revoke_retains_pin_and_isolates_other_owner();
    puts("nv_umem callback: 7 tests passed");
    return 0;
}
