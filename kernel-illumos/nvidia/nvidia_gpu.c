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
 * GPU lookup, bring-up and teardown, and the device reference interface used
 * by nvidia-modeset and nvidia-uvm.
 */

#include "nv-illumos.h"
#include "nv-reg.h"

void
nv_dev_free_stacks(nv_illumos_state_t *nvis)
{
    int i;

    for (i = 0; i < NV_DEV_STACK_COUNT; i++)
    {
        nv_stack_free(nvis->sp[i]);
        nvis->sp[i] = NULL;
    }
}

int
nv_dev_alloc_stacks(nv_illumos_state_t *nvis)
{
    int i;

    for (i = 0; i < NV_DEV_STACK_COUNT; i++)
    {
        if (nv_stack_alloc(&nvis->sp[i]) != 0)
        {
            nv_dev_free_stacks(nvis);
            return (ENOMEM);
        }
    }

    return (0);
}

/*
 * Lookups hold the device list as reader while taking the device's
 * ldata_lock, and return with ldata_lock held, so the device cannot be
 * detached between the lookup and its use.  Detach takes the list as writer
 * before ldata_lock, the same order as Linux.
 */
typedef NvBool (*nv_match_fn_t)(nv_illumos_state_t *, const void *);

static nv_illumos_state_t *
nv_find_locked(nv_match_fn_t match, const void *arg)
{
    nv_illumos_state_t *nvis;

    rw_enter(&nv_illumos_devices_lock, RW_READER);
    for (nvis = nv_illumos_devices; nvis != NULL; nvis = nvis->next)
    {
        sema_p(&nvis->ldata_lock);
        if (match(nvis, arg))
            break;
        sema_v(&nvis->ldata_lock);
    }
    rw_exit(&nv_illumos_devices_lock);

    return (nvis);
}

static NvBool
nv_match_minor(nv_illumos_state_t *nvis, const void *arg)
{
    return (nvis->minor_num == *(const NvU32 *)arg);
}

static NvBool
nv_match_gpu_id(nv_illumos_state_t *nvis, const void *arg)
{
    return (NV_STATE_PTR(nvis)->gpu_id == *(const NvU32 *)arg);
}

static NvBool
nv_match_uuid(nv_illumos_state_t *nvis, const void *arg)
{
    const NvU8 *uuid = nv_get_cached_uuid(NV_STATE_PTR(nvis));

    return (uuid != NULL && memcmp(uuid, arg, GPU_UUID_LEN) == 0);
}

static NvBool
nv_match_uuid_or_missing(nv_illumos_state_t *nvis, const void *arg)
{
    const NvU8 *uuid = nv_get_cached_uuid(NV_STATE_PTR(nvis));

    return (uuid == NULL || memcmp(uuid, arg, GPU_UUID_LEN) == 0);
}

nv_illumos_state_t *
nv_find_minor_locked(NvU32 minor)
{
    return nv_find_locked(nv_match_minor, &minor);
}

nv_illumos_state_t *
nv_find_gpu_id_locked(NvU32 gpu_id)
{
    return nv_find_locked(nv_match_gpu_id, &gpu_id);
}

static nv_illumos_state_t *
nv_find_uuid_locked(const NvU8 *uuid)
{
    return nv_find_locked(nv_match_uuid, uuid);
}

/*
 * The GPU with this UUID, or else one whose UUID is not yet known.  The
 * second pass also matches the UUID, which may have been cached since the
 * first.
 */
static nv_illumos_state_t *
nv_find_uuid_candidate_locked(const NvU8 *uuid)
{
    nv_illumos_state_t *nvis = nv_find_uuid_locked(uuid);

    if (nvis == NULL)
        nvis = nv_find_locked(nv_match_uuid_or_missing, uuid);

    return (nvis);
}

/* For getinfo(9E): the dip of /dev/nvidiaN without taking device locks. */
nv_illumos_state_t *
nv_find_minor(NvU32 minor)
{
    nv_illumos_state_t *nvis;

    rw_enter(&nv_illumos_devices_lock, RW_READER);
    for (nvis = nv_illumos_devices; nvis != NULL; nvis = nvis->next)
    {
        if (nvis->minor_num == minor)
            break;
    }
    rw_exit(&nv_illumos_devices_lock);

    return (nvis);
}

