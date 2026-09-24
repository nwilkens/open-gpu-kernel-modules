/*
 * SPDX-FileCopyrightText: Copyright (c) 2015-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * Interface exported to the nvidia_modeset module.
 *
 * Callbacks into nvidia_modeset run under a reader lock that
 * set_callbacks() takes as writer, so unregistering waits for callbacks in
 * progress; nvidia_modeset can unload while this module stays loaded.
 */

#include "nv-illumos.h"
#include "nv-modeset-interface.h"
#include "nv-gpu-info.h"

static const nvidia_modeset_callbacks_t *nv_modeset_callbacks;
static krwlock_t nv_modeset_cb_lock;

void
nv_modeset_interface_init(void)
{
    rw_init(&nv_modeset_cb_lock, NULL, RW_DRIVER, NULL);
}

void
nv_modeset_interface_fini(void)
{
    rw_destroy(&nv_modeset_cb_lock);
}

static int
nvidia_modeset_rm_ops_alloc_stack(nvidia_stack_t **sp)
{
    return (nv_stack_alloc(sp) == 0) ? 0 : -ENOMEM;
}

static void
nvidia_modeset_rm_ops_free_stack(nvidia_stack_t *sp)
{
    nv_stack_free(sp);
}

static int
nvidia_modeset_set_callbacks(const nvidia_modeset_callbacks_t *cb)
{
    int rc = 0;

    rw_enter(&nv_modeset_cb_lock, RW_WRITER);
    if ((nv_modeset_callbacks != NULL && cb != NULL) ||
        (nv_modeset_callbacks == NULL && cb == NULL))
        rc = -EINVAL;
    else
        nv_modeset_callbacks = cb;
    rw_exit(&nv_modeset_cb_lock);

    return rc;
}

void
nvidia_modeset_suspend(NvU32 gpu_id)
{
    rw_enter(&nv_modeset_cb_lock, RW_READER);
    if (nv_modeset_callbacks != NULL)
        nv_modeset_callbacks->suspend(gpu_id);
    rw_exit(&nv_modeset_cb_lock);
}

void
nvidia_modeset_resume(NvU32 gpu_id)
{
    rw_enter(&nv_modeset_cb_lock, RW_READER);
    if (nv_modeset_callbacks != NULL)
        nv_modeset_callbacks->resume(gpu_id);
    rw_exit(&nv_modeset_cb_lock);
}

void
nvidia_modeset_remove(NvU32 gpu_id)
{
    rw_enter(&nv_modeset_cb_lock, RW_READER);
    if (nv_modeset_callbacks != NULL && nv_modeset_callbacks->remove != NULL)
        nv_modeset_callbacks->remove(gpu_id);
    rw_exit(&nv_modeset_cb_lock);
}

static void
nvidia_modeset_get_gpu_info(nv_gpu_info_t *gpu_info, nv_illumos_state_t *nvis)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);

    gpu_info->gpu_id = nv->gpu_id;
    gpu_info->pci_info.domain   = nv->pci_info.domain;
    gpu_info->pci_info.bus      = nv->pci_info.bus;
    gpu_info->pci_info.slot     = nv->pci_info.slot;
    gpu_info->pci_info.function = nv->pci_info.function;
    /* GPU memory is never onlined as a NUMA node on illumos. */
    gpu_info->needs_numa_setup = NV_FALSE;
    gpu_info->os_device_ptr = nvis->dip;
    gpu_info->is_soc_disp = NV_IS_SOC_DISPLAY_DEVICE(nv);
}

void
nvidia_modeset_probe(nv_illumos_state_t *nvis)
{
    nv_gpu_info_t gpu_info;

    rw_enter(&nv_modeset_cb_lock, RW_READER);
    if (nv_modeset_callbacks != NULL && nv_modeset_callbacks->probe != NULL)
    {
        nvidia_modeset_get_gpu_info(&gpu_info, nvis);
        nv_modeset_callbacks->probe(&gpu_info);
    }
    rw_exit(&nv_modeset_cb_lock);
}

static NvU32
nvidia_modeset_enumerate_gpus(nv_gpu_info_t *gpu_info)
{
    nv_illumos_state_t *nvis;
    NvU32 count = 0;

    rw_enter(&nv_illumos_devices_lock, RW_READER);
    for (nvis = nv_illumos_devices; nvis != NULL; nvis = nvis->next)
    {
        /* gpu_info[] has NV_MAX_GPUS elements. */
        if (count >= NV_MAX_GPUS)
        {
            nv_printf(NV_DBG_WARNINGS, "NVRM: More than %d GPUs found.",
                NV_MAX_GPUS);
            count = 0;
            break;
        }

        nvidia_modeset_get_gpu_info(&gpu_info[count], nvis);
        count++;
    }
    rw_exit(&nv_illumos_devices_lock);

    return count;
}

NV_STATUS
nvidia_get_rm_ops(nvidia_modeset_rm_ops_t *rm_ops)
{
    const nvidia_modeset_rm_ops_t local_rm_ops = {
        .version_string = NV_VERSION_STRING,
        .system_info    = {
            /* illumos maps BARs write-combined through the PAT. */
            .allow_write_combining = NV_TRUE,
        },
        .alloc_stack    = nvidia_modeset_rm_ops_alloc_stack,
        .free_stack     = nvidia_modeset_rm_ops_free_stack,
        .enumerate_gpus = nvidia_modeset_enumerate_gpus,
        .open_gpu       = nvidia_dev_get,
        .close_gpu      = nvidia_dev_put,
        .op             = rm_kernel_rmapi_op,
        .set_callbacks  = nvidia_modeset_set_callbacks,
    };

    if (strcmp(rm_ops->version_string, NV_VERSION_STRING) != 0)
    {
        rm_ops->version_string = NV_VERSION_STRING;
        return NV_ERR_GENERIC;
    }

    *rm_ops = local_rm_ops;
    return NV_OK;
}
