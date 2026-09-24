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
 * Interrupt handling.
 *
 * The top half runs as a low-level interrupt thread at nv_intr_pri and calls
 * rm_isr().  The two bottom halves Linux runs from a threaded IRQ and a
 * kthread run here on single-threaded taskqs.  Each bottom half is dispatched
 * with a preallocated taskq entry guarded by a pending flag, so dispatch from
 * interrupt context cannot fail and back-to-back interrupts coalesce.
 */

#include "nv-illumos.h"
#include "nv-reg.h"

#include <sys/taskq_impl.h>
#include <sys/disp.h>

static krwlock_t nv_uvm_top_half_lock;

void
nv_intr_init(void)
{
    rw_init(&nv_uvm_top_half_lock, NULL, RW_DRIVER, DDI_INTR_PRI(nv_intr_pri));
}

void
nv_intr_fini(void)
{
    rw_destroy(&nv_uvm_top_half_lock);
}

static NvBool
nv_uvm_top_half(nv_state_t *nv)
{
    NV_STATUS status;

    rw_enter(&nv_uvm_top_half_lock, RW_READER);
    status = nv_uvm_event_interrupt(nv_get_cached_uuid(nv));
    rw_exit(&nv_uvm_top_half_lock);

    return (status == NV_OK);
}

struct nv_intr_priv_s {
    taskq_t        *bh_tq;
    taskq_t        *bh_unlocked_tq;
    taskq_ent_t     bh_ent;
    taskq_ent_t     bh_unlocked_ent;
};

static void
nvidia_isr_bh(void *arg)
{
    nv_illumos_state_t *nvis = arg;
    nv_state_t *nv = NV_STATE_PTR(nvis);

    atomic_swap_32(&nvis->bh_pending, 0);

    if (NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv))
    {
        nv_printf(NV_DBG_INFO, "NVRM: GPU is lost, skipping ISR bottom half\n");
        return;
    }

    rm_isr_bh(nvis->sp[NV_DEV_STACK_ISR_BH], nv);
}

static void
nvidia_isr_bh_unlocked(void *arg)
{
    nv_illumos_state_t *nvis = arg;
    nv_state_t *nv = NV_STATE_PTR(nvis);

    atomic_swap_32(&nvis->bh_unlocked_pending, 0);

    if (NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv))
    {
        nv_printf(NV_DBG_INFO,
            "NVRM: GPU is lost, skipping unlocked ISR bottom half\n");
        return;
    }

    rm_isr_bh_unlocked(nvis->sp[NV_DEV_STACK_ISR_BH_UNLOCKED], nv);
}

static void
nv_schedule_bh(nv_illumos_state_t *nvis)
{
    struct nv_intr_priv_s *ip = nvis->intr_priv;

    if (atomic_cas_32(&nvis->bh_pending, 0, 1) == 0)
        taskq_dispatch_ent(ip->bh_tq, nvidia_isr_bh, nvis, TQ_NOSLEEP,
            &ip->bh_ent);
}

static void
nv_schedule_bh_unlocked(nv_illumos_state_t *nvis)
{
    struct nv_intr_priv_s *ip = nvis->intr_priv;

    if (atomic_cas_32(&nvis->bh_unlocked_pending, 0, 1) == 0)
        taskq_dispatch_ent(ip->bh_unlocked_tq, nvidia_isr_bh_unlocked, nvis,
            TQ_NOSLEEP, &ip->bh_unlocked_ent);
}

static uint_t
nvidia_isr(caddr_t arg1, caddr_t arg2)
{
    nv_illumos_state_t *nvis = (nv_illumos_state_t *)arg1;
    nv_state_t *nv = NV_STATE_PTR(nvis);
    nvidia_stack_t *sp = nvis->sp[NV_DEV_STACK_ISR];
    NvU32 need_to_run_bottom_half = 0;
    NvU32 rm_serviceable_fault_cnt = 0;
    NvBool rm_handled, uvm_handled;

    if (sp == NULL || !nvis->intr_enabled)
        return (DDI_INTR_UNCLAIMED);

    /* RM's ISR is not reentrant across MSI-X vectors. */
    mutex_enter(&nvis->isr_lock);

    rm_gpu_handle_mmu_faults(sp, nv, &rm_serviceable_fault_cnt);

    uvm_handled = nv_uvm_top_half(nv);

    rm_handled = rm_isr(sp, nv, &need_to_run_bottom_half);

    mutex_exit(&nvis->isr_lock);

    if (need_to_run_bottom_half)
        nv_schedule_bh(nvis);
    else if (rm_serviceable_fault_cnt != 0)
        nv_schedule_bh_unlocked(nvis);

    /* MSI and MSI-X vectors are never shared. */
    if (nvis->intr_type != DDI_INTR_TYPE_FIXED)
        return (DDI_INTR_CLAIMED);

    return (rm_handled || uvm_handled || rm_serviceable_fault_cnt != 0 ||
            need_to_run_bottom_half) ? DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED;
}

