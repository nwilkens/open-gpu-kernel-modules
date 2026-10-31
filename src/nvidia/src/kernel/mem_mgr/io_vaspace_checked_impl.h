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

/* Included by io_vaspace.c and by the focused release-failure test. */
NV_STATUS
iovaspaceReleaseMappingChecked(OBJIOVASPACE *pIOVAS,
                                PIOVAMAPPING pIovaMapping)
{
    NV_STATUS status;

    if (pIOVAS == NULL || pIovaMapping == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    /* This scoped path cannot revoke aliases or child mappings. */
    if (pIovaMapping->refcount != 1 ||
        pIovaMapping->link.pChildren != NULL ||
        memdescIsSubMemoryMemDesc(pIovaMapping->pPhysMemDesc))
        return NV_ERR_NOT_SUPPORTED;

    status = osIovaUnmapChecked(pIovaMapping);
    if (status != NV_OK)
        return status;

    memdescRemoveIommuMap(pIovaMapping->pPhysMemDesc, pIovaMapping);
    portMemFree(pIovaMapping);
    --pIOVAS->mappingCount;
    return NV_OK;
}
