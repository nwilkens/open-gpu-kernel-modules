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

/* Included by uvm_map_external.c and the focused map-ownership test. */
NV_STATUS
uvm_ext_gpu_map_destroy_checked(uvm_va_range_external_t *external_range,
                                uvm_ext_gpu_map_t *ext_gpu_map)
{
    uvm_gpu_t *mapped_gpu;
    uvm_ext_gpu_range_tree_t *range_tree;
    uvm_ext_gpu_mem_handle *mem_handle;
    NV_STATUS status;

    if (external_range == NULL || ext_gpu_map == NULL ||
        ext_gpu_map->base.gpu == NULL || ext_gpu_map->mem_handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    mapped_gpu = ext_gpu_map->base.gpu;
    range_tree = uvm_ext_gpu_range_tree(external_range, mapped_gpu);
    mem_handle = ext_gpu_map->mem_handle;
    uvm_assert_mutex_locked(&range_tree->lock);

    /* The caller keeps the VA space, GPU and RM descriptor guard alive. */
    status = uvm_ext_gpu_map_revoke_ptes_checked(external_range, ext_gpu_map);
    if (status != NV_OK)
        return status;

    if (nv_kref_read(&mem_handle->ref_count) == 0)
        return NV_ERR_INVALID_STATE;

    if (nv_kref_read(&mem_handle->ref_count) == 1) {
        status = uvm_release_rm_handle_checked(mem_handle);
        if (status != NV_OK)
            return status;
    }
    else {
        /* A split fragment still owns the shared duplicate in another node. */
        nv_kref_put(&mem_handle->ref_count, uvm_release_rm_handle);
    }

    ext_gpu_map->mem_handle = NULL;
    uvm_range_tree_remove(&range_tree->tree, &ext_gpu_map->base.node);
    uvm_ext_gpu_map_free_internal(ext_gpu_map);

    if (uvm_range_tree_empty(&range_tree->tree))
        uvm_processor_mask_clear_atomic(&external_range->mapped_gpus,
                                        mapped_gpu->id);

    return NV_OK;
}
