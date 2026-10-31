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

/* A failed RM unset must leave the page directory recorded as installed. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef int NV_STATUS;
typedef struct { int lock; } uvm_va_space_t;
typedef struct {
    uvm_va_space_t *va_space;
    bool did_set_page_directory;
    void *duped_gpu_va_space;
    int state;
} uvm_gpu_va_space_t;

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define UVM_GPU_VA_SPACE_STATE_INIT 0
#define uvm_rm_locked_call(expr) (expr)

static NV_STATUS rm_status;
static int unset_calls;

static int
uvm_gpu_va_space_state(const uvm_gpu_va_space_t *gpu_va_space)
{
    return gpu_va_space->state;
}

static void
uvm_assert_rwsem_locked_read(int *lock)
{
    assert(lock != NULL);
}

static NV_STATUS
nvUvmInterfaceUnsetPageDirectory(void *handle)
{
    assert(handle != NULL);
    unset_calls++;
    return rm_status;
}

#include "../../../kernel-open/nvidia-uvm/uvm_va_space_unset_checked_impl.h"

int
main(void)
{
    uvm_va_space_t va_space = {0};
    int handle;
    uvm_gpu_va_space_t gpu_va_space = {
        .va_space = &va_space,
        .duped_gpu_va_space = &handle,
        .state = 1,
        .did_set_page_directory = true,
    };

    rm_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_gpu_va_space_unset_page_dir_checked(&gpu_va_space) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(gpu_va_space.did_set_page_directory);
    assert(unset_calls == 1);

    rm_status = NV_OK;
    assert(uvm_gpu_va_space_unset_page_dir_checked(&gpu_va_space) == NV_OK);
    assert(!gpu_va_space.did_set_page_directory);
    assert(unset_calls == 2);

    assert(uvm_gpu_va_space_unset_page_dir_checked(&gpu_va_space) == NV_OK);
    assert(unset_calls == 2);

    gpu_va_space.did_set_page_directory = true;
    rm_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_gpu_va_space_unset_page_dir_ordinary(&gpu_va_space) ==
           NV_ERR_OPERATING_SYSTEM);
#if defined(NV_SUNOS)
    assert(gpu_va_space.did_set_page_directory);
#else
    assert(!gpu_va_space.did_set_page_directory);
#endif
    assert(unset_calls == 3);
    return 0;
}
