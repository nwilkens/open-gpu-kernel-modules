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

/* Included by uvm_map_external.c and the focused barrier-failure test. */
static NV_STATUS
uvm_ext_gpu_map_revoke_ptes_checked(uvm_va_range_external_t *external_range,
                                     uvm_ext_gpu_map_t *ext_gpu_map)
{
    NV_STATUS status;

    if (external_range == NULL || ext_gpu_map == NULL ||
        ext_gpu_map->mem_handle == NULL || ext_gpu_map->base.gpu == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (ext_gpu_map->pt_revoke_complete)
        return NV_OK;

    /* A failed page-tree wait may be retried, but PTE puts must not be. */
    if (!ext_gpu_map->pt_release_queued) {
        status = uvm_tracker_wait(&ext_gpu_map->tracker);
        if (status != NV_OK)
            return status;

        if (ext_gpu_map->need_l2_invalidate_at_unmap) {
            uvm_aperture_t aperture = ext_gpu_map->is_sysmem ?
                UVM_APERTURE_SYS : UVM_APERTURE_PEER(0);

            status = uvm_mmu_l2_invalidate(ext_gpu_map->base.gpu, aperture);
            if (status != NV_OK)
                return status;
        }

        if (ext_gpu_map->base.pt_range_vec.ranges) {
            uvm_membar_t membar = va_range_downgrade_membar(
                &external_range->va_range, ext_gpu_map);

            status = uvm_page_table_range_vec_clear_ptes(
                &ext_gpu_map->base.pt_range_vec, membar);
            if (status != NV_OK)
                return status;
        }
    }

    status = uvm_page_table_range_vec_deinit_checked(
        &ext_gpu_map->base.pt_range_vec,
        &ext_gpu_map->pt_release_queued);
    if (status != NV_OK)
        return status;

    uvm_tracker_deinit(&ext_gpu_map->tracker);
    ext_gpu_map->pt_revoke_complete = true;
    return NV_OK;
}
