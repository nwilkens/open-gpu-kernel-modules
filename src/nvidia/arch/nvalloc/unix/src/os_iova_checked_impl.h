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

/* Included by os.c and by the focused unbind-failure test. */
NV_STATUS
osIovaUnmapAllocationChecked(PIOVAMAPPING pIovaMapping,
                             nv_dma_device_t *dmaDevice,
                             NvU64 osPageCount)
{
    void *pPriv;
    NV_STATUS status;

    if (pIovaMapping == NULL || pIovaMapping->pOsData == NULL)
        return NV_ERR_NOT_SUPPORTED;

    pPriv = (void *)pIovaMapping->pOsData;
    status = nv_dma_unmap_alloc(dmaDevice, osPageCount,
                                &pIovaMapping->iovaArray[0], &pPriv);
    if (status != NV_OK)
        return status;

    pIovaMapping->pOsData = pPriv;
    return NV_OK;
}
