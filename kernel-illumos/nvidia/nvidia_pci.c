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
 * PCI configuration space access (os_pci_*) and BAR discovery.
 *
 * RM keeps the handles from os_pci_init_handle() for the life of the module
 * and never releases them, so handles are cached per function and freed at
 * unload.  illumos x86 exposes a single PCI segment, reported as domain 0.
 */

#include "nv-illumos.h"

#include <sys/pci_cap.h>
#include <sys/pcie.h>

#define NV_PCIE_CFG_MAX_OFFSET      0x1000

typedef struct nv_pci_handle_s {
    list_node_t         link;
    dev_info_t         *dip;
    ddi_acc_handle_t    cfg;
    NvU8                bus;
    NvU8                slot;
    NvU8                func;
    NvU16               vendor;
    NvU16               device;
} nv_pci_handle_t;

static list_t   nv_pci_handles;
static kmutex_t nv_pci_handles_lock;
static NvBool   nv_pci_handles_ready;

void
nv_pci_handles_init(void)
{
    mutex_init(&nv_pci_handles_lock, NULL, MUTEX_DRIVER, NULL);
    list_create(&nv_pci_handles, sizeof (nv_pci_handle_t),
        offsetof(nv_pci_handle_t, link));
    nv_pci_handles_ready = NV_TRUE;
}

void
nv_pci_handles_fini(void)
{
    nv_pci_handle_t *h;

    if (!nv_pci_handles_ready)
        return;

    while ((h = list_remove_head(&nv_pci_handles)) != NULL)
    {
        pci_config_teardown(&h->cfg);
        ndi_rele_devi(h->dip);
        kmem_free(h, sizeof (*h));
    }

    list_destroy(&nv_pci_handles);
    mutex_destroy(&nv_pci_handles_lock);
    nv_pci_handles_ready = NV_FALSE;
}

/* Decode bus/device/function from the first "reg" entry of a PCI node. */
int
nv_pci_dip_bdf(dev_info_t *dip, NvU8 *bus, NvU8 *slot, NvU8 *func)
{
    pci_regspec_t *regs;
    uint_t nelem;

    if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
            "reg", (int **)&regs, &nelem) != DDI_PROP_SUCCESS)
        return (DDI_FAILURE);

    if (nelem < sizeof (pci_regspec_t) / sizeof (int))
    {
        ddi_prop_free(regs);
        return (DDI_FAILURE);
    }

    *bus = PCI_REG_BUS_G(regs[0].pci_phys_hi);
    *slot = PCI_REG_DEV_G(regs[0].pci_phys_hi);
    *func = PCI_REG_FUNC_G(regs[0].pci_phys_hi);
    ddi_prop_free(regs);

    return (DDI_SUCCESS);
}

static NvBool
nv_pci_is_pci_function(dev_info_t *dip)
{
    return (ddi_prop_exists(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
                "vendor-id") &&
            ddi_prop_exists(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
                "class-code"));
}

typedef struct {
    NvU8        bus;
    NvU8        slot;
    NvU8        func;
    dev_info_t *dip;
} nv_pci_find_t;

static int
nv_pci_find_bdf(dev_info_t *dip, void *arg)
{
    nv_pci_find_t *f = arg;
    NvU8 bus, slot, func;

    if (!nv_pci_is_pci_function(dip))
        return (DDI_WALK_CONTINUE);

    if (nv_pci_dip_bdf(dip, &bus, &slot, &func) != DDI_SUCCESS)
        return (DDI_WALK_CONTINUE);

    if (bus == f->bus && slot == f->slot && func == f->func)
    {
        ndi_hold_devi(dip);
        f->dip = dip;
        return (DDI_WALK_TERMINATE);
    }

    return (DDI_WALK_CONTINUE);
}