nv_state_t* NV_API_CALL nv_get_adapter_state(NvU32 domain, NvU8 bus, NvU8 slot)
{
    nv_illumos_state_t *nvis;
    nv_state_t *nv = NULL;

    rw_enter(&nv_illumos_devices_lock, RW_READER);
    for (nvis = nv_illumos_devices; nvis != NULL; nvis = nvis->next)
    {
        nv_state_t *cur = NV_STATE_PTR(nvis);

        if (cur->pci_info.domain == domain && cur->pci_info.bus == bus &&
            cur->pci_info.slot == slot)
        {
            nv = cur;
            break;
        }
    }
    rw_exit(&nv_illumos_devices_lock);

    return nv;
}

nv_state_t* NV_API_CALL nv_get_ctl_state(void)
{
    return NV_STATE_PTR(&nv_ctl_device);
}

NvU32 NV_API_CALL nv_get_dev_minor(nv_state_t *nv)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    if (nv->flags & NV_FLAG_CONTROL)
        return NV_MINOR_CTL;

    return nvis->minor_num;
}

/*
 * Brings up the device on first use.  Called with ldata_lock held.
 */
int
nv_start_device(nv_state_t *nv, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);
    NvBool power_ref = NV_FALSE;
    char name[32];
    int rc;

    if (nv->pci_info.device_id == 0)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: open of non-existent GPU with minor number %d\n",
            nvis->minor_num);
        return (ENXIO);
    }

    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
    {
        if (rm_ref_dynamic_power(sp, nv, NV_DYNAMIC_PM_COARSE) != NV_OK)
            return (EINVAL);
    }
    else
    {
        if (rm_ref_dynamic_power(sp, nv, NV_DYNAMIC_PM_FINE) != NV_OK)
            return (EINVAL);
    }
    power_ref = NV_TRUE;

    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
    {
        rc = nv_dev_alloc_stacks(nvis);
        if (rc != 0)
            goto failed;

        rc = nv_intr_setup(nvis);
        if (rc != 0)
        {
            NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
                "No interrupts of any type are available. Cannot use this GPU.\n");
            goto failed;
        }

        (void) snprintf(name, sizeof (name), "nvidia_queue_%d", nvis->instance);
        rc = nv_queue_init(&nvis->queue, name);
        if (rc != 0)
            goto failed_intr;
        nv->queue = &nvis->queue;
    }

    /*
     * RM keeps its I2C adapter table across shutdown and a full init only
     * adds ports; a port that did not come back would stay in the table
     * with no route, and rm_i2c_transfer() reports NV_OK for it.
     */
    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
        rm_i2c_remove_adapters(sp, nv);

    if (!rm_init_adapter(sp, nv))
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "rm_init_adapter failed, device minor number %d\n",
            nvis->minor_num);
        rc = EIO;
        goto failed_queue;
    }

    /* Generate and cache the UUID for future callers */
    (void) rm_get_gpu_uuid_raw(sp, nv);

    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
        nv_acpi_register_notifier(nvis);

    nv->flags |= NV_FLAG_INITIALIZED;

    rm_request_dnotifier_state(sp, nv);

    /* Balanced by the FINE ref at the start of nv_stop_device(). */
    rm_unref_dynamic_power(sp, nv, NV_DYNAMIC_PM_FINE);

    return (0);

failed_queue:
    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
    {
        nv->queue = NULL;
        nv_queue_fini(&nvis->queue);
    }
failed_intr:
    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
        nv_intr_teardown(nvis);
failed:
    if (nv->flags & NV_FLAG_TRIGGER_FLR)
    {
        os_pci_trigger_flr(nv->handle);
        nv->flags &= ~NV_FLAG_TRIGGER_FLR;
    }

    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
        nv_dev_free_stacks(nvis);

    if (power_ref)
        rm_unref_dynamic_power(sp, nv, NV_DYNAMIC_PM_COARSE);

    return (rc);
}

void
nv_shutdown_adapter(nvidia_stack_t *sp, nv_state_t *nv)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    rm_disable_adapter(sp, nv);

    nv_intr_teardown(nvis);

    if (nv->queue != NULL)
    {
        nv->queue = NULL;
        nv_queue_fini(&nvis->queue);
    }

    rm_shutdown_adapter(sp, nv);

    if (nv->flags & NV_FLAG_TRIGGER_FLR)
    {
        nv_printf(NV_DBG_INFO, "NVRM: Trigger FLR!\n");
        os_pci_trigger_flr(nv->handle);
        nv->flags &= ~NV_FLAG_TRIGGER_FLR;
    }
}

