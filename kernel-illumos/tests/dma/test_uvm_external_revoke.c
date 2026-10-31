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

/* Each failed UVM barrier must retain the external map for a later retry. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef int NV_STATUS;
typedef int uvm_aperture_t;
typedef int uvm_membar_t;
typedef struct { int used; } uvm_gpu_t;
typedef struct { int used; } uvm_tracker_t;
typedef struct { int ranges; } uvm_page_table_range_vec_t;
typedef struct { int used; } uvm_va_range_t;
typedef struct { uvm_va_range_t va_range; } uvm_va_range_external_t;
typedef struct { int used; } uvm_ext_gpu_mem_handle;
typedef struct {
    struct {
        uvm_gpu_t *gpu;
        uvm_page_table_range_vec_t pt_range_vec;
    } base;
    uvm_ext_gpu_mem_handle *mem_handle;
    uvm_tracker_t tracker;
    bool need_l2_invalidate_at_unmap;
    bool is_sysmem;
    bool is_egm;
    bool pt_release_queued;
    bool pt_revoke_complete;
} uvm_ext_gpu_map_t;

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_ERR_INVALID_ARGUMENT 2
#define UVM_APERTURE_PEER(x) (10 + (x))
#define UVM_APERTURE_SYS 20

static NV_STATUS tracker_status;
static NV_STATUS l2_status;
static NV_STATUS clear_status;
static NV_STATUS drain_status;
static int tracker_waits;
static int tracker_deinits;
static int l2_calls;
static int clear_calls;
static int drain_calls;

static NV_STATUS
uvm_tracker_wait(uvm_tracker_t *tracker)
{
    (void)tracker;
    tracker_waits++;
    return tracker_status;
}

static void
uvm_tracker_deinit(uvm_tracker_t *tracker)
{
    (void)tracker;
    tracker_deinits++;
}

static NV_STATUS
uvm_mmu_l2_invalidate(uvm_gpu_t *gpu, uvm_aperture_t aperture)
{
    (void)gpu;
    assert(aperture == UVM_APERTURE_SYS);
    l2_calls++;
    return l2_status;
}

static uvm_membar_t
va_range_downgrade_membar(uvm_va_range_t *range, uvm_ext_gpu_map_t *map)
{
    (void)range;
    (void)map;
    return 1;
}

static NV_STATUS
uvm_page_table_range_vec_clear_ptes(uvm_page_table_range_vec_t *vec,
                                    uvm_membar_t membar)
{
    assert(vec->ranges && membar == 1);
    clear_calls++;
    return clear_status;
}

static NV_STATUS
uvm_page_table_range_vec_deinit_checked(uvm_page_table_range_vec_t *vec,
                                        bool *queued)
{
    drain_calls++;
    *queued = true;
    if (drain_status != NV_OK)
        return drain_status;
    vec->ranges = 0;
    *queued = false;
    return NV_OK;
}

#include "../../../kernel-open/nvidia-uvm/uvm_map_external_revoke_impl.h"

int
main(void)
{
    uvm_gpu_t gpu = {0};
    uvm_ext_gpu_mem_handle handle = {0};
    uvm_va_range_external_t range = {0};
    uvm_ext_gpu_map_t map = {
        .base = {.gpu = &gpu, .pt_range_vec = {.ranges = 1}},
        .mem_handle = &handle,
        .need_l2_invalidate_at_unmap = true,
        .is_sysmem = true,
    };

    tracker_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_map_revoke_ptes_checked(&range, &map) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(tracker_waits == 1 && l2_calls == 0 && clear_calls == 0);
    assert(map.base.pt_range_vec.ranges && tracker_deinits == 0);

    tracker_status = NV_OK;
    l2_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_map_revoke_ptes_checked(&range, &map) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(l2_calls == 1 && clear_calls == 0 && tracker_deinits == 0);

    l2_status = NV_OK;
    clear_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_map_revoke_ptes_checked(&range, &map) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(clear_calls == 1 && drain_calls == 0 && tracker_deinits == 0);

    clear_status = NV_OK;
    drain_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_map_revoke_ptes_checked(&range, &map) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(clear_calls == 2 && drain_calls == 1 && map.pt_release_queued);
    assert(tracker_deinits == 0 && map.mem_handle == &handle);

    drain_status = NV_OK;
    assert(uvm_ext_gpu_map_revoke_ptes_checked(&range, &map) == NV_OK);
    assert(clear_calls == 2 && drain_calls == 2);
    assert(!map.pt_release_queued && !map.base.pt_range_vec.ranges);
    assert(tracker_deinits == 1 && map.mem_handle == &handle);

    assert(uvm_ext_gpu_map_revoke_ptes_checked(&range, &map) == NV_OK);
    assert(tracker_deinits == 1 && drain_calls == 2);
    return 0;
}