static nv_pci_handle_t *
nv_pci_handle_create(dev_info_t *dip, NvU8 bus, NvU8 slot, NvU8 func)
{
    nv_pci_handle_t *h;

    h = kmem_zalloc(sizeof (*h), KM_SLEEP);
    if (pci_config_setup(dip, &h->cfg) != DDI_SUCCESS)
    {
        kmem_free(h, sizeof (*h));
        return (NULL);
    }

    h->dip = dip;
    h->bus = bus;
    h->slot = slot;
    h->func = func;
    h->vendor = pci_config_get16(h->cfg, PCI_CONF_VENID);
    h->device = pci_config_get16(h->cfg, PCI_CONF_DEVID);

    return (h);
}

/*
 * Returns the cached handle for a function, creating it on first use.  The
 * caller of the "dip" variant already holds the node.
 */
void *
nv_pci_handle_for_dip(dev_info_t *dip)
{
    nv_pci_handle_t *h;
    NvU8 bus, slot, func;

    if (nv_pci_dip_bdf(dip, &bus, &slot, &func) != DDI_SUCCESS)
        return (NULL);

    mutex_enter(&nv_pci_handles_lock);
    for (h = list_head(&nv_pci_handles); h != NULL;
         h = list_next(&nv_pci_handles, h))
    {
        if (h->dip == dip)
        {
            mutex_exit(&nv_pci_handles_lock);
            return (h);
        }
    }
    mutex_exit(&nv_pci_handles_lock);

    ndi_hold_devi(dip);
    h = nv_pci_handle_create(dip, bus, slot, func);
    if (h == NULL)
    {
        ndi_rele_devi(dip);
        return (NULL);
    }

    mutex_enter(&nv_pci_handles_lock);
    list_insert_tail(&nv_pci_handles, h);
    mutex_exit(&nv_pci_handles_lock);

    return (h);
}

void* NV_API_CALL os_pci_init_handle(
    NvU32 domain,
    NvU8  bus,
    NvU8  slot,
    NvU8  function,
    NvU16 *vendor,
    NvU16 *device
)
{
    nv_pci_handle_t *h;
    nv_pci_find_t f;

    if (!nv_may_sleep() || domain != 0)
        return NULL;

    mutex_enter(&nv_pci_handles_lock);
    for (h = list_head(&nv_pci_handles); h != NULL;
         h = list_next(&nv_pci_handles, h))
    {
        if (h->bus == bus && h->slot == slot && h->func == function)
            break;
    }
    mutex_exit(&nv_pci_handles_lock);

    if (h == NULL)
    {
        f.bus = bus;
        f.slot = slot;
        f.func = function;
        f.dip = NULL;

        ndi_devi_enter(ddi_root_node());
        ddi_walk_devs(ddi_get_child(ddi_root_node()), nv_pci_find_bdf, &f);
        ndi_devi_exit(ddi_root_node());

        if (f.dip == NULL)
            return NULL;

        h = nv_pci_handle_for_dip(f.dip);
        ndi_rele_devi(f.dip);
        if (h == NULL)
            return NULL;
    }

    if (vendor != NULL)
        *vendor = h->vendor;
    if (device != NULL)
        *device = h->device;

    return h;
}

