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
 * ACPI methods (_DSM, _DDC, _ROM, _DOD, NVIF, WMMX, MXDS/MXDM) and
 * notifications, through the illumos ACPICA module.  A device's ACPI handle
 * comes from acpica_get_handle().
 */

#include "nv-illumos.h"
#include "nv-reg.h"

#include <sys/acpi/acpi.h>
#include <sys/acpica.h>

#include "nvidia_acpi.h"

/* Maximum size of ACPI _DSM method's 4th argument */
#define NV_MAX_ACPI_DSM_PARAM_SIZE      1024

#define BIX_BATTERY_TECHNOLOGY_OFFSET   0x4
#define BIF_BATTERY_TECHNOLOGY_OFFSET   0x3
#define BATTERY_RECHARGABLE             0x1

#define ACPI_POWER_SOURCE_BUS_CHANGE_EVENT   0x00
#define ACPI_POWER_SOURCE_CHANGE_EVENT       0x80
#define ACPI_NVPCF_EVENT_CHANGE              0xC0

typedef struct nv_acpi_s {
    ACPI_HANDLE         handle;
    ACPI_NOTIFY_HANDLER handler;
    UINT32              type;
    void               *notifier_data;
    nvidia_stack_t     *sp;
} nv_acpi_t;

static ACPI_HANDLE nvif_handle;
static ACPI_HANDLE wmmx_handle;

static ACPI_HANDLE psr_handle;
static ACPI_HANDLE psr_device_handle;
static nv_acpi_t  *psr_nv_acpi_object;

static ACPI_HANDLE nvpcf_handle;
static ACPI_HANDLE nvpcf_device_handle;
static nv_acpi_t  *nvpcf_nv_acpi_object;

static NvBool battery_present;

ACPI_HANDLE
nv_acpi_dev_handle(dev_info_t *dip)
{
    ACPI_HANDLE handle = NULL;

    if (dip == NULL || acpica_get_handle(dip, &handle) != AE_OK)
        return NULL;

    return handle;
}

ACPI_STATUS
nv_acpi_evaluate_integer(ACPI_HANDLE handle, char *path, UINT64 *value)
{
    ACPI_BUFFER buf = { sizeof (ACPI_OBJECT), NULL };
    ACPI_OBJECT obj;
    ACPI_STATUS status;

    buf.Pointer = &obj;
    status = AcpiEvaluateObjectTyped(handle, path, NULL, &buf,
        ACPI_TYPE_INTEGER);
    if (ACPI_FAILURE(status))
        return status;

    *value = obj.Integer.Value;
    return AE_OK;
}

