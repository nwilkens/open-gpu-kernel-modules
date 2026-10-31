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

/* Included by uvm_mmu.c and the focused failed-drain test. */
NV_STATUS uvm_page_table_range_vec_deinit_checked(
    uvm_page_table_range_vec_t *range_vec, bool *put_queued)
{
    size_t i;
    NV_STATUS status;

    if (range_vec == NULL)
        return NV_OK;
    if (put_queued == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (range_vec->ranges != NULL) {
        if (!*put_queued) {
            for (i = 0; i < range_vec->range_count; ++i) {
                uvm_page_table_range_t *range = &range_vec->ranges[i];
                if (!range->entry_count)
                    break;
                uvm_page_tree_put_ptes_async(range_vec->tree, range);
            }
            *put_queued = true;
        }

        status = uvm_page_tree_wait(range_vec->tree);
        if (status != NV_OK)
            return status;

        uvm_kvfree(range_vec->ranges);
    }

    memset(range_vec, 0, sizeof (*range_vec));
    *put_queued = false;
    return NV_OK;
}
