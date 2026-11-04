/*
 * SPDX-FileCopyrightText: Copyright 2026 Edgecast Cloud LLC
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

/* Targeted revocation must retire every split fragment or keep the pin. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef int NV_STATUS;
#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_ERR_INVALID_STATE 2
#define UVM_VA_RANGE_TYPE_EXTERNAL 3

typedef struct uvm_gpu { int id; struct uvm_gpu *next; } uvm_gpu_t;
typedef struct { int ref_count; } uvm_ext_gpu_mem_handle;
typedef struct uvm_ext_gpu_map {
    uvm_ext_gpu_mem_handle *mem_handle;
    struct uvm_ext_gpu_map *next;
    bool freed;
} uvm_ext_gpu_map_t;
typedef struct { int lock; uvm_ext_gpu_map_t *head; } uvm_ext_gpu_range_tree_t;
typedef struct uvm_va_range {
    int type;
    struct { unsigned long start; unsigned long end; } node;
    struct uvm_va_range *next;
} uvm_va_range_t;
typedef struct {
    uvm_va_range_t va_range;
    uvm_ext_gpu_range_tree_t trees[2];
} uvm_va_range_external_t;
typedef struct {
    int locked;
    uvm_gpu_t *gpu;
    uvm_va_range_t *first;
    bool disallow_new_registers;
    int tracked_imports_blocked;
    bool channels_revoked;
} uvm_va_space_t;

static NV_STATUS stop_result;
static NV_STATUS drain_result;
static NV_STATUS map_result;
static int stop_calls;
static int drain_calls;
static int destroy_calls;

static void uvm_va_space_down_read_rm(uvm_va_space_t *vs)
{
    assert(!vs->locked);
    vs->locked = 1;
}
static void uvm_va_space_up_read_rm(uvm_va_space_t *vs)
{
    assert(vs->locked);
    vs->locked = 0;
}
static void uvm_va_space_free_deferred_external_ranges(uvm_va_space_t *vs)
{
    assert(!vs->locked);
}
static int atomic_read(int *value) { return *value; }
static NV_STATUS uvm_va_space_revoke_all_user_channels_checked(uvm_va_space_t *vs)
{
    assert(vs->locked);
    ++stop_calls;
    if (stop_result == NV_OK)
        vs->channels_revoked = true;
    return stop_result;
}
static NV_STATUS uvm_va_space_quiesce_user_faults_checked(uvm_va_space_t *vs)
{
    assert(!vs->locked && vs->channels_revoked);
    ++drain_calls;
    return drain_result;
}
static uvm_ext_gpu_range_tree_t *uvm_ext_gpu_range_tree(uvm_va_range_external_t *r,
                                                         uvm_gpu_t *gpu)
{
    return &r->trees[gpu->id];
}
static void uvm_mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void uvm_mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static uvm_ext_gpu_map_t *uvm_ext_gpu_map_iter_first(uvm_va_range_external_t *r,
                                                      uvm_gpu_t *gpu, unsigned long start,
                                                      unsigned long end)
{
    (void)start;
    (void)end;
    return r->trees[gpu->id].head;
}
static uvm_ext_gpu_map_t *uvm_ext_gpu_map_iter_next(uvm_va_range_external_t *r,
                                                     uvm_ext_gpu_map_t *map,
                                                     unsigned long end)
{
    (void)r;
    (void)end;
    return map->next;
}
static int nv_kref_read(int *refs) { return *refs; }
static NV_STATUS uvm_ext_gpu_map_destroy_checked(uvm_va_range_external_t *r,
                                                   uvm_ext_gpu_map_t *map)
{
    uvm_ext_gpu_range_tree_t *tree = &r->trees[0];
    uvm_ext_gpu_map_t **slot = &tree->head;

    assert(tree->lock);
    ++destroy_calls;
    if (map_result != NV_OK)
        return map_result;
    while (*slot != map) {
        assert(*slot != NULL);
        slot = &(*slot)->next;
    }
    *slot = map->next;
    --map->mem_handle->ref_count;
    map->freed = true;
    return NV_OK;
}

#define uvm_for_each_va_range(range, vs) \
    for ((range) = (vs)->first; (range) != NULL; (range) = (range)->next)
#define for_each_va_space_gpu(gpu, vs) \
    for ((gpu) = (vs)->gpu; (gpu) != NULL; (gpu) = (gpu)->next)
#define uvm_va_range_to_external(range) ((uvm_va_range_external_t *)(range))

#include "../../../kernel-open/nvidia-uvm/uvm_map_external_import_checked_impl.h"

int main(void)
{
    uvm_gpu_t gpu = {0, NULL};
    uvm_ext_gpu_mem_handle target = {2}, unrelated = {1};
    uvm_ext_gpu_map_t second = {&target, NULL, false};
    uvm_ext_gpu_map_t other = {&unrelated, &second, false};
    uvm_ext_gpu_map_t first = {&target, &other, false};
    uvm_va_range_external_t range = {{UVM_VA_RANGE_TYPE_EXTERNAL, {0, 4095}, NULL}, {{0, &first}}};
    uvm_va_space_t vs = {.gpu = &gpu, .first = &range.va_range};
    uvm_ext_gpu_mem_handle live_target = {1};
    uvm_ext_gpu_map_t live_map = {&live_target, NULL, false};
    uvm_va_range_external_t live_range = {{UVM_VA_RANGE_TYPE_EXTERNAL, {0, 4095}, NULL}, {{0, &live_map}}};
    uvm_va_space_t live_vs = {.gpu = &gpu, .first = &live_range.va_range};
    uvm_ext_gpu_mem_handle release_target = {1};
    uvm_ext_gpu_map_t release_map = {&release_target, NULL, false};
    uvm_va_range_external_t release_range = {{UVM_VA_RANGE_TYPE_EXTERNAL, {0, 4095}, NULL}, {{0, &release_map}}};
    uvm_va_space_t release_vs = {
        .gpu = &gpu, .first = &release_range.va_range,
        .tracked_imports_blocked = 1,
    };

    vs.disallow_new_registers = true;

    stop_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_import_revoke_checked(&vs, &target) == NV_ERR_OPERATING_SYSTEM);
    assert(!first.freed && !second.freed && destroy_calls == 0 && drain_calls == 0);

    stop_result = NV_OK;
    drain_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_import_revoke_checked(&vs, &target) == NV_ERR_OPERATING_SYSTEM);
    assert(!first.freed && !second.freed && destroy_calls == 0);

    drain_result = NV_OK;
    map_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_import_revoke_checked(&vs, &target) == NV_ERR_OPERATING_SYSTEM);
    assert(!first.freed && target.ref_count == 2);

    map_result = NV_OK;
    assert(uvm_ext_gpu_import_revoke_checked(&vs, &target) == NV_OK);
    assert(first.freed && second.freed && !other.freed);
    assert(target.ref_count == 0 && unrelated.ref_count == 1 && destroy_calls == 3);

    /* A detached fragment is still an owner; a partial scan cannot certify it. */
    target.ref_count = 2;
    first.freed = false;
    first.next = &other;
    other.next = NULL;
    range.trees[0].head = &first;
    assert(uvm_ext_gpu_import_revoke_checked(&vs, &target) == NV_ERR_INVALID_STATE);
    assert(first.freed && target.ref_count == 1);

    /* Releasing one live host mapping must leave unrelated CUDA channels usable. */
    stop_calls = drain_calls = 0;
    stop_result = drain_result = NV_ERR_OPERATING_SYSTEM;
    assert(atomic_read(&live_vs.tracked_imports_blocked) == 0);
    map_result = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_import_revoke_checked(&live_vs, &live_target) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(!live_map.freed && live_target.ref_count == 1);
    assert(!live_vs.channels_revoked && stop_calls == 0 && drain_calls == 0);

    map_result = NV_OK;
    assert(uvm_ext_gpu_import_revoke_checked(&live_vs, &live_target) == NV_OK);
    assert(live_map.freed && live_target.ref_count == 0);
    assert(!live_vs.channels_revoked && stop_calls == 0 && drain_calls == 0);

    /* File release still uses the full channel and fault-worker barrier. */
    stop_result = drain_result = NV_OK;
    assert(uvm_ext_gpu_import_revoke_checked(&release_vs, &release_target) == NV_OK);
    assert(release_map.freed && release_target.ref_count == 0);
    assert(release_vs.channels_revoked && stop_calls == 1 && drain_calls == 1);

    return 0;
}
