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
 *
 * Kept separate so the DMA unmap failure path can be exercised with DDI
 * stubs. The including file provides nv_dma_map_t and the DDI/NVIDIA types.
 */
NV_STATUS NV_API_CALL nv_dma_unmap_alloc(
    nv_dma_device_t *dma_dev,
    NvU64            page_count,
    NvU64           *va_array,
    void           **priv
)
{
    nv_dma_map_t *map;

    (void)dma_dev;
    (void)page_count;
    (void)va_array;

    if (priv == NULL || *priv == NULL)
        return NV_ERR_NOT_SUPPORTED;

    map = *priv;

    if (map->handle != NULL)
    {
        if (ddi_dma_unbind_handle(map->handle) != DDI_SUCCESS)
            return NV_ERR_OPERATING_SYSTEM;
        ddi_dma_free_handle(&map->handle);
    }

    if (map->own_kva)
        nv_kunmap_phys(map->kva, map->page_count);

    kmem_free(map, sizeof (*map));
    *priv = NULL;
    return NV_OK;
}
