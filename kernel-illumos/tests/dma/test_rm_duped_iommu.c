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

/* Checked UVM copies must not be freed after failed IOMMU unmap. */
#include <assert.h>
#include <stddef.h>

typedef unsigned int NvU32;
typedef int NvBool;
typedef int NV_STATUS;
typedef struct memory_descriptor_s {
    int needs_iommu;
} MEMORY_DESCRIPTOR, *PMEMORY_DESCRIPTOR;
typedef struct memory_s {
    PMEMORY_DESCRIPTOR pMemDesc;
    int is_os_descriptor;
} Memory;
typedef struct os_desc_memory_s { int unused; } OsDescMemory;

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 2
#define NV_SUNOS 1
#define dynamicCast(memory, type) ((memory)->is_os_descriptor ? (type *)1 : NULL)

static NV_STATUS checked_result;
static int checked_count;
static int legacy_count;

static int
memdescRequiresIommuMapping(PMEMORY_DESCRIPTOR desc)
{
    return desc->needs_iommu;
}

static NV_STATUS
memdescUnmapIommuDupChecked(PMEMORY_DESCRIPTOR desc, NvU32 id)
{
    assert(desc != NULL && id == 7);
    checked_count++;
    return checked_result;
}

static void
memdescUnmapIommu(PMEMORY_DESCRIPTOR desc, NvU32 id)
{
    assert(desc != NULL && id == 7);
    legacy_count++;
}

#include "../../../src/nvidia/src/kernel/rmapi/nv_gpu_ops_iommu_impl.h"

int
main(void)
{
    MEMORY_DESCRIPTOR desc = {.needs_iommu = 1};
    Memory os_desc = {.pMemDesc = &desc, .is_os_descriptor = 1};
    Memory ordinary = {.pMemDesc = &desc, .is_os_descriptor = 0};

    checked_result = NV_ERR_OPERATING_SYSTEM;
    assert(nvGpuOpsUnmapDupIommu(&os_desc, 7, 1) == NV_ERR_OPERATING_SYSTEM);
    assert(checked_count == 1 && legacy_count == 0);

    checked_result = NV_OK;
    assert(nvGpuOpsUnmapDupIommu(&os_desc, 7, 1) == NV_OK);
    assert(checked_count == 2 && legacy_count == 0);

    checked_result = NV_ERR_OPERATING_SYSTEM;
    assert(nvGpuOpsUnmapDupIommu(&ordinary, 7, 1) == NV_ERR_OPERATING_SYSTEM);
    assert(checked_count == 3 && legacy_count == 0);

    checked_result = NV_OK;
    assert(nvGpuOpsUnmapDupIommu(&ordinary, 7, 1) == NV_OK);
    assert(checked_count == 4 && legacy_count == 0);

    assert(nvGpuOpsUnmapDupIommu(&os_desc, 7, 0) == NV_OK);
    assert(checked_count == 4 && legacy_count == 1);

    desc.needs_iommu = 0;
    assert(nvGpuOpsUnmapDupIommu(&os_desc, 7, 1) == NV_OK);
    assert(checked_count == 4 && legacy_count == 1);
    return 0;
}
