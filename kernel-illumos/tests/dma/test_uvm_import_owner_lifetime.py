#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Carlos Neira
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
"""Exercise the real owner finalizer under a forced last-owner/destroy race."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "kernel-open/nvidia-uvm/uvm_map_external.c"
VA_SOURCE = ROOT / "kernel-open/nvidia-uvm/uvm_va_space.c"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError(f"unterminated function: {signature}")


finish = function_body(SOURCE.read_text(), "static void uvm_import_owner_finish(")
resume = function_body(VA_SOURCE.read_text(), "void uvm_va_space_resume_import_release(")
harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct { int value; } atomic_t;
typedef struct { pthread_mutex_t mutex; } uvm_spinlock_t;
typedef struct { int unused; } nv_kthread_q_item_t;
typedef struct { int unused; } uvm_gpu_t;
typedef struct { int unused; } list_node_t;
typedef struct {
    uvm_spinlock_t import_owner_lock;
    atomic_t import_owner_count;
    atomic_t import_release_pending;
    nv_kthread_q_item_t import_resume_release_q_item;
    int alive;
} uvm_va_space_t;
struct uvm_ext_gpu_import_owner {
    uvm_va_space_t *va_space;
    uvm_gpu_t *gpu;
    list_node_t va_node;
    nv_kthread_q_item_t finalize_item;
};
static uvm_va_space_t space;
static pthread_mutex_t step_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t step_cv = PTHREAD_COND_INITIALIZER;
static int dropped;
static int destroy_attempted;
static int use_after_free;
static int release_queued;
static int destroy_got_lock_before_finish;
static struct { int deferred_release_q; } g_uvm_global;

static void uvm_spin_lock(uvm_spinlock_t *lock) { assert(pthread_mutex_lock(&lock->mutex) == 0); }
static void uvm_spin_unlock(uvm_spinlock_t *lock) { assert(pthread_mutex_unlock(&lock->mutex) == 0); }
static void list_del_init(list_node_t *node) { (void)node; }
static void uvm_gpu_release(uvm_gpu_t *gpu) { (void)gpu; }
static void uvm_import_owner_set_state(struct uvm_ext_gpu_import_owner *owner, int state) { (void)owner; (void)state; }
static void uvm_import_owner_put(struct uvm_ext_gpu_import_owner *owner) { (void)owner; }
static void uvm_ext_gpu_import_owner_finalize(void *owner) { (void)owner; }
static void nv_kthread_q_item_init(nv_kthread_q_item_t *item, void (*fn)(void *), void *arg)
{ (void)item; (void)fn; (void)arg; }
static int nv_kthread_q_schedule_q_item(int *q, nv_kthread_q_item_t *item)
{ (void)q; (void)item; if (!space.alive) use_after_free = 1; ++release_queued; return 1; }
static __attribute__((unused)) int atomic_read(atomic_t *item)
{
    if ((item == &space.import_release_pending || item == &space.import_owner_count) && !space.alive)
        use_after_free = 1;
    return item->value;
}
static __attribute__((unused)) void atomic_set(atomic_t *item, int value) { item->value = value; }
static __attribute__((unused)) int atomic_cmpxchg(atomic_t *item, int old, int value)
{ int prior = item->value; if (prior == old) item->value = value; return prior; }
static int atomic_dec_and_test(atomic_t *item)
{
    --item->value;
    if (item == &space.import_owner_count) {
        pthread_mutex_lock(&step_lock);
        dropped = 1;
        pthread_cond_broadcast(&step_cv);
        while (!destroy_attempted)
            pthread_cond_wait(&step_cv, &step_lock);
        pthread_mutex_unlock(&step_lock);
    }
    return item->value == 0;
}
static int uvm_va_space_has_tracked_imports(uvm_va_space_t *va_space)
{ return atomic_read(&va_space->import_owner_count) != 0; }
#define UVM_IMPORT_COMPLETE 1
#define UVM_ERR_PRINT(...) ((void)0)

''' + resume + "\n" + finish + r'''

static void *finish_thread(void *arg)
{ uvm_import_owner_finish(arg, false); return NULL; }

static void *destroy_thread(void *arg)
{
    int got_lock;
    (void)arg;
    pthread_mutex_lock(&step_lock);
    while (!dropped)
        pthread_cond_wait(&step_cv, &step_lock);
    pthread_mutex_unlock(&step_lock);

    got_lock = pthread_mutex_trylock(&space.import_owner_lock.mutex) == 0;
    if (got_lock)
        space.alive = 0;
    pthread_mutex_lock(&step_lock);
    destroy_got_lock_before_finish = got_lock;
    destroy_attempted = 1;
    pthread_cond_broadcast(&step_cv);
    pthread_mutex_unlock(&step_lock);
    if (!got_lock)
        uvm_spin_lock(&space.import_owner_lock);
    assert(space.import_owner_count.value == 0);
    space.alive = 0; /* VA-space destruction after observing zero. */
    uvm_spin_unlock(&space.import_owner_lock);
    return NULL;
}

static int run_scenario(int pending)
{
    struct uvm_ext_gpu_import_owner owner;
    pthread_t finalizer, destroyer;
    dropped = 0;
    destroy_attempted = 0;
    use_after_free = 0;
    release_queued = 0;
    destroy_got_lock_before_finish = 0;
    assert(pthread_mutex_init(&space.import_owner_lock.mutex, NULL) == 0);
    space.import_owner_count.value = 1;
    space.import_release_pending.value = pending;
    space.alive = 1;
    owner.va_space = &space;
    owner.gpu = NULL;
    assert(pthread_create(&finalizer, NULL, finish_thread, &owner) == 0);
    assert(pthread_create(&destroyer, NULL, destroy_thread, NULL) == 0);
    assert(pthread_join(finalizer, NULL) == 0);
    assert(pthread_join(destroyer, NULL) == 0);
    assert(pthread_mutex_destroy(&space.import_owner_lock.mutex) == 0);
    if (destroy_got_lock_before_finish || use_after_free || release_queued != pending) {
        fprintf(stderr, "last-owner race (pending=%d): destroy_bypassed_lock=%d use_after_free=%d queued=%d\n",
                pending, destroy_got_lock_before_finish, use_after_free, release_queued);
        return 1;
    }
    return 0;
}

int main(void)
{ return run_scenario(0) || run_scenario(1); }
'''

with tempfile.TemporaryDirectory() as temp:
    c_file = Path(temp) / "owner_race.c"
    binary = Path(temp) / "owner_race"
    c_file.write_text(harness)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