NV_STATUS NV_API_CALL nv_acpi_get_powersource(NvU32 *ac_plugged)
{
    UINT64 val;

    if (ac_plugged == NULL || psr_device_handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (ACPI_FAILURE(nv_acpi_evaluate_integer(psr_device_handle, "_PSR", &val)))
        return NV_ERR_GENERIC;

    *ac_plugged = (val == 0x1);
    return NV_OK;
}

static void
nv_acpi_powersource_hotplug_event(ACPI_HANDLE handle, UINT32 event_type,
    void *data)
{
    nv_acpi_t *obj = data;
    NvU32 ac_plugged = 0;

    if (event_type == ACPI_POWER_SOURCE_CHANGE_EVENT ||
        event_type == ACPI_POWER_SOURCE_BUS_CHANGE_EVENT)
    {
        if (nv_acpi_get_powersource(&ac_plugged) != NV_OK)
            return;

        rm_power_source_change_event(obj->sp, !ac_plugged);
    }
}

static void
nv_acpi_nvpcf_event(ACPI_HANDLE handle, UINT32 event_type, void *data)
{
    nv_acpi_t *obj = data;

    if (event_type == ACPI_NVPCF_EVENT_CHANGE)
        rm_acpi_nvpcf_notify(obj->sp);
    else
        nv_printf(NV_DBG_INFO, "NVRM: %s: NVPCF event 0x%x is not supported\n",
                  __func__, event_type);
}

static void
nv_acpi_notify_event(ACPI_HANDLE handle, UINT32 event_type, void *data)
{
    nv_acpi_t *obj = data;
    nv_illumos_state_t *nvis = obj->notifier_data;

    /* Display hotplug, GPS and D-notifier events for this GPU. */
    rm_acpi_notify(obj->sp, NV_STATE_PTR(nvis), event_type);
}

static nv_acpi_t *
nv_install_notifier(ACPI_HANDLE handle, ACPI_NOTIFY_HANDLER handler,
    UINT32 type, void *notifier_data)
{
    nv_acpi_t *obj;

    if (handle == NULL)
        return NULL;

    obj = kmem_zalloc(sizeof (*obj), KM_SLEEP);
    if (nv_stack_alloc(&obj->sp) != 0)
    {
        kmem_free(obj, sizeof (*obj));
        return NULL;
    }

    obj->handle = handle;
    obj->handler = handler;
    obj->type = type;
    obj->notifier_data = notifier_data;

    if (ACPI_FAILURE(AcpiInstallNotifyHandler(handle, type, handler, obj)))
    {
        nv_stack_free(obj->sp);
        kmem_free(obj, sizeof (*obj));
        return NULL;
    }

    return obj;
}

static void
nv_uninstall_notifier(nv_acpi_t *obj)
{
    ACPI_STATUS status;

    if (obj == NULL)
        return;

    status = AcpiRemoveNotifyHandler(obj->handle, obj->type, obj->handler);
    if (ACPI_FAILURE(status))
    {
        nv_printf(NV_DBG_INFO,
            "NVRM: failed to remove event notification handler (%d)!\n",
            status);
        return;
    }

    nv_stack_free(obj->sp);
    kmem_free(obj, sizeof (*obj));
}

void
nv_acpi_register_notifier(nv_illumos_state_t *nvis)
{
    ACPI_HANDLE handle = nv_acpi_dev_handle(nvis->dip);

    if (nvis->acpi_object != NULL || handle == NULL)
        return;

    nvis->acpi_object = nv_install_notifier(handle, nv_acpi_notify_event,
        ACPI_DEVICE_NOTIFY, nvis);
    if (nvis->acpi_object == NULL)
        nv_printf(NV_DBG_ERRORS,
            "NVRM: nv_acpi_register_notifier: failed to install notifier\n");
}

void
nv_acpi_unregister_notifier(nv_illumos_state_t *nvis)
{
    if (nvis->acpi_object != NULL)
    {
        nv_uninstall_notifier(nvis->acpi_object);
        nvis->acpi_object = NULL;
    }
}

static ACPI_STATUS
nv_acpi_find_methods(ACPI_HANDLE handle, UINT32 nest_level, void *context,
    void **return_value)
{
    ACPI_HANDLE method_handle;

    if (AcpiGetHandle(handle, "NVIF", &method_handle) == AE_OK)
        nvif_handle = method_handle;

    if (AcpiGetHandle(handle, "WMMX", &method_handle) == AE_OK)
        wmmx_handle = method_handle;

    if (AcpiGetHandle(handle, "_PSR", &method_handle) == AE_OK)
    {
        psr_handle = method_handle;
        psr_device_handle = handle;
    }

    if (AcpiGetHandle(handle, "NPCF", &method_handle) == AE_OK)
    {
        nvpcf_handle = method_handle;
        nvpcf_device_handle = handle;
    }

    return AE_OK;
}

void NV_API_CALL nv_acpi_methods_init(NvU32 *handlesPresent)
{
    if (handlesPresent == NULL)
        return;

    *handlesPresent = 0;

    (void) AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
        ACPI_UINT32_MAX, nv_acpi_find_methods, NULL, NULL, NULL);

    if (nvif_handle != NULL)
        *handlesPresent = NV_ACPI_NVIF_HANDLE_PRESENT;

    if (wmmx_handle != NULL)
        *handlesPresent |= NV_ACPI_WMMX_HANDLE_PRESENT;

    /* _PSR is system wide; one notifier serves every GPU. */
    if (psr_handle != NULL && psr_nv_acpi_object == NULL)
    {
        psr_nv_acpi_object = nv_install_notifier(psr_device_handle,
            nv_acpi_powersource_hotplug_event, ACPI_ALL_NOTIFY, NULL);
    }

    if (nvpcf_handle != NULL && nvpcf_nv_acpi_object == NULL)
    {
        nvpcf_nv_acpi_object = nv_install_notifier(nvpcf_device_handle,
            nv_acpi_nvpcf_event, ACPI_DEVICE_NOTIFY, NULL);
    }
}