NV_STATUS NV_API_CALL os_pci_read_byte(void *handle, NvU32 offset, NvU8 *pReturnValue)
{
    nv_pci_handle_t *h = handle;

    if (h == NULL || offset >= NV_PCIE_CFG_MAX_OFFSET)
    {
        *pReturnValue = 0xff;
        return NV_ERR_NOT_SUPPORTED;
    }

    *pReturnValue = pci_config_get8(h->cfg, offset);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_read_word(void *handle, NvU32 offset, NvU16 *pReturnValue)
{
    nv_pci_handle_t *h = handle;

    if (h == NULL || offset >= NV_PCIE_CFG_MAX_OFFSET)
    {
        *pReturnValue = 0xffff;
        return NV_ERR_NOT_SUPPORTED;
    }

    *pReturnValue = pci_config_get16(h->cfg, offset);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_read_dword(void *handle, NvU32 offset, NvU32 *pReturnValue)
{
    nv_pci_handle_t *h = handle;

    if (h == NULL || offset >= NV_PCIE_CFG_MAX_OFFSET)
    {
        *pReturnValue = 0xffffffff;
        return NV_ERR_NOT_SUPPORTED;
    }

    *pReturnValue = pci_config_get32(h->cfg, offset);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_write_byte(void *handle, NvU32 offset, NvU8 value)
{
    nv_pci_handle_t *h = handle;

    if (h == NULL || offset >= NV_PCIE_CFG_MAX_OFFSET)
        return NV_ERR_NOT_SUPPORTED;

    pci_config_put8(h->cfg, offset, value);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_write_word(void *handle, NvU32 offset, NvU16 value)
{
    nv_pci_handle_t *h = handle;

    if (h == NULL || offset >= NV_PCIE_CFG_MAX_OFFSET)
        return NV_ERR_NOT_SUPPORTED;

    pci_config_put16(h->cfg, offset, value);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_write_dword(void *handle, NvU32 offset, NvU32 value)
{
    nv_pci_handle_t *h = handle;

    if (h == NULL || offset >= NV_PCIE_CFG_MAX_OFFSET)
        return NV_ERR_NOT_SUPPORTED;

    pci_config_put32(h->cfg, offset, value);
    return NV_OK;
}

/*
 * Removing a device from the tree must happen outside the caller's context:
 * RM asks for it from paths that hold the device open.
 */
static void
nv_pci_remove_task(void *arg)
{
    dev_info_t *dip = arg;
    dev_info_t *pdip = ddi_get_parent(dip);
    int rv;

    ndi_devi_enter(pdip);
    rv = ndi_devi_offline(dip, NDI_DEVI_REMOVE);
    ndi_devi_exit(pdip);

    if (rv != NDI_SUCCESS)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: failed to remove %s%d from the device tree (%d)\n",
            ddi_driver_name(dip), ddi_get_instance(dip), rv);
    }

    ndi_rele_devi(dip);
}

NvBool NV_API_CALL os_pci_remove_supported(void)
{
    return NV_TRUE;
}

void NV_API_CALL os_pci_remove(void *handle)
{
    nv_pci_handle_t *h = handle;

    if (h == NULL)
        return;

    ndi_hold_devi(h->dip);
    if (taskq_dispatch(nv_global_queue.tq, nv_pci_remove_task, h->dip,
                       TQ_SLEEP) == TASKQID_INVALID)
    {
        ndi_rele_devi(h->dip);
    }
}

static int
nv_pci_find_pcie_cap(ddi_acc_handle_t cfg, uint16_t *off)
{
    return (PCI_CAP_LOCATE(cfg, PCI_CAP_ID_PCI_E, off) == DDI_SUCCESS) ?
        0 : ENOENT;
}

/*
 * Enable AtomicOp requests only when every switch on the path routes
 * AtomicOps and the root port can complete the requested operand size, as
 * pci_enable_atomic_ops_to_root() does on Linux.
 */
NV_STATUS NV_API_CALL os_enable_pci_req_atomics(void *handle,
    enum os_pci_req_atomics_type type)
{
    nv_pci_handle_t *h = handle;
    dev_info_t *pdip;
    uint32_t comp_cap;
    uint16_t cap, devctl2;

    if (h == NULL)
        return NV_ERR_NOT_SUPPORTED;

    switch (type)
    {
        case OS_INTF_PCIE_REQ_ATOMICS_32BIT:
            comp_cap = PCIE_DEVCAP2_32_ATOMICOP_COMPL;
            break;
        case OS_INTF_PCIE_REQ_ATOMICS_64BIT:
            comp_cap = PCIE_DEVCAP2_64_ATOMICOP_COMPL;
            break;
        case OS_INTF_PCIE_REQ_ATOMICS_128BIT:
            comp_cap = PCIE_DEVCAP2_128_CAS_COMPL;
            break;
        default:
            return NV_ERR_NOT_SUPPORTED;
    }

    if (nv_pci_find_pcie_cap(h->cfg, &cap) != 0)
        return NV_ERR_NOT_SUPPORTED;

    for (pdip = ddi_get_parent(h->dip); pdip != NULL;
         pdip = ddi_get_parent(pdip))
    {
        ddi_acc_handle_t pcfg;
        uint16_t pcap, pflags, pctl2;
        uint32_t pcap2;
        uint16_t port_type;

        if (!nv_pci_is_pci_function(pdip))
            return NV_ERR_NOT_SUPPORTED;

        if (pci_config_setup(pdip, &pcfg) != DDI_SUCCESS)
            return NV_ERR_NOT_SUPPORTED;

        if (nv_pci_find_pcie_cap(pcfg, &pcap) != 0)
        {
            pci_config_teardown(&pcfg);
            return NV_ERR_NOT_SUPPORTED;
        }

        pflags = pci_config_get16(pcfg, pcap + PCIE_PCIECAP);
        pcap2 = pci_config_get32(pcfg, pcap + PCIE_DEVCAP2);
        pctl2 = pci_config_get16(pcfg, pcap + PCIE_DEVCTL2);
        pci_config_teardown(&pcfg);

        port_type = pflags & PCIE_PCIECAP_DEV_TYPE_MASK;

        if (port_type == PCIE_PCIECAP_DEV_TYPE_ROOT)
        {
            if ((pcap2 & comp_cap) != comp_cap)
                return NV_ERR_NOT_SUPPORTED;
            break;
        }

        if (port_type == PCIE_PCIECAP_DEV_TYPE_DOWN ||
            port_type == PCIE_PCIECAP_DEV_TYPE_UP)
        {
            if ((pcap2 & PCIE_DEVCAP2_ATOMICOP_ROUTING) == 0)
                return NV_ERR_NOT_SUPPORTED;
            if (port_type == PCIE_PCIECAP_DEV_TYPE_UP &&
                (pctl2 & PCIE_DEVCTL2_ATOMICOP_EGRS_BLK) != 0)
                return NV_ERR_NOT_SUPPORTED;
            continue;
        }

        return NV_ERR_NOT_SUPPORTED;
    }

    if (pdip == NULL)
        return NV_ERR_NOT_SUPPORTED;

    devctl2 = pci_config_get16(h->cfg, cap + PCIE_DEVCTL2);
    pci_config_put16(h->cfg, cap + PCIE_DEVCTL2,
        devctl2 | PCIE_DEVCTL2_ATOMICOP_REQ_EN);

    /* GPUs without requester AtomicOps keep the bit clear. */
    devctl2 = pci_config_get16(h->cfg, cap + PCIE_DEVCTL2);
    return (devctl2 & PCIE_DEVCTL2_ATOMICOP_REQ_EN) ?
        NV_OK : NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL os_pci_trigger_flr(void *handle)
{
    nv_pci_handle_t *h = handle;
    uint16_t cap, devctl;
    uint32_t devcap;

    if (h == NULL)
        return;

    nv_printf(NV_DBG_INFO, "NVRM: %s() Start to trigger FLR\n", __func__);

    if (nv_pci_find_pcie_cap(h->cfg, &cap) != 0)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s() PCI FLR not supported\n", __func__);
        return;
    }

    devcap = pci_config_get32(h->cfg, cap + PCIE_DEVCAP);
    if ((devcap & PCIE_DEVCAP_FLR) == 0)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: %s() PCI FLR not supported\n", __func__);
        return;
    }

    if (pci_save_config_regs(h->dip) != DDI_SUCCESS)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: %s() PCI save state failed, Skip FLR\n", __func__);
        return;
    }

    devctl = pci_config_get16(h->cfg, cap + PCIE_DEVCTL);
    pci_config_put16(h->cfg, cap + PCIE_DEVCTL, devctl | PCIE_DEVCTL_INITIATE_FLR);

    /* PCIe base spec: wait 100ms after initiating FLR. */
    delay(drv_usectohz(100 * 1000));

    (void) pci_restore_config_regs(h->dip);
}

/*
 * Fill nv->bars[] from "assigned-addresses".  BAR0 is the register aperture;
 * the remaining memory BARs, in config-space order, are FB then IMEM, the
 * same order Linux's nv_pci_probe() uses.
 */
int
nv_pci_map_bars(nv_illumos_state_t *nvis)
{
    nv_state_t *nv = NV_STATE_PTR(nvis);
    pci_regspec_t *regs;
    uint_t nelem, nregs, i, j;
    struct {
        NvU64 start;
        NvU64 size;
        NvBool valid;
    } bar[NVRM_PCICFG_NUM_BARS];

    bzero(bar, sizeof (bar));

    if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, nvis->dip, DDI_PROP_DONTPASS,
            "assigned-addresses", (int **)&regs, &nelem) != DDI_PROP_SUCCESS)
        return (ENXIO);

    nregs = nelem / (sizeof (pci_regspec_t) / sizeof (int));
    for (i = 0; i < nregs; i++)
    {
        uint_t space = PCI_REG_ADDR_G(regs[i].pci_phys_hi);
        uint_t off = PCI_REG_REG_G(regs[i].pci_phys_hi);
        uint_t idx;

        if (space != PCI_REG_ADDR_G(PCI_ADDR_MEM32) &&
            space != PCI_REG_ADDR_G(PCI_ADDR_MEM64))
            continue;

        if (off < PCI_CONF_BASE0 || off > PCI_CONF_BASE5 ||
            ((off - PCI_CONF_BASE0) % 4) != 0)
            continue;

        idx = (off - PCI_CONF_BASE0) / 4;
        bar[idx].start = ((NvU64)regs[i].pci_phys_mid << 32) |
            regs[i].pci_phys_low;
        bar[idx].size = ((NvU64)regs[i].pci_size_hi << 32) |
            regs[i].pci_size_low;
        bar[idx].valid = (bar[idx].size != 0);
    }
    ddi_prop_free(regs);

    for (i = 0; i < NVRM_PCICFG_NUM_BARS; i++)
    {
        if (bar[i].valid)
        {
            nv->bars[NV_GPU_BAR_INDEX_REGS].offset = NVRM_PCICFG_BAR_OFFSET(i);
            nv->bars[NV_GPU_BAR_INDEX_REGS].cpu_address = bar[i].start;
            nv->bars[NV_GPU_BAR_INDEX_REGS].size = bar[i].size;
            break;
        }
    }

    if (i == NVRM_PCICFG_NUM_BARS)
        return (ENXIO);

    for (i = i + 1, j = NV_GPU_BAR_INDEX_REGS + 1;
         i < NVRM_PCICFG_NUM_BARS && j < NV_GPU_NUM_BARS; i++)
    {
        if (!bar[i].valid)
            continue;

        /* skip the upper half of a 64-bit BAR */
        if (i > 0 && bar[i - 1].valid &&
            bar[i - 1].start == bar[i].start && bar[i - 1].size == bar[i].size)
            continue;

        nv->bars[j].offset = NVRM_PCICFG_BAR_OFFSET(i);
        nv->bars[j].cpu_address = bar[i].start;
        nv->bars[j].size = bar[i].size;
        j++;
    }

    nv->regs = &nv->bars[NV_GPU_BAR_INDEX_REGS];
    nv->fb = &nv->bars[NV_GPU_BAR_INDEX_FB];

    return (0);
}

/*
 * The GPU is the primary VGA device when it decodes I/O and legacy VGA
 * accesses are routed to it by every bridge above it.
 */
NvBool
nv_pci_is_primary_vga(nv_illumos_state_t *nvis)
{
    dev_info_t *pdip;
    uint16_t cmd;

    cmd = pci_config_get16(nvis->pci_cfg, PCI_CONF_COMM);
    if ((cmd & PCI_COMM_IO) == 0)
        return NV_FALSE;

    for (pdip = ddi_get_parent(nvis->dip); pdip != NULL;
         pdip = ddi_get_parent(pdip))
    {
        ddi_acc_handle_t pcfg;
        uint16_t bctl;

        if (!nv_pci_is_pci_function(pdip))
            break;

        if (pci_config_setup(pdip, &pcfg) != DDI_SUCCESS)
            return NV_FALSE;

        bctl = pci_config_get16(pcfg, PCI_BCNF_BCNTRL);
        pci_config_teardown(&pcfg);

        if ((bctl & PCI_BCNF_BCNTRL_VGA_ENABLE) == 0)
            return NV_FALSE;
    }

    return NV_TRUE;
}
