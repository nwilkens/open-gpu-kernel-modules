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

/* Included by uvm_map_external.c and the focused import-revocation test. */
NV_STATUS
uvm_ext_gpu_import_revoke_checked(uvm_va_space_t *va_space,
                                  uvm_ext_gpu_mem_handle *target)
{
    uvm_va_range_t *va_range;
    uvm_gpu_t *gpu;
    NV_STATUS status;

    if (va_space == NULL || target == NULL)
        return NV_ERR_INVALID_STATE;

    /*
     * An ordinary host unmap has the same lifetime contract as a targeted
     * UVM_UNMAP_EXTERNAL: retire this import's PTEs and RM duplicate while
     * leaving unrelated user channels running. The checked PTE path waits for
     * the TLB membar before the pin can be released.
     *
     * VA-space shutdown still needs the full channel and fault-worker drain.
     * Keep the VA read lock across the live scan so shutdown cannot begin
     * between this decision and the checked destroy.
     */
    uvm_va_space_down_read_rm(va_space);
    if (va_space->disallow_new_registers ||
        atomic_read(&va_space->tracked_imports_blocked)) {
        status = uvm_va_space_revoke_all_user_channels_checked(va_space);
        uvm_va_space_up_read_rm(va_space);
        if (status != NV_OK)
            return status;

        /* Fault workers need the VA writer for lookup removal, then no VA
         * lock while queued work drains. Revocation is sticky across this gap. */
        status = uvm_va_space_quiesce_user_faults_checked(va_space);
        if (status != NV_OK)
            return status;

        uvm_va_space_down_read_rm(va_space);
    }

    uvm_for_each_va_range(va_range, va_space) {
        uvm_va_range_external_t *external_range;

        if (va_range->type != UVM_VA_RANGE_TYPE_EXTERNAL)
            continue;
        external_range = uvm_va_range_to_external(va_range);

        for_each_va_space_gpu(gpu, va_space) {
            uvm_ext_gpu_range_tree_t *tree = uvm_ext_gpu_range_tree(external_range, gpu);
            uvm_ext_gpu_map_t *map, *next;

            uvm_mutex_lock(&tree->lock);
            map = uvm_ext_gpu_map_iter_first(external_range, gpu,
                                             va_range->node.start,
                                             va_range->node.end);
            while (map != NULL) {
                next = uvm_ext_gpu_map_iter_next(external_range, map,
                                                 va_range->node.end);
                if (map->mem_handle == target) {
                    /* A detached deferred fragment must keep the pin held. */
                    bool last = nv_kref_read(&target->ref_count) == 1;

                    status = uvm_ext_gpu_map_destroy_checked(external_range, map);
                    if (status != NV_OK) {
                        uvm_mutex_unlock(&tree->lock);
                        goto out;
                    }
                    if (last) {
                        /* The shared handle is now freed; do not inspect it. */
                        uvm_mutex_unlock(&tree->lock);
                        status = NV_OK;
                        goto out;
                    }
                }
                map = next;
            }
            uvm_mutex_unlock(&tree->lock);
        }
    }

    status = NV_ERR_INVALID_STATE;
out:
    uvm_va_space_up_read_rm(va_space);
    uvm_va_space_free_deferred_external_ranges(va_space);
    return status;
}
