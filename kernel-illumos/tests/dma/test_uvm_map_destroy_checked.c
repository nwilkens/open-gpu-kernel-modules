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

/* A failed PTE or RM barrier must leave the last external map owned. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef int NV_STATUS;
#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_ERR_INVALID_ARGUMENT 2
#define NV_ERR_INVALID_STATE 3

typedef struct { int id; } uvm_gpu_t;
typedef struct { int ref_count; int failed; } uvm_ext_gpu_mem_handle;
typedef struct { int present; } uvm_range_tree_t;
typedef struct { int lock; uvm_range_tree_t tree; } uvm_ext_gpu_range_tree_t;
typedef struct { int mapped_gpus; } uvm_processor_mask_t;
typedef struct { int used; } uvm_va_space_t;
typedef struct { uvm_va_space_t *va_space; } uvm_va_range_t;
typedef struct {
    uvm_va_range_t va_range;
    uvm_processor_mask_t mapped_gpus;
    uvm_ext_gpu_range_tree_t range_tree;
} uvm_va_range_external_t;
typedef struct {
    struct { uvm_gpu_t *gpu; int node; } base;
    uvm_ext_gpu_mem_handle *mem_handle;
    int ptes_revoked;
    int freed;
} uvm_ext_gpu_map_t;

static NV_STATUS pte_status;
static NV_STATUS rm_status;
static int rm_frees;
static int map_frees;

static NV_STATUS
uvm_ext_gpu_map_revoke_ptes_checked(uvm_va_range_external_t *range,
                                     uvm_ext_gpu_map_t *map)
{
    (void)range;
    if (pte_status == NV_OK)
        map->ptes_revoked = 1;
    return pte_status;
}

static NV_STATUS
uvm_release_rm_handle_checked(uvm_ext_gpu_mem_handle *handle)
{
    assert(handle->ref_count == 1);
    rm_frees++;
    if (rm_status != NV_OK) {
        handle->failed = 1;
        return rm_status;
    }
    handle->ref_count = 0;
    return NV_OK;
}

static void nv_kref_put(int *refs, void (*release)(int *))
{
    (void)release;
    assert(*refs > 1);
    --*refs;
}
static void uvm_release_rm_handle(int *refs) { (void)refs; }
static int nv_kref_read(int *refs) { return *refs; }
static uvm_ext_gpu_range_tree_t *uvm_ext_gpu_range_tree(uvm_va_range_external_t *range,
                                                          uvm_gpu_t *gpu)
{
    (void)gpu;
    return &range->range_tree;
}
static void uvm_assert_mutex_locked(int *lock) { assert(*lock); }
static void uvm_range_tree_remove(uvm_range_tree_t *tree, int *node)
{
    assert(*node && tree->present);
    *node = 0;
    tree->present--;
}
static int uvm_range_tree_empty(uvm_range_tree_t *tree) { return tree->present == 0; }
static void uvm_processor_mask_clear_atomic(uvm_processor_mask_t *mask, int id)
{
    (void)id;
    mask->mapped_gpus = 0;
}
static uvm_gpu_t *uvm_ext_gpu_map_free_internal(uvm_ext_gpu_map_t *map)
{
    assert(map->mem_handle == NULL);
    map->freed = 1;
    map_frees++;
    return NULL;
}

#include "../../../kernel-open/nvidia-uvm/uvm_map_external_destroy_checked_impl.h"

int
main(void)
{
    uvm_gpu_t gpu = {1};
    uvm_ext_gpu_mem_handle handle = {2, 0};
    uvm_va_space_t va = {0};
    uvm_va_range_external_t range = {{&va}, {1}, {1, {2}}};
    uvm_ext_gpu_map_t first = {{&gpu, 1}, &handle, 0, 0};
    uvm_ext_gpu_map_t last = {{&gpu, 1}, &handle, 0, 0};

    assert(uvm_ext_gpu_map_destroy_checked(&range, NULL) == NV_ERR_INVALID_ARGUMENT);

    pte_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_map_destroy_checked(&range, &first) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(handle.ref_count == 2 && range.range_tree.tree.present == 2);
    assert(!first.freed && rm_frees == 0);

    pte_status = NV_OK;
    assert(uvm_ext_gpu_map_destroy_checked(&range, &first) == NV_OK);
    assert(handle.ref_count == 1 && range.range_tree.tree.present == 1);
    assert(first.freed && !last.freed && rm_frees == 0);

    rm_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_map_destroy_checked(&range, &last) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(last.ptes_revoked && !last.freed && last.mem_handle == &handle);
    assert(handle.ref_count == 1 && handle.failed && rm_frees == 1);
    assert(range.range_tree.tree.present == 1 && map_frees == 1);

    {
        uvm_ext_gpu_mem_handle clean_handle = {1, 0};
        uvm_va_range_external_t clean_range = {{&va}, {1}, {1, {1}}};
        uvm_ext_gpu_map_t clean_map = {{&gpu, 1}, &clean_handle, 0, 0};

        rm_status = NV_OK;
        assert(uvm_ext_gpu_map_destroy_checked(&clean_range, &clean_map) == NV_OK);
        assert(clean_map.freed && clean_handle.ref_count == 0);
        assert(clean_range.range_tree.tree.present == 0);
        assert(clean_range.mapped_gpus.mapped_gpus == 0);
        assert(rm_frees == 2 && map_frees == 2);
    }
    return 0;
}
