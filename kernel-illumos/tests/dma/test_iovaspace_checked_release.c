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

/* A failed checked unbind must retain the RM IOVA mapping record. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef int NV_STATUS;
typedef struct memdesc_s { int is_submemory; } MEMORY_DESCRIPTOR;
typedef struct iova_mapping_s {
    unsigned int refcount;
    struct { struct iova_mapping_s *pChildren; } link;
    MEMORY_DESCRIPTOR *pPhysMemDesc;
    void *pOsData;
} IOVAMAPPING, *PIOVAMAPPING;
typedef struct iovaspace_s { unsigned int mappingCount; } OBJIOVASPACE;

#define NV_OK 0
#define NV_ERR_OPERATING_SYSTEM 2
#define NV_ERR_NOT_SUPPORTED 3
#define NV_ERR_INVALID_ARGUMENT 4

static NV_STATUS os_result;
static int os_count;
static int remove_count;
static int free_count;

static int
memdescIsSubMemoryMemDesc(MEMORY_DESCRIPTOR *desc)
{
    return desc->is_submemory;
}

static NV_STATUS
osIovaUnmapChecked(PIOVAMAPPING mapping)
{
    os_count++;
    if (os_result == NV_OK)
        mapping->pOsData = NULL;
    return os_result;
}

static void
memdescRemoveIommuMap(MEMORY_DESCRIPTOR *desc, PIOVAMAPPING mapping)
{
    assert(desc == mapping->pPhysMemDesc);
    assert(mapping->pOsData == NULL);
    remove_count++;
}

static void
portMemFree(void *p)
{
    free_count++;
    free(p);
}

#include "../../../src/nvidia/src/kernel/mem_mgr/io_vaspace_checked_impl.h"

int
main(void)
{
    MEMORY_DESCRIPTOR desc = {0};
    OBJIOVASPACE space = {.mappingCount = 1};
    PIOVAMAPPING mapping = calloc(1, sizeof (*mapping));
    IOVAMAPPING child = {0};

    assert(mapping != NULL);
    mapping->refcount = 1;
    mapping->pPhysMemDesc = &desc;
    mapping->pOsData = (void *)(uintptr_t)0x1234;

    os_result = NV_ERR_OPERATING_SYSTEM;
    assert(iovaspaceReleaseMappingChecked(&space, mapping) ==
        NV_ERR_OPERATING_SYSTEM);
    assert(mapping->refcount == 1);
    assert(mapping->pOsData == (void *)(uintptr_t)0x1234);
    assert(space.mappingCount == 1);
    assert(os_count == 1 && remove_count == 0 && free_count == 0);

    mapping->refcount = 2;
    assert(iovaspaceReleaseMappingChecked(&space, mapping) ==
        NV_ERR_NOT_SUPPORTED);
    assert(mapping->refcount == 2 && os_count == 1);
    mapping->refcount = 1;

    mapping->link.pChildren = &child;
    assert(iovaspaceReleaseMappingChecked(&space, mapping) ==
        NV_ERR_NOT_SUPPORTED);
    assert(mapping->link.pChildren == &child && os_count == 1);
    mapping->link.pChildren = NULL;

    os_result = NV_OK;
    assert(iovaspaceReleaseMappingChecked(&space, mapping) == NV_OK);
    assert(space.mappingCount == 0);
    assert(os_count == 2 && remove_count == 1 && free_count == 1);
    return 0;
}
