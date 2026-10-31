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

/* VA writer teardown must detect a tracked map before freeing its parent. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { void *import_token; } uvm_ext_gpu_mem_handle;
typedef struct uvm_ext_gpu_map {
    uvm_ext_gpu_mem_handle *mem_handle;
    struct uvm_ext_gpu_map *next;
} uvm_ext_gpu_map_t;
typedef struct { int lock; uvm_ext_gpu_map_t *head; } uvm_ext_gpu_range_tree_t;
typedef struct { int id; } uvm_gpu_t;
typedef struct { uvm_ext_gpu_range_tree_t tree; } uvm_va_range_external_t;

static uvm_ext_gpu_range_tree_t *
uvm_ext_gpu_range_tree(uvm_va_range_external_t *range, uvm_gpu_t *gpu)
{
    (void)gpu;
    return &range->tree;
}
static void uvm_assert_mutex_locked(int *lock) { assert(*lock); }
#define uvm_ext_gpu_map_for_each(map, range, gpu) \
    for ((map) = (range)->tree.head; (map) != NULL; (map) = (map)->next)

#include "../../../kernel-open/nvidia-uvm/uvm_map_external_has_tracked_impl.h"

int main(void)
{
    uvm_gpu_t gpu = {0};
    uvm_ext_gpu_mem_handle ordinary = {NULL};
    uvm_ext_gpu_mem_handle tracked = {(void *)1};
    uvm_ext_gpu_map_t second = {&tracked, NULL};
    uvm_ext_gpu_map_t first = {&ordinary, &second};
    uvm_va_range_external_t range = {{1, &first}};

    assert(uvm_ext_gpu_range_has_tracked_import(&range, &gpu));
    second.mem_handle = NULL;
    assert(!uvm_ext_gpu_range_has_tracked_import(&range, &gpu));
    first.mem_handle = &tracked;
    assert(uvm_ext_gpu_range_has_tracked_import(&range, &gpu));
    return 0;
}