void NV_API_CALL nv_acpi_methods_uninit(void)
{
    nvif_handle = NULL;
    wmmx_handle = NULL;

    if (psr_nv_acpi_object != NULL)
    {
        nv_uninstall_notifier(psr_nv_acpi_object);
        psr_handle = NULL;
        psr_device_handle = NULL;
        psr_nv_acpi_object = NULL;
    }

    if (nvpcf_nv_acpi_object != NULL)
    {
        nv_uninstall_notifier(nvpcf_nv_acpi_object);
        nvpcf_handle = NULL;
        nvpcf_device_handle = NULL;
        nvpcf_nv_acpi_object = NULL;
    }
}

static NV_STATUS nv_acpi_extract_object(const ACPI_OBJECT *, void *, NvU32,
    NvU32 *);

static NV_STATUS
nv_acpi_extract_integer(const ACPI_OBJECT *obj, void *buffer,
    NvU32 buffer_size, NvU32 *data_size)
{
    if (obj->Type != ACPI_TYPE_INTEGER)
        return NV_ERR_INVALID_ARGUMENT;

    if (obj->Integer.Value & ~0xffffffffULL)
        *data_size = sizeof (obj->Integer.Value);
    else
        *data_size = sizeof (NvU32);

    if (buffer_size < *data_size)
        return NV_ERR_BUFFER_TOO_SMALL;

    bcopy(&obj->Integer.Value, buffer, *data_size);
    return NV_OK;
}

static NV_STATUS
nv_acpi_extract_buffer(const ACPI_OBJECT *obj, void *buffer,
    NvU32 buffer_size, NvU32 *data_size)
{
    if (obj->Type != ACPI_TYPE_BUFFER)
        return NV_ERR_INVALID_ARGUMENT;

    *data_size = obj->Buffer.Length;

    if (buffer_size < obj->Buffer.Length)
        return NV_ERR_BUFFER_TOO_SMALL;

    bcopy(obj->Buffer.Pointer, buffer, *data_size);
    return NV_OK;
}

static NV_STATUS
nv_acpi_extract_package(const ACPI_OBJECT *obj, void *buffer,
    NvU32 buffer_size, NvU32 *data_size)
{
    NV_STATUS status = NV_OK;
    NvU32 i, element_size = 0;

    if (obj->Type != ACPI_TYPE_PACKAGE)
        return NV_ERR_INVALID_ARGUMENT;

    *data_size = 0;
    for (i = 0; i < obj->Package.Count; i++)
    {
        if (element_size > buffer_size)
            return NV_ERR_BUFFER_TOO_SMALL;

        buffer = (char *)buffer + element_size;
        buffer_size -= element_size;

        status = nv_acpi_extract_object(&obj->Package.Elements[i], buffer,
            buffer_size, &element_size);
        if (status != NV_OK)
            break;

        *data_size += element_size;
    }

    return status;
}

static NV_STATUS
nv_acpi_extract_object(const ACPI_OBJECT *obj, void *buffer,
    NvU32 buffer_size, NvU32 *data_size)
{
    switch (obj->Type)
    {
        case ACPI_TYPE_INTEGER:
            return nv_acpi_extract_integer(obj, buffer, buffer_size, data_size);
        case ACPI_TYPE_BUFFER:
            return nv_acpi_extract_buffer(obj, buffer, buffer_size, data_size);
        case ACPI_TYPE_PACKAGE:
            return nv_acpi_extract_package(obj, buffer, buffer_size, data_size);
        case ACPI_TYPE_ANY:
            /* An uninitialized object is not an error. */
            *data_size = 0;
            return NV_OK;
        default:
            return NV_ERR_NOT_SUPPORTED;
    }
}

