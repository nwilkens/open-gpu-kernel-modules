/*
 * SPDX-FileCopyrightText: Copyright (c) 2000-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * ACPI display methods: _DDC, _ROM, _DOD and the MXM mux.
 */

#include "nv-illumos.h"

#include <sys/acpi/acpi.h>
#include <sys/acpica.h>

#include "nvidia_acpi.h"

/* Find the child device whose _ADR matches one of the given ids. */
static ACPI_HANDLE
nv_acpi_find_child_by_adr(ACPI_HANDLE parent, const UINT64 *ids, int nids,
    UINT64 mask)
{
    ACPI_HANDLE child = NULL;
    UINT64 adr;
    int i;

    while (AcpiGetNextObject(ACPI_TYPE_DEVICE, parent, child, &child) == AE_OK &&
           child != NULL)
    {
        if (ACPI_FAILURE(nv_acpi_evaluate_integer(child, "_ADR", &adr)))
            continue;

        for (i = 0; i < nids; i++)
        {
            if ((adr & mask) == ids[i])
                return child;
        }
    }

    return NULL;
}

NV_STATUS NV_API_CALL nv_acpi_ddc_method(
    nv_state_t *nv,
    void *pEdidBuffer,
    NvU32 *pSize,
    NvBool bReadMultiBlock
)
{
    static const UINT64 lcd_ids[] = { 0x0110, 0x0118, 0x0400, 0xA420 };
    ACPI_HANDLE dev_handle = nv_acpi_dev_handle(NV_GET_NVIS(nv)->dip);
    ACPI_HANDLE lcd;
    ACPI_OBJECT *ddc = NULL;
    ACPI_STATUS status = AE_ERROR;
    NV_STATUS rc = NV_OK;
    NvU32 i;

    if (dev_handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (!nv_may_sleep())
        return NV_ERR_NOT_SUPPORTED;

    lcd = nv_acpi_find_child_by_adr(dev_handle, lcd_ids,
        NV_ARRAY_ELEMENTS(lcd_ids), 0xffff);
    if (lcd == NULL)
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s LCD not found\n", __func__);
        return NV_ERR_GENERIC;
    }

    /* ACPI: _DDC(1) returns 128 bytes of EDID, _DDC(2) returns 256. */
    for (i = bReadMultiBlock ? 2 : 1; i >= 1; i--)
    {
        ACPI_BUFFER output = { ACPI_ALLOCATE_BUFFER, NULL };
        ACPI_OBJECT arg;
        ACPI_OBJECT_LIST input = { 1, &arg };

        arg.Integer.Type = ACPI_TYPE_INTEGER;
        arg.Integer.Value = i;
        status = AcpiEvaluateObject(lcd, "_DDC", &input, &output);
        if (ACPI_SUCCESS(status))
        {
            ddc = output.Pointer;
            break;
        }
    }

    if (ACPI_FAILURE(status))
        return NV_ERR_GENERIC;

    if (ddc != NULL && ddc->Type == ACPI_TYPE_BUFFER && ddc->Buffer.Length > 0)
    {
        if (ddc->Buffer.Length <= *pSize)
        {
            *pSize = ddc->Buffer.Length;
            bcopy(ddc->Buffer.Pointer, pEdidBuffer, ddc->Buffer.Length);
        }
        else
        {
            rc = NV_ERR_BUFFER_TOO_SMALL;
        }
    }

    AcpiOsFree(ddc);
    return rc;
}

NV_STATUS NV_API_CALL nv_acpi_rom_method(
    nv_state_t *nv,
    NvU32 *pInData,
    NvU32 *pOutData
)
{
    ACPI_HANDLE dev_handle = nv_acpi_dev_handle(NV_GET_NVIS(nv)->dip);
    ACPI_BUFFER output = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_OBJECT args[2];
    ACPI_OBJECT_LIST input = { 2, args };
    ACPI_OBJECT *rom;
    NvU32 offset, length;
    NV_STATUS rc = NV_OK;

    if (dev_handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (!nv_may_sleep())
        return NV_ERR_NOT_SUPPORTED;

    offset = pInData[0];
    length = pInData[1];

    args[0].Type = ACPI_TYPE_INTEGER;
    args[0].Integer.Value = offset;
    args[1].Type = ACPI_TYPE_INTEGER;
    args[1].Integer.Value = length;

    if (ACPI_FAILURE(AcpiEvaluateObject(dev_handle, "_ROM", &input, &output)))
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s: failed to evaluate _ROM method!\n",
                  __func__);
        return NV_ERR_GENERIC;
    }

    rom = output.Pointer;
    if (rom != NULL && rom->Type == ACPI_TYPE_BUFFER && rom->Buffer.Length >= length)
    {
        bcopy(rom->Buffer.Pointer, pOutData, length);
    }
    else
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s: Invalid _ROM data\n", __func__);
        rc = NV_ERR_GENERIC;
    }

    AcpiOsFree(output.Pointer);
    return rc;
}

