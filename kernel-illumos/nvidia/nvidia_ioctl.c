/*
 * SPDX-FileCopyrightText: Copyright (c) 1999-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * ioctl(9E) for the control, GPU and capability nodes.
 */

#include "nv-illumos.h"
#include "nv-ioctl.h"
#include "nv_speculation_barrier.h"

#include <sys/policy.h>

/*
 * Fills ci[] with the attached, non-excluded GPUs.  Fails if the caller's
 * array cannot hold all of them.
 */
static int
nvidia_read_card_info(nv_ioctl_card_info_t *ci, size_t num_entries)
{
    nv_illumos_state_t *nvis;
    size_t i = 0, count = 0;
    int rc = 0;

    bzero(ci, num_entries * sizeof (ci[0]));

    rw_enter(&nv_illumos_devices_lock, RW_READER);

    for (nvis = nv_illumos_devices; nvis != NULL; nvis = nvis->next)
        count++;

    if (num_entries < count)
    {
        rc = EINVAL;
        goto out;
    }

    for (nvis = nv_illumos_devices; nvis != NULL && i < num_entries;
         nvis = nvis->next)
    {
        nv_state_t *nv = NV_STATE_PTR(nvis);

        if ((nv->flags & NV_FLAG_EXCLUDE) != 0)
            continue;

        ci[i].valid              = NV_TRUE;
        ci[i].pci_info.domain    = nv->pci_info.domain;
        ci[i].pci_info.bus       = nv->pci_info.bus;
        ci[i].pci_info.slot      = nv->pci_info.slot;
        ci[i].pci_info.vendor_id = nv->pci_info.vendor_id;
        ci[i].pci_info.device_id = nv->pci_info.device_id;
        ci[i].gpu_id             = nv->gpu_id;
        ci[i].interrupt_line     = nv->interrupt_line;
        ci[i].reg_address        = nv->regs->cpu_address;
        ci[i].reg_size           = nv->regs->size;
        ci[i].minor_number       = nvis->minor_num;
        ci[i].fb_address         = nv->fb->cpu_address;
        ci[i].fb_size            = nv->fb->size;
        i++;
    }

out:
    rw_exit(&nv_illumos_devices_lock);
    return (rc);
}

static NV_STATUS
nv_validate_ioctl_data(unsigned int arg_cmd, size_t arg_size)
{
    static const struct {
        unsigned int cmdKey;
        size_t paramSize;
        NvBool isArgumentArray;
    } nv_ioctls_table[] = {
#define _NV_IOCTL_ENTRY(_cmd, _type, _isArgumentArray) \
        { (_cmd & 0xFF), sizeof (_type), _isArgumentArray }
        _NV_IOCTL_ENTRY(NV_ESC_CHECK_VERSION_STR, nv_ioctl_rm_api_version_t, NV_FALSE),
        _NV_IOCTL_ENTRY(NV_ESC_IOCTL_XFER_CMD, nv_ioctl_xfer_t, NV_FALSE),
        _NV_IOCTL_ENTRY(NV_ESC_ATTACH_GPUS_TO_FD, NvU32, NV_TRUE),
        _NV_IOCTL_ENTRY(NV_ESC_CARD_INFO, nv_ioctl_card_info_t, NV_TRUE),
        _NV_IOCTL_ENTRY(NV_ESC_QUERY_DEVICE_INTR, nv_ioctl_query_device_intr_t, NV_FALSE),
        _NV_IOCTL_ENTRY(NV_ESC_SYS_PARAMS, nv_ioctl_sys_params_t, NV_FALSE),
        _NV_IOCTL_ENTRY(NV_ESC_EXPORT_TO_DMABUF_FD, nv_ioctl_export_to_dma_buf_fd_t, NV_FALSE),
        _NV_IOCTL_ENTRY(NV_ESC_WAIT_OPEN_COMPLETE, nv_ioctl_wait_open_complete_t, NV_FALSE),
        _NV_IOCTL_ENTRY(NV_ESC_NUMA_INFO, nv_ioctl_numa_info_t, NV_FALSE),
        _NV_IOCTL_ENTRY(NV_ESC_SET_NUMA_STATUS, nv_ioctl_set_numa_status_t, NV_FALSE),
#undef _NV_IOCTL_ENTRY
    };
    NV_STATUS status;
    size_t i;

    status = rm_validate_ioctls(arg_cmd, (NvU32)arg_size);
    if (status != NV_ERR_INVALID_COMMAND)
        return status;

    for (i = 0; i < NV_ARRAY_ELEMENTS(nv_ioctls_table); i++)
    {
        if (nv_ioctls_table[i].cmdKey != arg_cmd)
            continue;

        if (nv_ioctls_table[i].isArgumentArray ?
                (arg_size != 0 && (arg_size % nv_ioctls_table[i].paramSize) == 0) :
                (arg_size == nv_ioctls_table[i].paramSize))
            return NV_OK;

        nv_printf(NV_DBG_ERRORS,
            "NVRM: invalid %u structure size, expected %lu, got %lu!\n",
            arg_cmd, (unsigned long)nv_ioctls_table[i].paramSize,
            (unsigned long)arg_size);
        return NV_ERR_INVALID_ARGUMENT;
    }

    nv_printf(NV_DBG_ERRORS, "NVRM: unknown NVRM ioctl command: 0x%x\n", arg_cmd);
    return NV_ERR_INVALID_ARGUMENT;
}

