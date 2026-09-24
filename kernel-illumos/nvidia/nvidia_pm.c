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
 * GPU suspend and resume (DDI_SUSPEND and DDI_RESUME).
 */

#include "nv-illumos.h"

static kmutex_t nv_pm_lock;

/* GPUs currently suspended; NVKMS and UVM are quiesced while any are. */
static uint_t nv_suspended_gpus;

void
nv_pm_init(void)
{
    mutex_init(&nv_pm_lock, NULL, MUTEX_DRIVER, NULL);
}

void
nv_pm_fini(void)
{
    mutex_destroy(&nv_pm_lock);
}

static NV_STATUS
nv_power_management(nv_state_t *nv, nv_pm_action_t pm_action)
{
    nvidia_stack_t *sp = NULL;
    NV_STATUS status;

    if (nv_stack_alloc(&sp) != 0)
        return NV_ERR_NO_MEMORY;

    if (NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv))
    {
        NV_DEV_PRINTF(NV_DBG_INFO, nv, "GPU is lost, skipping PM event\n");
        nv_stack_free(sp);
        return NV_ERR_GPU_IS_LOST;
    }

    status = rm_power_management(sp, nv, pm_action);

    nv_stack_free(sp);
    return status;
}

/*
 * UVM expects user channels to be stopped before it suspends, so they cannot
 * stall on faults it is about to discard.  Called with ldata_lock held.
 */
static NV_STATUS
nv_preempt_user_channels(nv_illumos_state_t *nvis, nvidia_stack_t *sp)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);
    NV_STATUS status;

    if ((nv->flags & NV_FLAG_INITIALIZED) == 0)
        return NV_OK;

    status = rm_ref_dynamic_power(sp, nv, NV_DYNAMIC_PM_FINE);
    if (status != NV_OK)
        return status;

    sema_p(&nvis->mmap_lock);
    nv_set_safe_to_mmap_locked(nv, NV_FALSE);
    nv_revoke_mappings_locked(nvis);
    sema_v(&nvis->mmap_lock);

    nvis->channels_preempted = NV_TRUE;

    return rm_stop_user_channels(sp, nv);
}

static void
nv_restore_user_channels(nv_illumos_state_t *nvis, nvidia_stack_t *sp)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);

    if (!nvis->channels_preempted)
        return;
    nvis->channels_preempted = NV_FALSE;

    if (rm_restart_user_channels(sp, nv) != NV_OK)
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "failed to restart user channels\n");

    sema_p(&nvis->mmap_lock);
    nv_set_safe_to_mmap_locked(nv, NV_TRUE);
    sema_v(&nvis->mmap_lock);

    rm_unref_dynamic_power(sp, nv, NV_DYNAMIC_PM_FINE);
}

static void
nv_restore_all_user_channels(nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis;

    rw_enter(&nv_illumos_devices_lock, RW_READER);
    for (nvis = nv_illumos_devices; nvis != NULL; nvis = nvis->next)
    {
        sema_p(&nvis->ldata_lock);
        nv_restore_user_channels(nvis, sp);
        sema_v(&nvis->ldata_lock);
    }
    rw_exit(&nv_illumos_devices_lock);
}

static NV_STATUS
nv_preempt_all_user_channels(nvidia_stack_t *sp)
{
    nv_illumos_state_t *nvis;
    NV_STATUS status = NV_OK;

    rw_enter(&nv_illumos_devices_lock, RW_READER);
    for (nvis = nv_illumos_devices; nvis != NULL && status == NV_OK;
         nvis = nvis->next)
    {
        sema_p(&nvis->ldata_lock);
        status = nv_preempt_user_channels(nvis, sp);
        sema_v(&nvis->ldata_lock);
    }
    rw_exit(&nv_illumos_devices_lock);

    if (status != NV_OK)
        nv_restore_all_user_channels(sp);

    return status;
}

/*
 * DDI suspends GPUs one at a time.  User channels, NVKMS and UVM are
 * quiesced once, before the first GPU suspends, and resumed after the last
 * one resumes, the way the Linux system suspend path brackets its per-GPU
 * suspends.
 */