static NV_STATUS
nv_acpi_nvif_method(NvU32 function, NvU32 subFunction, void *inParams,
    NvU16 inParamSize, NvU32 *outStatus, void *outData, NvU16 *outDataSize)
{
    ACPI_OBJECT_LIST input;
    ACPI_BUFFER output = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_OBJECT params[3];
    ACPI_OBJECT *nvif;
    NvU8 localInParams[8];
    NV_STATUS status = NV_OK;
    NvU16 localOutDataSize;

    if (nvif_handle == NULL)
        return NV_ERR_NOT_SUPPORTED;

    if (!nv_may_sleep())
        return NV_ERR_NOT_SUPPORTED;

    params[0].Integer.Type = ACPI_TYPE_INTEGER;
    params[0].Integer.Value = function;
    params[1].Integer.Type = ACPI_TYPE_INTEGER;
    params[1].Integer.Value = subFunction;
    params[2].Buffer.Type = ACPI_TYPE_BUFFER;

    if (inParams != NULL && inParamSize > 0)
    {
        params[2].Buffer.Length = inParamSize;
        params[2].Buffer.Pointer = inParams;
    }
    else
    {
        bzero(localInParams, sizeof (localInParams));
        params[2].Buffer.Length = sizeof (localInParams);
        params[2].Buffer.Pointer = localInParams;
    }

    input.Count = 3;
    input.Pointer = params;

    if (ACPI_FAILURE(AcpiEvaluateObject(nvif_handle, NULL, &input, &output)))
    {
        nv_printf(NV_DBG_INFO,
            "NVRM: nv_acpi_nvif_method: failed to get NVIF data, "
            "function 0x%x, subFunction 0x%x!\n", function, subFunction);
        return NV_ERR_GENERIC;
    }

    nvif = output.Pointer;
    if (nvif != NULL && nvif->Type == ACPI_TYPE_BUFFER && nvif->Buffer.Length >= 4)
    {
        if (outStatus != NULL)
        {
            *outStatus = nvif->Buffer.Pointer[3] << 24 |
                         nvif->Buffer.Pointer[2] << 16 |
                         nvif->Buffer.Pointer[1] << 8  |
                         nvif->Buffer.Pointer[0];
        }

        if (outData != NULL && outDataSize != NULL)
        {
            if (nvif->Buffer.Length - 4 > 0xffff)
            {
                status = NV_ERR_BUFFER_TOO_SMALL;
            }
            else
            {
                localOutDataSize = (NvU16)(nvif->Buffer.Length - 4);
                if (localOutDataSize <= *outDataSize)
                {
                    *outDataSize = localOutDataSize;
                    bcopy(&nvif->Buffer.Pointer[4], outData, localOutDataSize);
                }
                else
                {
                    *outDataSize = localOutDataSize;
                    status = NV_ERR_BUFFER_TOO_SMALL;
                }
            }
        }
    }
    else
    {
        nv_printf(NV_DBG_INFO,
            "NVRM: nv_acpi_nvif_method: NVIF data invalid, function 0x%x, "
            "subFunction 0x%x!\n", function, subFunction);
        status = NV_ERR_GENERIC;
    }

    AcpiOsFree(output.Pointer);
    return status;
}

static NV_STATUS
nv_acpi_wmmx_method(NvU32 arg2, NvU8 *outData, NvU16 *outDataSize)
{
    ACPI_OBJECT_LIST input;
    ACPI_BUFFER output = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_OBJECT params[3];
    ACPI_OBJECT *mmx;
    NV_STATUS status = NV_OK;

    if (wmmx_handle == NULL)
    {
        *outDataSize = 0;
        return NV_ERR_NOT_SUPPORTED;
    }

    if (!nv_may_sleep())
        return NV_ERR_NOT_SUPPORTED;

    /* Arguments 0 and 1 are unused by WMMX. */
    params[0].Integer.Type = ACPI_TYPE_INTEGER;
    params[0].Integer.Value = 0;
    params[1].Integer.Type = ACPI_TYPE_INTEGER;
    params[1].Integer.Value = 0;
    params[2].Integer.Type = ACPI_TYPE_INTEGER;
    params[2].Integer.Value = arg2;

    input.Count = 3;
    input.Pointer = params;

    if (ACPI_FAILURE(AcpiEvaluateObject(wmmx_handle, NULL, &input, &output)))
    {
        nv_printf(NV_DBG_INFO, "NVRM: nv_acpi_wmmx_method: failed to get WMMX data\n");
        return NV_ERR_GENERIC;
    }

    mmx = output.Pointer;
    if (mmx != NULL && mmx->Type == ACPI_TYPE_BUFFER && mmx->Buffer.Length > 0)
    {
        if (outData != NULL && outDataSize != NULL)
        {
            if (mmx->Buffer.Length <= *outDataSize)
            {
                *outDataSize = (NvU16)mmx->Buffer.Length;
                bcopy(mmx->Buffer.Pointer, outData, mmx->Buffer.Length);
            }
            else
            {
                status = NV_ERR_BUFFER_TOO_SMALL;
            }
        }
    }
    else
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: nv_acpi_wmmx_method: WMMX data invalid.\n");
        status = NV_ERR_GENERIC;
    }

    AcpiOsFree(output.Pointer);
    return status;
}