static int
nvidia_ioctl_attach_gpus(nv_illumos_file_private_t *nvifp, void *arg_copy,
    size_t arg_size, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis = nvifp->nvis;
    size_t num_arg_gpus = arg_size / sizeof (NvU32);
    NvU32 *gpus;
    size_t i;

    if (num_arg_gpus == 0)
        return (EINVAL);

    gpus = kmem_alloc(arg_size, KM_SLEEP);
    bcopy(arg_copy, gpus, arg_size);

    sema_p(&nvis->ldata_lock);
    if (nvifp->num_attached_gpus != 0)
    {
        sema_v(&nvis->ldata_lock);
        kmem_free(gpus, arg_size);
        return (EINVAL);
    }
    nvifp->attached_gpus = gpus;
    nvifp->num_attached_gpus = num_arg_gpus;
    sema_v(&nvis->ldata_lock);

    for (i = 0; i < num_arg_gpus; i++)
    {
        if (gpus[i] == 0)
            continue;

        if (nvidia_dev_get(gpus[i], sp, NV_FALSE) != 0)
        {
            while (i-- > 0)
            {
                if (gpus[i] != 0)
                    nvidia_dev_put(gpus[i], sp, NV_FALSE);
            }

            sema_p(&nvis->ldata_lock);
            nvifp->attached_gpus = NULL;
            nvifp->num_attached_gpus = 0;
            sema_v(&nvis->ldata_lock);
            kmem_free(gpus, arg_size);
            return (EINVAL);
        }
    }

    return (0);
}

