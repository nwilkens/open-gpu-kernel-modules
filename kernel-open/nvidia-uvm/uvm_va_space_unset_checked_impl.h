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

/* Included by uvm_va_space.c and the focused RM failure test. */
NV_STATUS
uvm_gpu_va_space_unset_page_dir_checked(uvm_gpu_va_space_t *gpu_va_space)
{
    NV_STATUS status;

    if (uvm_gpu_va_space_state(gpu_va_space) != UVM_GPU_VA_SPACE_STATE_INIT)
        uvm_assert_rwsem_locked_read(&gpu_va_space->va_space->lock);

    if (!gpu_va_space->did_set_page_directory)
        return NV_OK;

    status = uvm_rm_locked_call(nvUvmInterfaceUnsetPageDirectory(
        gpu_va_space->duped_gpu_va_space));
    if (status == NV_OK)
        gpu_va_space->did_set_page_directory = false;

    return status;
}

/* Preserve Linux's ordinary teardown bookkeeping after an RM error. */
static NV_STATUS
uvm_gpu_va_space_unset_page_dir_ordinary(uvm_gpu_va_space_t *gpu_va_space)
{
    NV_STATUS status = uvm_gpu_va_space_unset_page_dir_checked(gpu_va_space);

#if !defined(NV_SUNOS)
    gpu_va_space->did_set_page_directory = false;
#endif
    return status;
}
