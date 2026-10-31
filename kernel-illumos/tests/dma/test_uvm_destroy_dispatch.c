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

/* Tracked OS imports use checked destruction; ordinary maps keep legacy path. */
#include <assert.h>
#include <stddef.h>

typedef int NV_STATUS;
#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 1

struct list_head { int unused; };
typedef struct { void *import_token; } uvm_ext_gpu_mem_handle;
typedef struct { uvm_ext_gpu_mem_handle *mem_handle; int present; } uvm_ext_gpu_map_t;
typedef struct { int unused; } uvm_va_range_external_t;

static NV_STATUS checked_status;
static int checked_calls;
static int legacy_calls;

static NV_STATUS
uvm_ext_gpu_map_destroy_tracked(uvm_va_range_external_t *range,
                                uvm_ext_gpu_map_t *map)
{
    (void)range;
    ++checked_calls;
    if (checked_status == NV_OK)
        map->present = 0;
    return checked_status;
}

static void
uvm_ext_gpu_map_destroy_legacy(uvm_va_range_external_t *range,
                               uvm_ext_gpu_map_t *map,
                               struct list_head *deferred)
{
    (void)range;
    (void)deferred;
    ++legacy_calls;
    if (map != NULL)
        map->present = 0;
}

#include "../../../kernel-open/nvidia-uvm/uvm_map_external_destroy_dispatch_impl.h"

int main(void)
{
    uvm_va_range_external_t range = {0};
    uvm_ext_gpu_mem_handle tracked_handle = {(void *)1};
    uvm_ext_gpu_mem_handle ordinary_handle = {NULL};
    uvm_ext_gpu_map_t tracked = {&tracked_handle, 1};
    uvm_ext_gpu_map_t ordinary = {&ordinary_handle, 1};

    checked_status = NV_ERR_OPERATING_SYSTEM;
    assert(uvm_ext_gpu_map_destroy(&range, &tracked, NULL) ==
           NV_ERR_OPERATING_SYSTEM);
    assert(tracked.present && checked_calls == 1 && legacy_calls == 0);

    checked_status = NV_OK;
    assert(uvm_ext_gpu_map_destroy(&range, &tracked, NULL) == NV_OK);
    assert(!tracked.present && checked_calls == 2 && legacy_calls == 0);

    assert(uvm_ext_gpu_map_destroy(&range, &ordinary, NULL) == NV_OK);
    assert(!ordinary.present && checked_calls == 2 && legacy_calls == 1);
    assert(uvm_ext_gpu_map_destroy(&range, NULL, NULL) == NV_OK);
    assert(legacy_calls == 2);
    return 0;
}