int
nvidia_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
    nv_illumos_file_private_t *nvifp;
    nv_illumos_state_t *nvis;
    nv_state_t *nv;
    nvidia_stack_t *sp = NULL;
    nv_ioctl_xfer_t ioc_xfer;
    void *arg_ptr = (void *)arg;
    void *arg_copy = NULL;
    size_t arg_size;
    unsigned int arg_cmd;
    uint32_t ucmd = (uint32_t)cmd;
    int status = 0;

    *rvalp = 0;

    nvifp = nv_file_private_hold(dev);
    if (nvifp == NULL)
        return (ENXIO);

    switch (nvifp->kind)
    {
        case NV_NODE_CTL:
        case NV_NODE_GPU:
            break;
        case NV_NODE_NVLINK:
            status = nvlink_node_ioctl(nvifp, cmd, arg, mode, credp);
            nv_file_private_rele(nvifp);
            return (status);
        case NV_NODE_NVSWITCH_CTL:
        case NV_NODE_NVSWITCH:
            status = nvswitch_node_ioctl(nvifp, cmd, arg, mode, credp);
            nv_file_private_rele(nvifp);
            return (status);
        default:
            nv_file_private_rele(nvifp);
            return (ENOTTY);
    }

    if (NV_IOC_TYPE(ucmd) != NV_IOCTL_MAGIC || NV_IOC_DIR(ucmd) != NV_IOC_INOUT)
    {
        nv_file_private_rele(nvifp);
        return (ENOTTY);
    }

    arg_size = NV_IOC_SIZE(ucmd);
    arg_cmd = NV_IOC_NR(ucmd);

    if (nv_validate_ioctl_data(arg_cmd, arg_size) != NV_OK)
    {
        nv_file_private_rele(nvifp);
        return (EINVAL);
    }

    if (arg_cmd == NV_ESC_IOCTL_XFER_CMD)
    {
        if (ddi_copyin(arg_ptr, &ioc_xfer, sizeof (ioc_xfer), mode) != 0)
        {
            status = EFAULT;
            goto done_early;
        }

        arg_cmd = ioc_xfer.cmd;
        arg_size = ioc_xfer.size;
        arg_ptr = NvP64_VALUE(ioc_xfer.ptr);

        if (arg_size > NV_ABSOLUTE_MAX_IOCTL_SIZE || arg_size == 0)
        {
            nv_printf(NV_DBG_ERRORS, "NVRM: invalid ioctl XFER size!\n");
            status = EINVAL;
            goto done_early;
        }

        if (arg_cmd == NV_ESC_IOCTL_XFER_CMD ||
            nv_validate_ioctl_data(arg_cmd, arg_size) != NV_OK)
        {
            status = EINVAL;
            goto done_early;
        }
    }

    if (arg_size == 0)
    {
        status = EINVAL;
        goto done_early;
    }

    arg_copy = kmem_alloc(arg_size, KM_SLEEP);

    if (ddi_copyin(arg_ptr, arg_copy, arg_size, mode) != 0)
    {
        status = EFAULT;
        goto done_early;
    }

    if (arg_cmd == NV_ESC_WAIT_OPEN_COMPLETE)
    {
        nv_ioctl_wait_open_complete_t *params = arg_copy;

        params->rc = nvifp->open_rc;
        params->adapterStatus = nvifp->adapter_status;
        goto done_early;
    }

    nvis = nvifp->nvis;
    if (nvis == NULL)
    {
        status = EIO;
        goto done_early;
    }
    nv = NV_STATE_PTR(nvis);

    if (nv_stack_alloc(&sp) != 0)
    {
        status = ENOMEM;
        goto done_early;
    }

    if (NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv))
    {
        status = EINVAL;
        goto done;
    }

    switch (arg_cmd)
    {
        case NV_ESC_QUERY_DEVICE_INTR:
        {
            nv_ioctl_query_device_intr_t *query_intr = arg_copy;

            if ((nv->flags & NV_FLAG_CONTROL) != 0 || nv->regs->map == NULL)
            {
                status = EINVAL;
                break;
            }

            query_intr->intrStatus =
                *(nv->regs->map + (NV_RM_DEVICE_INTR_ADDRESS >> 2));
            query_intr->status = NV_OK;
            break;
        }

        case NV_ESC_CARD_INFO:
            if ((nv->flags & NV_FLAG_CONTROL) == 0)
            {
                status = EINVAL;
                break;
            }
            status = nvidia_read_card_info(arg_copy,
                arg_size / sizeof (nv_ioctl_card_info_t));
            break;

        case NV_ESC_ATTACH_GPUS_TO_FD:
            if ((nv->flags & NV_FLAG_CONTROL) == 0)
            {
                status = EINVAL;
                break;
            }
            status = nvidia_ioctl_attach_gpus(nvifp, arg_copy, arg_size, sp);
            break;

        case NV_ESC_CHECK_VERSION_STR:
            if ((nv->flags & NV_FLAG_CONTROL) == 0)
            {
                status = EINVAL;
                break;
            }
            status = (rm_perform_version_check(sp, arg_copy, (NvU32)arg_size)
                == NV_OK) ? 0 : EINVAL;
            break;

        case NV_ESC_SYS_PARAMS:
        {
            nv_ioctl_sys_params_t *api = arg_copy;

            if ((nv->flags & NV_FLAG_CONTROL) == 0)
            {
                status = EINVAL;
                break;
            }

            sema_p(&nvis->ldata_lock);
            if (nvis->numa_memblock_size == 0)
                nvis->numa_memblock_size = api->memblock_size;
            else if (nvis->numa_memblock_size != api->memblock_size)
                status = EBUSY;
            sema_v(&nvis->ldata_lock);
            break;
        }

        case NV_ESC_NUMA_INFO:
        {
            nv_ioctl_numa_info_t *api = arg_copy;

            if ((nv->flags & NV_FLAG_CONTROL) != 0)
            {
                status = EINVAL;
                break;
            }

            if (rm_get_gpu_numa_info(sp, nv, api) != NV_OK)
            {
                status = EBUSY;
                break;
            }

            /* GPU memory is never onlined as a NUMA node on illumos. */
            api->status = NV_IOCTL_NUMA_STATUS_DISABLED;
            api->use_auto_online = NV_FALSE;
            api->memblock_size = nv_ctl_device.numa_memblock_size;
            break;
        }

        case NV_ESC_SET_NUMA_STATUS:
        {
            nv_ioctl_set_numa_status_t *api = arg_copy;

            if (secpolicy_sys_config(credp, B_FALSE) != 0)
            {
                status = EACCES;
                break;
            }

            if ((nv->flags & NV_FLAG_CONTROL) != 0)
            {
                status = EINVAL;
                break;
            }

            status = (api->status == NV_IOCTL_NUMA_STATUS_DISABLED) ?
                0 : ENOTSUP;
            break;
        }

        case NV_ESC_EXPORT_TO_DMABUF_FD:
        {
            nv_ioctl_export_to_dma_buf_fd_t *params = arg_copy;

            if ((nv->flags & NV_FLAG_CONTROL) != 0)
            {
                status = EINVAL;
                break;
            }

            params->status = NV_ERR_NOT_SUPPORTED;
            break;
        }

        default:
        {
            NvBool status_code = (arg_cmd == NV_ESC_STATUS_CODE);

            /* This escape uses a GPU found by nv_get_adapter_state(). */
            if (status_code)
                rw_enter(&nv_adapter_state_lock, RW_READER);
            status = (rm_ioctl(sp, nv, &nvifp->nvfp, arg_cmd, arg_copy,
                (NvU32)arg_size) == NV_OK) ? 0 : EINVAL;
            if (status_code)
                rw_exit(&nv_adapter_state_lock);
            break;
        }
    }

done:
    nv_stack_free(sp);

done_early:
    if (arg_copy != NULL)
    {
        if (status != EFAULT &&
            ddi_copyout(arg_copy, arg_ptr, arg_size, mode) != 0)
        {
            nv_printf(NV_DBG_ERRORS, "NVRM: failed to copy out ioctl data\n");
            status = EFAULT;
        }
        kmem_free(arg_copy, arg_size);
    }

    nv_file_private_rele(nvifp);
    return (status);
}