NV_STATUS NV_API_CALL nv_acpi_method(
    NvU32 acpi_method,
    NvU32 function,
    NvU32 subFunction,
    void  *inParams,
    NvU16 inParamSize,
    NvU32 *outStatus,
    void  *outData,
    NvU16 *outDataSize
)
{
    switch (acpi_method)
    {
        case NV_EVAL_ACPI_METHOD_NVIF:
            return nv_acpi_nvif_method(function, subFunction, inParams,
                inParamSize, outStatus, outData, outDataSize);
        case NV_EVAL_ACPI_METHOD_WMMX:
            return nv_acpi_wmmx_method(function, outData, outDataSize);
        default:
            return NV_ERR_NOT_SUPPORTED;
    }
}

static NV_STATUS
nv_acpi_evaluate_dsm_method(ACPI_HANDLE dev_handle, char *pathname,
    NvU8 *pAcpiDsmGuid, NvU32 acpiDsmRev, NvU32 acpiDsmSubFunction,
    void *arg3, NvU16 arg3Size, NvBool bArg3Integer, NvU32 *outStatus,
    void *pOutData, NvU16 *pSize)
{
    ACPI_OBJECT_LIST input;
    ACPI_BUFFER output = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_OBJECT params[4];
    ACPI_OBJECT *dsm;
    NV_STATUS status = NV_OK;
    NvU32 data_size = 0;

    if (!nv_may_sleep())
        return NV_ERR_NOT_SUPPORTED;

    params[0].Buffer.Type = ACPI_TYPE_BUFFER;
    params[0].Buffer.Length = 0x10;
    params[0].Buffer.Pointer = pAcpiDsmGuid;

    params[1].Integer.Type = ACPI_TYPE_INTEGER;
    params[1].Integer.Value = acpiDsmRev;

    params[2].Integer.Type = ACPI_TYPE_INTEGER;
    params[2].Integer.Value = acpiDsmSubFunction;

    if (bArg3Integer)
    {
        params[3].Integer.Type = ACPI_TYPE_INTEGER;
        params[3].Integer.Value = *((NvU32 *)arg3);
    }
    else
    {
        params[3].Buffer.Type = ACPI_TYPE_BUFFER;
        params[3].Buffer.Length = arg3Size;
        params[3].Buffer.Pointer = arg3;
    }

    input.Count = 4;
    input.Pointer = params;

    if (ACPI_FAILURE(AcpiEvaluateObject(dev_handle, pathname, &input, &output)))
    {
        nv_printf(NV_DBG_INFO, "NVRM: %s: failed to evaluate _DSM method!\n",
                  __func__);
        return NV_ERR_OPERATING_SYSTEM;
    }

    dsm = output.Pointer;
    if (dsm != NULL)
    {
        if (outStatus != NULL && dsm->Type == ACPI_TYPE_BUFFER &&
            dsm->Buffer.Length >= 4)
        {
            *outStatus = dsm->Buffer.Pointer[3] << 24 |
                         dsm->Buffer.Pointer[2] << 16 |
                         dsm->Buffer.Pointer[1] << 8  |
                         dsm->Buffer.Pointer[0];
        }

        status = nv_acpi_extract_object(dsm, pOutData, *pSize, &data_size);
        *pSize = (NvU16)MIN(data_size, 0xffff);
    }
    else
    {
        *pSize = 0;
    }

    if (status != NV_OK)
        nv_printf(NV_DBG_ERRORS, "NVRM: %s: DSM data invalid!\n", __func__);

    AcpiOsFree(output.Pointer);
    return status;
}

