/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * GSP firmware images live in /kernel/firmware/nvidia/<version>/, the
 * illumos counterpart of /lib/firmware/nvidia/<version>/ on Linux.
 */
#define NV_FIRMWARE_FOR_NAME(name)  NV_VERSION_STRING "/" name ".bin"

#include "nv-illumos.h"
#include "nv-firmware.h"

#include <sys/firmload.h>

typedef struct nv_firmware_s {
    void   *data;
    size_t  size;
} nv_firmware_t;

const void* NV_API_CALL nv_get_firmware(
    nv_state_t *nv,
    nv_firmware_type_t fw_type,
    nv_firmware_chip_family_t fw_chip_family,
    const void **fw_buf,
    NvU32 *fw_size
)
{
    const char *name = nv_firmware_for_chip_family(fw_type, fw_chip_family);
    firmware_handle_t fh;
    nv_firmware_t *fw;
    off_t size;
    int err;

    if (name[0] == '\0')
        return NULL;

    err = firmware_open(NV_ILLUMOS_DRIVER_NAME, name, &fh);
    if (err != 0)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "failed to open firmware %s/%s (error %d)\n",
            NV_ILLUMOS_DRIVER_NAME, name, err);
        return NULL;
    }

    size = firmware_get_size(fh);
    if (size <= 0 || size > NV_U32_MAX)
    {
        (void) firmware_close(fh);
        return NULL;
    }

    fw = kmem_zalloc(sizeof (*fw), KM_SLEEP);
    fw->size = (size_t)size;
    fw->data = kmem_alloc(fw->size, KM_NOSLEEP | KM_NORMALPRI);
    if (fw->data == NULL)
    {
        (void) firmware_close(fh);
        kmem_free(fw, sizeof (*fw));
        return NULL;
    }

    err = firmware_read(fh, 0, fw->data, fw->size);
    (void) firmware_close(fh);
    if (err != 0)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "failed to read firmware %s/%s (error %d)\n",
            NV_ILLUMOS_DRIVER_NAME, name, err);
        kmem_free(fw->data, fw->size);
        kmem_free(fw, sizeof (*fw));
        return NULL;
    }

    *fw_buf = fw->data;
    *fw_size = (NvU32)fw->size;

    return fw;
}

void NV_API_CALL nv_put_firmware(const void *fw_handle)
{
    nv_firmware_t *fw = (nv_firmware_t *)fw_handle;

    if (fw == NULL)
        return;

    kmem_free(fw->data, fw->size);
    kmem_free(fw, sizeof (*fw));
}