/*
 * Tears the device down on the last close.  Called with ldata_lock held.
 */
void
nv_stop_device(nv_state_t *nv, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);
    static int persistence_mode_notice_logged;

    if (!(nv->flags & NV_FLAG_INITIALIZED))
        return;

    rm_ref_dynamic_power(sp, nv, NV_DYNAMIC_PM_FINE);

    if (!nv->removed)
    {
        if (nv->flags & NV_FLAG_PERSISTENT_SW_STATE)
        {
            rm_disable_adapter(sp, nv);
        }
        else
        {
            nv_acpi_unregister_notifier(nvis);
            nv_shutdown_adapter(sp, nv);
        }
    }

    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
        nv_dev_free_stacks(nvis);

    if ((nv->flags & NV_FLAG_PERSISTENT_SW_STATE) &&
        !persistence_mode_notice_logged)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: Persistence mode is deprecated and"
                  " will be removed in a future release. Please use"
                  " nvidia-persistenced instead.\n");
        persistence_mode_notice_logged = 1;
    }

    nv->flags &= ~NV_FLAG_INITIALIZED;

    if (!(nv->flags & NV_FLAG_PERSISTENT_SW_STATE))
        rm_unref_dynamic_power(sp, nv, NV_DYNAMIC_PM_COARSE);
    else
        rm_unref_dynamic_power(sp, nv, NV_DYNAMIC_PM_FINE);
}

static void
nv_assert_not_in_gpu_exclusion_list(nvidia_stack_t *sp, nv_state_t *nv)
{
    char *uuid = rm_get_gpu_uuid(sp, nv);

    if (uuid == NULL)
    {
        NV_DEV_PRINTF(NV_DBG_INFO, nv, "Unable to read UUID");
        return;
    }

    if (nv_is_uuid_in_gpu_exclusion_list(uuid))
    {
        NV_DEV_PRINTF(NV_DBG_WARNINGS, nv,
                      "Could not exclude GPU %s because PBI is not supported\n",
                      uuid);
    }

    os_free_mem(uuid);
}

/*
 * Takes a usage reference on the device, starting it if needed.  Called with
 * ldata_lock held.
 */
int
nv_open_device(nv_state_t *nv, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);
    int rc;

    if ((nv->flags & NV_FLAG_EXCLUDE) != 0)
    {
        char *uuid = rm_get_gpu_uuid(sp, nv);

        NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "open() not permitted for excluded %s\n",
                      (uuid != NULL) ? uuid : "GPU");
        if (uuid != NULL)
            os_free_mem(uuid);
        return (EPERM);
    }

    if (NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv))
        return (ENODEV);

    if (!(nv->flags & NV_FLAG_INITIALIZED))
    {
        if (nvis->usage_count != 0)
        {
            NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
                "Minor device %u is referenced without being open!\n",
                nvis->minor_num);
            return (EBUSY);
        }

        rc = nv_start_device(nv, sp);
        if (rc != 0)
            return (rc);
    }
    else if (rm_is_device_sequestered(sp, nv))
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "Device is currently unavailable\n");
        return (EBUSY);
    }

    nv_assert_not_in_gpu_exclusion_list(sp, nv);

    atomic_inc_64(&nvis->usage_count);

    return (0);
}

/* Drops a usage reference.  Called with ldata_lock held. */
void
nv_close_device(nv_state_t *nv, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    if (nvis->usage_count == 0)
    {
        nv_printf(NV_DBG_ERRORS,
                  "NVRM: Attempting to close unopened minor device %u!\n",
                  nvis->minor_num);
        return;
    }

    if (atomic_dec_64_nv(&nvis->usage_count) == 0)
        nv_stop_device(nv, sp);
}

/*
 * Kernel-level open/close of a GPU by id, used by ATTACH_GPUS_TO_FD and by
 * nvidia-modeset and nvidia-uvm.  Return values follow the Linux convention
 * (0 or a negative errno) because the callers were written against it.
 */
