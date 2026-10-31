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

/*
 * Compile the real illumos DMA-unmap implementation with only its DDI calls
 * replaced. A failed unbind must preserve the mapping and its backing pages.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint64_t NvU64;
typedef int NvBool;
typedef int NV_STATUS;
typedef void *ddi_dma_handle_t;
typedef void nv_dma_device_t;

#define NV_API_CALL
#define NV_OK 0
#define NV_ERR_NOT_SUPPORTED 1
#define NV_ERR_OPERATING_SYSTEM 2
#define DDI_SUCCESS 0

typedef struct nv_dma_map_s {
    ddi_dma_handle_t handle;
    char *kva;
    NvU64 page_count;
    NvBool own_kva;
} nv_dma_map_t;

static int unbind_result;
static int unbind_count;
static int free_handle_count;
static int unmap_kva_count;
static int free_map_count;

static int
ddi_dma_unbind_handle(ddi_dma_handle_t handle)
{
    assert(handle == (void *)(uintptr_t)0x1234);
    unbind_count++;
    return unbind_result;
}

static void
ddi_dma_free_handle(ddi_dma_handle_t *handle)
{
    assert(*handle == (void *)(uintptr_t)0x1234);
    free_handle_count++;
    *handle = NULL;
}

static void
nv_kunmap_phys(char *kva, NvU64 pages)
{
    assert(kva == (char *)(uintptr_t)0x5678);
    assert(pages == 2);
    unmap_kva_count++;
}

static void
kmem_free(void *p, size_t size)
{
    assert(size == sizeof (nv_dma_map_t));
    free_map_count++;
    free(p);
}

#include "../../nvidia/nvidia_dma_unmap_impl.h"

int
main(void)
{
    nv_dma_map_t *map = calloc(1, sizeof (*map));
    void *priv;

    assert(map != NULL);
    map->handle = (void *)(uintptr_t)0x1234;
    map->kva = (char *)(uintptr_t)0x5678;
    map->page_count = 2;
    map->own_kva = 1;
    priv = map;

    unbind_result = -1;
    assert(nv_dma_unmap_alloc(NULL, 2, NULL, &priv) ==
        NV_ERR_OPERATING_SYSTEM);
    assert(priv == map);
    assert(map->handle == (void *)(uintptr_t)0x1234);
    assert(unbind_count == 1);
    assert(free_handle_count == 0);
    assert(unmap_kva_count == 0);
    assert(free_map_count == 0);

    unbind_result = DDI_SUCCESS;
    assert(nv_dma_unmap_alloc(NULL, 2, NULL, &priv) == NV_OK);
    assert(priv == NULL);
    assert(unbind_count == 2);
    assert(free_handle_count == 1);
    assert(unmap_kva_count == 1);
    assert(free_map_count == 1);
    return 0;
}
