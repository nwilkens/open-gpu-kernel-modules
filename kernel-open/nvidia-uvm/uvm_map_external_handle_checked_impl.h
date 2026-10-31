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

/* Included by uvm_map_external.c and the focused failed-RM-free test. */
static NV_STATUS
uvm_release_rm_handle_checked(uvm_ext_gpu_mem_handle *mem_handle)
{
    NV_STATUS status;

    if (mem_handle == NULL || mem_handle->gpu == NULL ||
        mem_handle->rm_handle == 0 || mem_handle->checked_release_failed ||
        nv_kref_read(&mem_handle->ref_count) != 1)
        return NV_ERR_INVALID_STATE;

    status = uvm_rm_locked_call(nvUvmInterfaceFreeDupedHandleChecked(
        uvm_gpu_device_handle(mem_handle->gpu), mem_handle->rm_handle,
        &mem_handle->checked_iova_released));
    if (status != NV_OK) {
        /* RM may have partly freed the handle; retry is not provably safe. */
        mem_handle->checked_release_failed = true;
        return status;
    }

    mem_handle->rm_handle = 0;
    nv_kref_put(&mem_handle->ref_count, uvm_release_rm_handle);
    return NV_OK;
}