NV_STATUS NV_API_CALL nv_acpi_dsm_method(
    nv_state_t  *nv,
    NvU8        *pAcpiDsmGuid,
    NvU32        acpiDsmRev,
    NvBool       acpiNvpcfDsmFunction,
    NvU32        acpiDsmSubFunction,
    void        *pInParams,
    NvU16        inParamSize,
    NvU32       *outStatus,
    void        *pOutData,
    NvU16       *pSize
)
{
    ACPI_HANDLE dev_handle = nv_acpi_dev_handle(NV_GET_NVIS(nv)->dip);
    char *pathname = "_DSM";
    NvU8 *argument3;
    NV_STATUS status;

    if (dev_handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (pInParams == NULL || inParamSize > NV_MAX_ACPI_DSM_PARAM_SIZE ||
        pOutData == NULL || pSize == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    argument3 = kmem_alloc(MAX(inParamSize, 1), KM_SLEEP);
    bcopy(pInParams, argument3, inParamSize);

    if (acpiNvpcfDsmFunction)
    {
        /* There is no device handle for NVPCF; evaluate it by path. */
        dev_handle = NULL;
        pathname = "\\_SB.NPCF._DSM";
    }

    status = nv_acpi_evaluate_dsm_method(dev_handle, pathname, pAcpiDsmGuid,
        acpiDsmRev, acpiDsmSubFunction, argument3, inParamSize, NV_FALSE,
        outStatus, pOutData, pSize);

    kmem_free(argument3, MAX(inParamSize, 1));
    return status;
}

static ACPI_STATUS
nv_acpi_find_battery_info(ACPI_HANDLE handle, NvBool bUseBix)
{
    ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_OBJECT *pkg;
    NvU32 offset = bUseBix ? BIX_BATTERY_TECHNOLOGY_OFFSET :
        BIF_BATTERY_TECHNOLOGY_OFFSET;
    ACPI_STATUS ret = AE_OK;

    if (ACPI_FAILURE(AcpiEvaluateObject(handle, NULL, NULL, &buf)))
        return AE_OK;

    pkg = buf.Pointer;
    if (pkg != NULL && pkg->Type == ACPI_TYPE_PACKAGE &&
        pkg->Package.Count > offset &&
        pkg->Package.Elements[offset].Type == ACPI_TYPE_INTEGER &&
        pkg->Package.Elements[offset].Integer.Value == BATTERY_RECHARGABLE)
    {
        battery_present = NV_TRUE;
        ret = AE_CTRL_TERMINATE;
    }

    AcpiOsFree(buf.Pointer);
    return ret;
}

static ACPI_STATUS
nv_acpi_find_battery_device(ACPI_HANDLE handle, UINT32 nest_level,
    void *context, void **return_value)
{
    ACPI_HANDLE method;
    ACPI_STATUS status = AE_OK;

    if (AcpiGetHandle(handle, "_BIX", &method) == AE_OK)
        status = nv_acpi_find_battery_info(method, NV_TRUE);

    if (!battery_present && AcpiGetHandle(handle, "_BIF", &method) == AE_OK)
        status = nv_acpi_find_battery_info(method, NV_FALSE);

    return status;
}

NvBool NV_API_CALL nv_acpi_is_battery_present(void)
{
    (void) AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
        ACPI_UINT32_MAX, nv_acpi_find_battery_device, NULL, NULL, NULL);

    return battery_present;
}

NV_STATUS NV_API_CALL nv_acpi_d3cold_dsm_for_upstream_port(
    nv_state_t *nv,
    NvU8       *pAcpiDsmGuid,
    NvU32       acpiDsmRev,
    NvU32       acpiDsmSubFunction,
    NvU32      *data
)
{
    ACPI_HANDLE dev_handle;
    NvU32 outData = 0;
    NvU16 outDataSize = sizeof (NvU32);
    NV_STATUS status;

    dev_handle = nv_acpi_dev_handle(ddi_get_parent(NV_GET_NVIS(nv)->dip));
    if (dev_handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    status = nv_acpi_evaluate_dsm_method(dev_handle, "_DSM", pAcpiDsmGuid,
        acpiDsmRev, acpiDsmSubFunction, data, sizeof (NvU32), NV_TRUE,
        NULL, &outData, &outDataSize);
    if (status == NV_OK)
        *data = outData;

    return status;
}

NvBool NV_API_CALL nv_platform_supports_s0ix(void)
{
    return (AcpiGbl_FADT.Flags & ACPI_FADT_LOW_POWER_S0) != 0;
}
