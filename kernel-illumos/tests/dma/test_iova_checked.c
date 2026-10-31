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

/* Checked IOVA unmap must preserve the RM mapping on an OS unbind error. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t NvU64;
typedef int NV_STATUS;
typedef void nv_dma_device_t;
typedef struct iova_mapping_s {
    void *pOsData;
    NvU64 iovaArray[2];
} IOVAMAPPING, *PIOVAMAPPING;

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 2
#define NV_ERR_NOT_SUPPORTED 3

static NV_STATUS unmap_result;
static int unmap_count;

static NV_STATUS
nv_dma_unmap_alloc(nv_dma_device_t *dev, NvU64 pages, NvU64 *array,
    void **priv)
{
    assert(dev == (void *)(uintptr_t)0xabcd);
    assert(pages == 2);
    assert(array[0] == 0x1000);
    assert(*priv == (void *)(uintptr_t)0x1234);
    unmap_count++;
    if (unmap_result != NV_OK)
        return unmap_result;
    *priv = NULL;
    return NV_OK;
}

#include "../../../src/nvidia/arch/nvalloc/unix/src/os_iova_checked_impl.h"

int
main(void)
{
    IOVAMAPPING mapping = {
        .pOsData = (void *)(uintptr_t)0x1234,
        .iovaArray = { 0x1000, 0x2000 }
    };

    unmap_result = NV_ERR_OPERATING_SYSTEM;
    assert(osIovaUnmapAllocationChecked(&mapping,
        (void *)(uintptr_t)0xabcd, 2) == NV_ERR_OPERATING_SYSTEM);
    assert(mapping.pOsData == (void *)(uintptr_t)0x1234);
    assert(mapping.iovaArray[0] == 0x1000);
    assert(unmap_count == 1);

    unmap_result = NV_OK;
    assert(osIovaUnmapAllocationChecked(&mapping,
        (void *)(uintptr_t)0xabcd, 2) == NV_OK);
    assert(mapping.pOsData == NULL);
    assert(unmap_count == 2);

    assert(osIovaUnmapAllocationChecked(&mapping,
        (void *)(uintptr_t)0xabcd, 2) == NV_ERR_NOT_SUPPORTED);
    assert(unmap_count == 2);
    return 0;
}