NV_STATUS NV_API_CALL nv_acpi_dod_method(
    nv_state_t *nv,
    NvU32      *pOutData,
    NvU32      *pSize
)
{
    ACPI_HANDLE dev_handle = nv_acpi_dev_handle(NV_GET_NVIS(nv)->dip);
    ACPI_BUFFER output = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_OBJECT *dod;
    NvU32 i, count = *pSize / sizeof (NvU32);
    NV_STATUS rc = NV_OK;

    if (dev_handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (!nv_may_sleep())
        return NV_ERR_NOT_SUPPORTED;

    if (ACPI_FAILURE(AcpiEvaluateObject(dev_handle, "_DOD", NULL, &output)))
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s: failed to evaluate _DOD method!\n",
                  __func__);
        return NV_ERR_GENERIC;
    }

    dod = output.Pointer;
    *pSize = 0;

    if (dod != NULL && dod->Type == ACPI_TYPE_PACKAGE &&
        dod->Package.Count <= count)
    {
        for (i = 0; i < dod->Package.Count; i++)
        {
            if (dod->Package.Elements[i].Type != ACPI_TYPE_INTEGER)
            {
                rc = NV_ERR_GENERIC;
                break;
            }

            pOutData[i] = (NvU32)dod->Package.Elements[i].Integer.Value;
            *pSize += sizeof (NvU32);
        }
    }
    else
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s: _DOD data too large!\n", __func__);
        rc = NV_ERR_GENERIC;
    }

    AcpiOsFree(output.Pointer);
    return rc;
}

NV_STATUS NV_API_CALL nv_acpi_mux_method(
    nv_state_t *nv,
    NvU32 *pInOut,
    NvU32 muxAcpiId,
    const char *pMethodName
)
{
    ACPI_HANDLE dev_handle = nv_acpi_dev_handle(NV_GET_NVIS(nv)->dip);
    ACPI_BUFFER output = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_OBJECT arg;
    ACPI_OBJECT_LIST input = { 1, &arg };
    ACPI_OBJECT *mux;
    ACPI_HANDLE mux_dev;
    UINT64 id = muxAcpiId;
    char method[5];
    NV_STATUS rc = NV_OK;

    if (strcmp(pMethodName, "MXDS") != 0 && strcmp(pMethodName, "MXDM") != 0)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s: Unsupported ACPI method %s\n",
                  __func__, pMethodName);
        return NV_ERR_NOT_SUPPORTED;
    }

    if (dev_handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (!nv_may_sleep())
        return NV_ERR_NOT_SUPPORTED;

    mux_dev = nv_acpi_find_child_by_adr(dev_handle, &id, 1, ~0ULL);
    if (mux_dev == NULL)
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s Mux device handle not found\n", __func__);
        return NV_ERR_GENERIC;
    }

    (void) strlcpy(method, pMethodName, sizeof (method));
    arg.Integer.Type = ACPI_TYPE_INTEGER;
    arg.Integer.Value = (UINT64)*pInOut;

    if (ACPI_FAILURE(AcpiEvaluateObject(mux_dev, method, &input, &output)))
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s: Failed to evaluate %s method!\n",
                  __func__, pMethodName);
        return NV_ERR_GENERIC;
    }

    mux = output.Pointer;
    if (mux != NULL && mux->Type == ACPI_TYPE_INTEGER)
    {
        *pInOut = (NvU32)mux->Integer.Value;
    }
    else
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s: Invalid MUX data\n", __func__);
        rc = NV_ERR_GENERIC;
    }

    AcpiOsFree(output.Pointer);
    return rc;
}