static void
nv_intr_free(nv_illumos_state_t *nvis, int nalloc)
{
    int i;

    for (i = 0; i < nalloc; i++)
        (void) ddi_intr_free(nvis->intr_htable[i]);

    kmem_free(nvis->intr_htable, nvis->intr_htable_size);
    nvis->intr_htable = NULL;
    nvis->intr_htable_size = 0;
    nvis->intr_count = 0;
    nvis->intr_pri = 0;
}

static int
nv_intr_alloc(nv_illumos_state_t *nvis, int type, int want)
{
    dev_info_t *dip = nvis->dip;
    int count, avail, actual = 0;
    int i;

    if (ddi_intr_get_nintrs(dip, type, &count) != DDI_SUCCESS || count < 1)
        return (DDI_FAILURE);
    if (ddi_intr_get_navail(dip, type, &avail) != DDI_SUCCESS || avail < 1)
        return (DDI_FAILURE);

    count = MIN(MIN(count, avail), want);

    nvis->intr_htable_size = count * sizeof (ddi_intr_handle_t);
    nvis->intr_htable = kmem_zalloc(nvis->intr_htable_size, KM_SLEEP);

    if (ddi_intr_alloc(dip, nvis->intr_htable, type, 0, count, &actual,
            DDI_INTR_ALLOC_NORMAL) != DDI_SUCCESS || actual < 1)
    {
        nv_intr_free(nvis, 0);
        return (DDI_FAILURE);
    }

    /*
     * RM locks used by the handler are initialized at nv_intr_pri, so the
     * vectors must run at that priority, below LOCK_LEVEL.
     */
    nvis->intr_pri = 0;
    for (i = 0; i < actual; i++)
    {
        uint_t pri;

        if (ddi_intr_get_pri(nvis->intr_htable[i], &pri) != DDI_SUCCESS)
        {
            nv_intr_free(nvis, actual);
            return (DDI_FAILURE);
        }

        if (pri != nv_intr_pri &&
            ddi_intr_set_pri(nvis->intr_htable[i], nv_intr_pri) == DDI_SUCCESS)
            pri = nv_intr_pri;

        if (pri >= ddi_intr_get_hilevel_pri())
        {
            dev_err(dip, CE_WARN, "interrupt priority %u is high-level", pri);
            nv_intr_free(nvis, actual);
            return (DDI_FAILURE);
        }
        nvis->intr_pri = MAX(nvis->intr_pri, pri);
    }

    for (i = 0; i < actual; i++)
    {
        if (ddi_intr_add_handler(nvis->intr_htable[i], nvidia_isr,
                (caddr_t)nvis, (caddr_t)(uintptr_t)i) != DDI_SUCCESS)
        {
            while (i-- > 0)
                (void) ddi_intr_remove_handler(nvis->intr_htable[i]);
            nv_intr_free(nvis, actual);
            return (DDI_FAILURE);
        }
    }

    (void) ddi_intr_get_cap(nvis->intr_htable[0], &nvis->intr_cap);
    nvis->intr_type = type;
    nvis->intr_count = actual;

    return (DDI_SUCCESS);
}

