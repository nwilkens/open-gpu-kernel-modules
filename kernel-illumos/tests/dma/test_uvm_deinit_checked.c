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

/* Failed page-tree drain retains the ranges without queueing put twice. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef int NV_STATUS;
typedef struct { unsigned int entry_count; } uvm_page_table_range_t;
typedef struct { int unused; } uvm_page_tree_t;
typedef struct {
    uvm_page_tree_t *tree;
    uvm_page_table_range_t *ranges;
    size_t range_count;
} uvm_page_table_range_vec_t;

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1
#define NV_ERR_INVALID_ARGUMENT 2

static int put_count;
static int wait_count;
static int free_count;
static NV_STATUS wait_status;

static void
uvm_page_tree_put_ptes_async(uvm_page_tree_t *tree,
                             uvm_page_table_range_t *range)
{
    (void)tree;
    assert(range->entry_count != 0);
    put_count++;
}

static NV_STATUS
uvm_page_tree_wait(uvm_page_tree_t *tree)
{
    (void)tree;
    wait_count++;
    return wait_status;
}

static void
uvm_kvfree(void *p)
{
    free_count++;
    free(p);
}

#include "../../../kernel-open/nvidia-uvm/uvm_mmu_deinit_checked_impl.h"

int
main(void)
{
    uvm_page_tree_t tree = {0};
    uvm_page_table_range_vec_t vec = {
        .tree = &tree,
        .ranges = calloc(3, sizeof (*vec.ranges)),
        .range_count = 3,
    };
    bool put_queued = false;

    assert(vec.ranges != NULL);
    vec.ranges[0].entry_count = 4;
    vec.ranges[1].entry_count = 2;
    wait_status = NV_ERR_OPERATING_SYSTEM;

    assert(uvm_page_table_range_vec_deinit_checked(&vec, &put_queued) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(put_queued);
    assert(vec.ranges != NULL);
    assert(put_count == 2 && wait_count == 1 && free_count == 0);

    wait_status = NV_OK;
    assert(uvm_page_table_range_vec_deinit_checked(&vec, &put_queued) == NV_OK);
    assert(!put_queued);
    assert(vec.ranges == NULL && vec.range_count == 0 && vec.tree == NULL);
    assert(put_count == 2 && wait_count == 2 && free_count == 1);

    assert(uvm_page_table_range_vec_deinit_checked(&vec, &put_queued) == NV_OK);
    assert(wait_count == 2 && free_count == 1);
    return 0;
}
