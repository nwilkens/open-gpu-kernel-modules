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

/* A failed RM free must retain the UVM handle and prevent an unsafe retry. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef int NV_STATUS;
typedef unsigned int NvHandle;
typedef int NvBool;
typedef struct { int unused; } uvm_gpu_t;
typedef struct { unsigned int refcount; } nv_kref_t;
typedef struct {
    uvm_gpu_t *gpu;
    NvHandle rm_handle;
    nv_kref_t ref_count;
    bool checked_release_failed;
    NvBool checked_iova_released;
} uvm_ext_gpu_mem_handle;

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_ERR_INVALID_STATE 2
#define uvm_rm_locked_call(expr) (expr)
#define uvm_gpu_device_handle(gpu) (gpu)

static NV_STATUS free_status;
static int free_calls;
static int release_calls;

static unsigned int
nv_kref_read(const nv_kref_t *ref)
{
    return ref->refcount;
}

static NV_STATUS
nvUvmInterfaceFreeDupedHandleChecked(uvm_gpu_t *gpu, NvHandle handle,
                                     NvBool *iova_released)
{
    assert(gpu != NULL && handle != 0);
    assert(iova_released != NULL);
    *iova_released = 1;
    free_calls++;
    return free_status;
}

static void
uvm_release_rm_handle(nv_kref_t *ref)
{
    (void)ref;
    release_calls++;
}

static int
nv_kref_put(nv_kref_t *ref, void (*release)(nv_kref_t *))
{
    assert(ref->refcount > 0);
    if (--ref->refcount != 0)
        return 0;
    release(ref);
    return 1;
}

#include "../../../kernel-open/nvidia-uvm/uvm_map_external_handle_checked_impl.h"

int
main(void)
{
    uvm_gpu_t gpu = {0};
    uvm_ext_gpu_mem_handle shared = {
        .gpu = &gpu, .rm_handle = 4, .ref_count = {.refcount = 2},
    };
    uvm_ext_gpu_mem_handle failed = {
        .gpu = &gpu, .rm_handle = 5, .ref_count = {.refcount = 1},
    };
    uvm_ext_gpu_mem_handle success = {
        .gpu = &gpu, .rm_handle = 6, .ref_count = {.refcount = 1},
    };

    assert(uvm_release_rm_handle_checked(&shared) == NV_ERR_INVALID_STATE);
    assert(shared.ref_count.refcount == 2 && free_calls == 0);

    free_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_release_rm_handle_checked(&failed) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(failed.checked_release_failed && failed.checked_iova_released &&
           failed.rm_handle == 5);
    assert(failed.ref_count.refcount == 1 && release_calls == 0);
    assert(uvm_release_rm_handle_checked(&failed) == NV_ERR_INVALID_STATE);
    assert(free_calls == 1);

    free_status = NV_OK;
    assert(uvm_release_rm_handle_checked(&success) == NV_OK);
    assert(success.rm_handle == 0 && success.ref_count.refcount == 0 &&
           success.checked_iova_released);
    assert(free_calls == 2 && release_calls == 1);
    return 0;
}