static NV_STATUS
nv_system_suspend_hold(void)
{
    nvidia_stack_t *sp = NULL;
    NV_STATUS status = NV_OK;

    if (nv_stack_alloc(&sp) != 0)
        return NV_ERR_NO_MEMORY;

    mutex_enter(&nv_pm_lock);
    if (nv_suspended_gpus == 0)
    {
        nvidia_modeset_suspend(0);
        status = nv_preempt_all_user_channels(sp);
        if (status == NV_OK)
        {
            status = nv_uvm_suspend();
            if (status != NV_OK)
                nv_restore_all_user_channels(sp);
        }
        if (status != NV_OK)
            nvidia_modeset_resume(0);
    }
    if (status == NV_OK)
        nv_suspended_gpus++;
    mutex_exit(&nv_pm_lock);

    nv_stack_free(sp);
    return status;
}

static void
nv_system_suspend_rele(void)
{
    nvidia_stack_t *sp = NULL;

    /* Resume cannot be refused; without a stack, channels stay stopped. */
    (void) nv_stack_alloc(&sp);

    mutex_enter(&nv_pm_lock);
    if (nv_suspended_gpus > 0 && --nv_suspended_gpus == 0)
    {
        if (nv_uvm_resume() != NV_OK)
            nv_printf(NV_DBG_ERRORS, "NVRM: UVM resume failed\n");
        if (sp != NULL)
            nv_restore_all_user_channels(sp);
        nvidia_modeset_resume(0);
    }
    mutex_exit(&nv_pm_lock);

    if (sp != NULL)
        nv_stack_free(sp);
}

int
nv_suspend_gpu(nv_illumos_state_t *nvis)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);
    NV_STATUS status;

    status = nv_system_suspend_hold();
    if (status != NV_OK)
        return (DDI_FAILURE);

    sema_p(&nvis->ldata_lock);

    if ((nv->flags & NV_FLAG_INITIALIZED) == 0 &&
        (nv->flags & NV_FLAG_PERSISTENT_SW_STATE) == 0)
        goto done;

    if (nv->is_pm_unsupported)
    {
        status = NV_ERR_NOT_SUPPORTED;
        goto done;
    }

    if ((nv->flags & NV_FLAG_SUSPENDED) != 0)
        goto done;

    /*
     * Preserving video memory writes it to a file, which is not safe once
     * DDI_SUSPEND has begun; Linux refuses this outside its procfs suspend.
     */
    if (nv->preserve_vidmem_allocations &&
        nv_dev_needs_vidmem_preservation(nv))
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "suspend with NVreg_PreserveVideoMemoryAllocations is not supported\n");
        status = NV_ERR_NOT_SUPPORTED;
        goto done;
    }

    nvidia_modeset_suspend(nv->gpu_id);
    status = nv_power_management(nv, NV_PM_ACTION_STANDBY);
    if (status == NV_OK)
    {
        nv->flags |= NV_FLAG_SUSPENDED;
    }
    else
    {
        (void) nv_power_management(nv, NV_PM_ACTION_RESUME);
        nvidia_modeset_resume(nv->gpu_id);
    }

done:
    sema_v(&nvis->ldata_lock);

    if (status == NV_OK)
    {
        (void) pci_save_config_regs(nvis->dip);
        return (DDI_SUCCESS);
    }

    nv_system_suspend_rele();

    return (DDI_FAILURE);
}

int
nv_resume_gpu(nv_illumos_state_t *nvis)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);
    NV_STATUS status = NV_OK;

    (void) pci_restore_config_regs(nvis->dip);

    sema_p(&nvis->ldata_lock);

    if ((nv->flags & NV_FLAG_SUSPENDED) == 0)
        goto done;

    status = nv_power_management(nv, NV_PM_ACTION_RESUME);
    if (status == NV_OK)
    {
        nvidia_modeset_resume(nv->gpu_id);
        nv->flags &= ~NV_FLAG_SUSPENDED;
    }
    else
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "resume failed: 0x%x\n", status);
    }

done:
    sema_v(&nvis->ldata_lock);

    nv_system_suspend_rele();

    return (status == NV_OK) ? DDI_SUCCESS : DDI_FAILURE;
}