int
nv_intr_setup(nv_illumos_state_t *nvis)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);
    nvidia_stack_t *sp = nvis->sp[NV_DEV_STACK_ISR];
    struct nv_intr_priv_s *ip;
    NvU32 msi_config = 1;
    int types = 0;
    char name[32];
    int rc = DDI_FAILURE;
    int i;

    (void) rm_read_registry_dword(sp, nv, NV_REG_ENABLE_MSI, &msi_config);

    if (ddi_intr_get_supported_types(nvis->dip, &types) != DDI_SUCCESS)
        return (EIO);

    nvis->intr_pri = 0;

    if (msi_config == 1 && (types & DDI_INTR_TYPE_MSIX) &&
        rm_is_msix_allowed(sp, nv))
    {
        rc = nv_intr_alloc(nvis, DDI_INTR_TYPE_MSIX, NV_RM_MAX_MSIX_LINES);
        if (rc == DDI_SUCCESS)
            nv->flags |= NV_FLAG_USES_MSIX;
    }

    if (rc != DDI_SUCCESS && msi_config == 1 && (types & DDI_INTR_TYPE_MSI))
    {
        rc = nv_intr_alloc(nvis, DDI_INTR_TYPE_MSI, 1);
        if (rc == DDI_SUCCESS)
            nv->flags |= NV_FLAG_USES_MSI;
    }

    if (rc != DDI_SUCCESS && (types & DDI_INTR_TYPE_FIXED))
        rc = nv_intr_alloc(nvis, DDI_INTR_TYPE_FIXED, 1);

    if (rc != DDI_SUCCESS)
        return (EIO);

    mutex_init(&nvis->isr_lock, NULL, MUTEX_DRIVER, DDI_INTR_PRI(nvis->intr_pri));

    ip = kmem_zalloc(sizeof (*ip), KM_SLEEP);
    (void) snprintf(name, sizeof (name), "nvidia_bh_%d", nvis->instance);
    ip->bh_tq = taskq_create(name, 1, maxclsyspri, 1, 1, TASKQ_PREPOPULATE);
    (void) snprintf(name, sizeof (name), "nvidia_bh_unlocked_%d", nvis->instance);
    ip->bh_unlocked_tq = taskq_create(name, 1, maxclsyspri, 1, 1,
        TASKQ_PREPOPULATE);
    nvis->intr_priv = ip;
    nvis->bh_pending = 0;
    nvis->bh_unlocked_pending = 0;

    nvis->intr_enabled = NV_TRUE;
    membar_producer();

    if (nvis->intr_cap & DDI_INTR_FLAG_BLOCK)
    {
        (void) ddi_intr_block_enable(nvis->intr_htable, nvis->intr_count);
    }
    else
    {
        for (i = 0; i < nvis->intr_count; i++)
            (void) ddi_intr_enable(nvis->intr_htable[i]);
    }

    return (0);
}

void
nv_intr_teardown(nv_illumos_state_t *nvis)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);
    struct nv_intr_priv_s *ip = nvis->intr_priv;
    int i;

    if (nvis->intr_htable == NULL)
        return;

    if (nvis->intr_cap & DDI_INTR_FLAG_BLOCK)
    {
        (void) ddi_intr_block_disable(nvis->intr_htable, nvis->intr_count);
    }
    else
    {
        for (i = 0; i < nvis->intr_count; i++)
            (void) ddi_intr_disable(nvis->intr_htable[i]);
    }

    nvis->intr_enabled = NV_FALSE;

    for (i = 0; i < nvis->intr_count; i++)
        (void) ddi_intr_remove_handler(nvis->intr_htable[i]);
    nv_intr_free(nvis, nvis->intr_count);

    /* Handlers are gone; finish any bottom half they queued. */
    if (ip != NULL)
    {
        taskq_destroy(ip->bh_tq);
        taskq_destroy(ip->bh_unlocked_tq);
        kmem_free(ip, sizeof (*ip));
        nvis->intr_priv = NULL;
    }

    mutex_destroy(&nvis->isr_lock);

    nv->flags &= ~(NV_FLAG_USES_MSI | NV_FLAG_USES_MSIX);
}

void NV_API_CALL nv_schedule_uvm_isr(nv_state_t *nv)
{
    (void) nv_uvm_top_half(nv);
}

/* No SoC interrupts exist on PCI GPUs. */
void NV_API_CALL nv_control_soc_irqs(nv_state_t *nv, NvBool bEnable)
{
}

NV_STATUS NV_API_CALL nv_get_current_irq_priv_data(nv_state_t *nv, NvU32 *priv_data)
{
    return NV_ERR_NOT_SUPPORTED;
}

nv_soc_irq_type_t NV_API_CALL nv_get_current_irq_type(nv_state_t *nv)
{
    return NV_SOC_IRQ_INVALID_TYPE;
}

/*
 * Wait for UVM top halves still running with callbacks that were just
 * unregistered.  Detaching GPUs are covered too, which a walk of the device
 * list would miss.
 */
void
nv_drain_isr_top_halves(void)
{
    rw_enter(&nv_uvm_top_half_lock, RW_WRITER);
    rw_exit(&nv_uvm_top_half_lock);
}

NV_STATUS NV_API_CALL nv_schedule_uvm_drain_p2p(NvU8 *pUuid)
{
    return nv_uvm_drain_P2P(pUuid);
}

void NV_API_CALL nv_schedule_uvm_resume_p2p(NvU8 *pUuid)
{
    (void) nv_uvm_resume_P2P(pUuid);
}