int
nvidia_dev_get(NvU32 gpu_id, nvidia_stack_t *sp, NvBool reset_aware)
{
    nv_illumos_state_t *nvis;
    nv_state_t *nv;
    int rc;

    nvis = nv_find_gpu_id_locked(gpu_id);
    if (nvis == NULL)
        return (-ENODEV);

    nv = NV_STATE_PTR(nvis);
    rc = nv_open_device(nv, sp);
    if (rc == 0 && !reset_aware)
        (void) rm_set_external_kernel_client_count(sp, nv, NV_TRUE);
    sema_v(&nvis->ldata_lock);

    return (-rc);
}

void
nvidia_dev_put(NvU32 gpu_id, nvidia_stack_t *sp, NvBool reset_aware)
{
    nv_illumos_state_t *nvis;
    nv_state_t *nv;

    nvis = nv_find_gpu_id_locked(gpu_id);
    if (nvis == NULL)
        return;

    nv = NV_STATE_PTR(nvis);
    if (!reset_aware)
        (void) rm_set_external_kernel_client_count(sp, nv, NV_FALSE);
    nv_close_device(nv, sp);
    sema_v(&nvis->ldata_lock);
}

/*
 * UUID-based open/close for nvidia-uvm.  A GPU whose UUID is not cached yet
 * is opened to learn it, as on Linux.
 */
int
nvidia_dev_get_uuid(const NvU8 *uuid, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis;
    const NvU8 *dev_uuid;
    int rc;

    while ((nvis = nv_find_uuid_candidate_locked(uuid)) != NULL)
    {
        nv_state_t *nv = NV_STATE_PTR(nvis);

        rc = nv_open_device(nv, sp);
        if (rc != 0)
        {
            sema_v(&nvis->ldata_lock);
            return (-rc);
        }

        dev_uuid = nv_get_cached_uuid(nv);
        if (dev_uuid != NULL && memcmp(dev_uuid, uuid, GPU_UUID_LEN) == 0)
        {
            (void) rm_set_external_kernel_client_count(sp, nv, NV_TRUE);
            sema_v(&nvis->ldata_lock);
            return (0);
        }

        nv_close_device(nv, sp);
        sema_v(&nvis->ldata_lock);

        /* Opening always caches the UUID; a GPU without one cannot match. */
        if (dev_uuid == NULL)
            break;
    }

    return (-ENODEV);
}

void
nvidia_dev_put_uuid(const NvU8 *uuid, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis = nv_find_uuid_locked(uuid);

    if (nvis == NULL)
        return;

    nv_close_device(NV_STATE_PTR(nvis), sp);
    (void) rm_set_external_kernel_client_count(sp, NV_STATE_PTR(nvis), NV_FALSE);
    sema_v(&nvis->ldata_lock);
}

int
nvidia_dev_block_gc6(const NvU8 *uuid, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis = nv_find_uuid_locked(uuid);
    int rc = 0;

    if (nvis == NULL)
        return (-ENODEV);

    if (rm_ref_dynamic_power(sp, NV_STATE_PTR(nvis), NV_DYNAMIC_PM_FINE) != NV_OK)
        rc = -EINVAL;

    sema_v(&nvis->ldata_lock);
    return (rc);
}

int
nvidia_dev_unblock_gc6(const NvU8 *uuid, nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis = nv_find_uuid_locked(uuid);

    if (nvis == NULL)
        return (-ENODEV);

    rm_unref_dynamic_power(sp, NV_STATE_PTR(nvis), NV_DYNAMIC_PM_FINE);
    sema_v(&nvis->ldata_lock);
    return (0);
}

int
nvidia_dev_get_pci_info(const NvU8 *uuid, struct pci_dev **pci_dev_out,
    NvU64 *dma_start, NvU64 *dma_limit)
{
    nv_illumos_state_t *nvis = nv_find_uuid_locked(uuid);

    if (nvis == NULL)
        return (-ENODEV);

    *pci_dev_out = &nvis->pci_dev;
    *dma_start = nvis->dma_dev.addressable_range.start;
    *dma_limit = nvis->dma_dev.addressable_range.limit;

    /* nvidia-uvm binds its DMA mappings within this range. */
    nvis->pci_dev.dev.dma_start = *dma_start;
    nvis->pci_dev.dev.dma_limit = *dma_limit;

    sema_v(&nvis->ldata_lock);
    return (0);
}
