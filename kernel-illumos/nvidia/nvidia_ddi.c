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
 * Module linkage and device attach/detach.
 *
 * The driver binds to NVIDIA PCI display controllers and to one pseudo node
 * (declared in nvidia.conf) that carries the control device, the capability
 * nodes and the NVLink/NVSwitch control nodes.  RM itself is brought up on
 * the first attach of any node, because driver.conf properties (the NVreg_*
 * registry keys) are only readable through a dev_info node.
 */

#include "nv-illumos.h"
#include "nv-reg.h"

#include <sys/pci_cap.h>

const NvBool nv_is_rm_firmware_supported_os = NV_TRUE;

nv_illumos_state_t   nv_ctl_device;
nv_illumos_state_t  *nv_illumos_devices;
krwlock_t            nv_illumos_devices_lock;
krwlock_t            nv_adapter_state_lock;
dev_info_t          *nv_ctl_dip;
struct os_work_queue nv_global_queue;

/*
 * Every interrupt consumer (RM spinlocks, event queues) is initialized at
 * this priority, and the driver sets its interrupts to it, so the locks are
 * safe to take from the top half.
 */
uint_t nv_intr_pri = 5;

static kmutex_t        nv_global_lock;
static NvBool          nv_rm_initialized;
static nvidia_stack_t *nv_init_sp;
static uint32_t        nv_minor_bitmap;

extern void nv_pci_handles_init(void);
extern void nv_pci_handles_fini(void);
extern void *nv_pci_handle_for_dip(dev_info_t *);
extern NvBool nv_pci_is_primary_vga(nv_illumos_state_t *);
extern NV_STATUS nv_parse_per_device_binary_option_string(nvidia_stack_t *,
    nv_state_t *);
extern void nvidia_modeset_probe(nv_illumos_state_t *);
extern void nvidia_modeset_remove(NvU32 gpu_id);
extern void nv_report_error_init(void);
extern void nv_report_error_fini(void);
extern void nv_modeset_interface_init(void);
extern void nv_modeset_interface_fini(void);


/* Whether any attached GPU does, or does not, support ATS; read by UVM. */
NvBool nv_ats_supported;
NvBool nv_non_ats_device_present;

static NvBool
nv_lock_init_locks(nvidia_stack_t *sp, nv_state_t *nv)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    sema_init(&nvis->ldata_lock, 1, NULL, SEMA_DRIVER, NULL);
    sema_init(&nvis->mmap_lock, 1, NULL, SEMA_DRIVER, NULL);
    mutex_init(&nvis->timer_lock, NULL, MUTEX_DRIVER, NULL);
    nvis->usage_count = 0;

    return rm_init_event_locks(sp, nv);
}

static void
nv_lock_destroy_locks(nvidia_stack_t *sp, nv_state_t *nv)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    rm_destroy_event_locks(sp, nv);
    mutex_destroy(&nvis->timer_lock);
    sema_destroy(&nvis->mmap_lock);
    sema_destroy(&nvis->ldata_lock);
}

static NvBool
nv_init_on_probe(nv_state_t *nv)
{
    switch (NVreg_GpuInitOnProbe)
    {
        case NV_GPU_INIT_ON_PROBE_FORCE_OFF:
            return NV_FALSE;
        case NV_GPU_INIT_ON_PROBE_FORCE_ON:
            return NV_TRUE;
        case NV_GPU_INIT_ON_PROBE_AUTO:
            return !nv->is_tegra_pci_igpu && !os_cc_enabled;
        default:
            NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
                "Init-on-probe: invalid NVreg_GpuInitOnProbe=%u; falling back to disabled\n",
                NVreg_GpuInitOnProbe);
            return NV_FALSE;
    }
}

static int
nv_global_init(dev_info_t *dip)
{
    nvidia_stack_t *sp = NULL;
    NvU32 data;
    int rc;

    ASSERT(MUTEX_HELD(&nv_global_lock));

    if (nv_rm_initialized)
        return (0);

    nv_registry_load(dip);

    if (nv_stack_alloc(&sp) != 0)
    {
        nv_registry_unload();
        return (ENOMEM);
    }

    nv_pci_handles_init();
    nv_dma_init();

    rc = nv_queue_init(&nv_global_queue, "nvidia_queue");
    if (rc != 0)
        goto fail_queue;

    rc = nv_caps_init();
    if (rc != 0)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to initialize capabilities.\n");
        goto fail_caps;
    }

    rc = nv_caps_imex_init();
    if (rc != 0)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to initialize IMEX channels.\n");
        goto fail_imex;
    }

    rc = nvlink_drivers_init();
    if (rc != 0)
        goto fail_nvlink;


    if (!rm_init_rm(sp))
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: rm_init_rm() failed!\n");
        rc = EIO;
        goto fail_rm;
    }

    nv_ctl_device.nv_state.os_state = &nv_ctl_device;
    nv_ctl_device.nv_state.flags = NV_FLAG_CONTROL;
    if (!nv_lock_init_locks(sp, NV_STATE_PTR(&nv_ctl_device)))
    {
        rc = ENOMEM;
        goto fail_locks;
    }

    if (rm_read_registry_dword(sp, NV_STATE_PTR(&nv_ctl_device),
            NV_DMA_REMAP_PEER_MMIO, &data) == NV_OK)
        nv_dma_remap_peer_mmio = data;

    nv_printf(NV_DBG_ERRORS, "NVRM: loading %s\n", pNVRM_ID);

    nv_init_sp = sp;
    nv_rm_initialized = NV_TRUE;
    return (0);

fail_locks:
    rm_shutdown_rm(sp);
fail_rm:
    nvlink_drivers_exit();
fail_nvlink:
    nv_caps_imex_fini();
fail_imex:
    nv_caps_fini();
fail_caps:
    nv_queue_fini(&nv_global_queue);
fail_queue:
    nv_dma_fini();
    nv_pci_handles_fini();
    nv_stack_free(sp);
    nv_registry_unload();
    return (rc);
}

static void
nv_global_fini(void)
{
    nvidia_stack_t *sp = nv_init_sp;

    if (!nv_rm_initialized)
        return;

    (void) os_flush_work_queue(&nv_global_queue, NV_TRUE);
    nv_lock_destroy_locks(sp, NV_STATE_PTR(&nv_ctl_device));
    rm_shutdown_rm(sp);
    nvlink_drivers_exit();
    nv_caps_imex_fini();
    nv_caps_fini();
    nv_queue_fini(&nv_global_queue);
    nv_dma_fini();
    nv_pci_handles_fini();
    nv_stack_free(sp);
    nv_init_sp = NULL;
    nv_registry_unload();
    nv_rm_initialized = NV_FALSE;
}

static NvBool
nv_dip_is_pseudo(dev_info_t *dip)
{
    return (strcmp(ddi_node_name(ddi_get_parent(dip)), "pseudo") == 0);
}

static int
nv_attach_ctl(dev_info_t *dip)
{
    if (nv_ctl_dip != NULL)
    {
        dev_err(dip, CE_WARN, "control node already attached");
        return (DDI_FAILURE);
    }

    if (ddi_create_minor_node(dip, "nvidiactl", S_IFCHR, NV_MINOR_CTL,
            NV_DDI_NT_NVIDIA, 0) != DDI_SUCCESS)
        return (DDI_FAILURE);

    nv_ctl_device.dip = dip;
    nv_ctl_device.instance = ddi_get_instance(dip);
    nv_ctl_dip = dip;

    nv_caps_attach(dip);
    nvlink_ctl_attach(dip);

    ddi_report_dev(dip);
    return (DDI_SUCCESS);
}

static int
nv_detach_ctl(dev_info_t *dip)
{
    if (nv_ctl_device.usage_count != 0 || nv_illumos_devices != NULL)
        return (DDI_FAILURE);

    if (nvlink_ctl_detach(dip) != DDI_SUCCESS)
        return (DDI_FAILURE);

    ddi_remove_minor_node(dip, NULL);
    (void) ddi_prop_remove_all(dip);
    nv_ctl_dip = NULL;
    nv_ctl_device.dip = NULL;
    return (DDI_SUCCESS);
}

static int
nv_assign_minor(nv_illumos_state_t *nvis)
{
    int i;

    for (i = 0; i < NV_MAX_DEVICES; i++)
    {
        if ((nv_minor_bitmap & (1U << i)) == 0)
        {
            nv_minor_bitmap |= (1U << i);
            nvis->minor_num = i;
            return (0);
        }
    }

    return (ENOSPC);
}

static int
nv_attach_gpu(dev_info_t *dip)
{
    nv_illumos_state_t *nvis;
    nv_state_t *nv;
    nvidia_stack_t *sp = NULL;
    ddi_acc_handle_t cfg;
    uint16_t vendor, device, subvendor, subdevice, cmd;
    uint8_t class, subclass;
    NvU8 bus, slot, func;
    char name[32];
    int rc;

    if (pci_config_setup(dip, &cfg) != DDI_SUCCESS)
        return (DDI_FAILURE);

    vendor = pci_config_get16(cfg, PCI_CONF_VENID);
    device = pci_config_get16(cfg, PCI_CONF_DEVID);
    subvendor = pci_config_get16(cfg, PCI_CONF_SUBVENID);
    subdevice = pci_config_get16(cfg, PCI_CONF_SUBSYSID);
    class = pci_config_get8(cfg, PCI_CONF_BASCLASS);
    subclass = pci_config_get8(cfg, PCI_CONF_SUBCLASS);

    if (nv_pci_dip_bdf(dip, &bus, &slot, &func) != DDI_SUCCESS)
    {
        pci_config_teardown(&cfg);
        return (DDI_FAILURE);
    }

    if (nv_stack_alloc(&sp) != 0)
    {
        pci_config_teardown(&cfg);
        return (DDI_FAILURE);
    }

    if (!rm_wait_for_bar_firewall(sp, 0, bus, slot, func, device, subdevice))
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to wait for bar firewall to lower\n");
        goto fail_early;
    }

    if (!rm_is_supported_pci_device(class, subclass, vendor, device,
            subvendor, subdevice, NV_FALSE))
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: ignoring the legacy GPU %04x:%02x:%02x.%x\n",
            0, bus, slot, func);
        goto fail_early;
    }

    nvis = kmem_zalloc(sizeof (*nvis), KM_SLEEP);
    nv = NV_STATE_PTR(nvis);

    nvis->dip = dip;
    nvis->pci_dev.dip = dip;
    nvis->instance = ddi_get_instance(dip);
    nvis->pci_cfg = cfg;
    (void) memcpy(nv->cached_gpu_info.vbios_version, "??.??.??.??.??", 15);

    nv->os_state = nvis;
    nv->pci_info.domain = 0;
    nv->pci_info.bus = bus;
    nv->pci_info.slot = slot;
    nv->pci_info.function = func;
    nv->pci_info.vendor_id = vendor;
    nv->pci_info.device_id = device;
    nv->subsystem_id = subdevice;
    nv->subsystem_vendor = subvendor;
    nv->interrupt_line = pci_config_get8(cfg, PCI_CONF_ILINE);
    nv->cpu_numa_node_id = -1;
    nv->ats_support = NV_FALSE;

    if (nv_pci_map_bars(nvis) != 0)
    {
        dev_err(dip, CE_WARN, "no usable BARs assigned");
        goto fail_free;
    }

    nv->handle = nv_pci_handle_for_dip(dip);
    if (nv->handle == NULL)
        goto fail_free;

    nvis->dma_dev.dip = dip;
    nvis->dma_dev.nvis = nvis;
    nvis->dma_dev.addressable_range.start = 0;
    nvis->dma_dev.addressable_range.limit = 0xffffffffULL;
    nvis->niso_dma_dev = nvis->dma_dev;
    nv->dma_dev = &nvis->dma_dev;
    nv->niso_dma_dev = &nvis->niso_dma_dev;
    nv->dma_mask = 0xffffffffULL;

    nvis->dma_remap = nv_dma_detect_remap(dip);
    if (nvis->dma_remap)
        dev_err(dip, CE_CONT, "?DMA is remapped by an IOMMU\n");

    /* Enable memory decode and bus mastering. */
    cmd = pci_config_get16(cfg, PCI_CONF_COMM);
    pci_config_put16(cfg, PCI_CONF_COMM, cmd | PCI_COMM_MAE | PCI_COMM_ME);

    if (!nv_lock_init_locks(sp, nv))
        goto fail_free;

    if (rm_is_supported_device(sp, nv) != NV_OK)
        goto fail_locks;

    if (!rm_init_private_state(sp, nv))
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "rm_init_private_state() failed!\n");
        goto fail_locks;
    }

    if (nv->regs->cpu_address == 0 || nv->regs->size == 0 ||
        nv->fb->cpu_address == 0 || nv->fb->size == 0)
    {
        NV_DEV_PRINTF(NV_DBG_ERRORS, nv,
            "BAR0 or BAR1 has no address assigned; check BIOS PCI resource settings\n");
        goto fail_private;
    }

    list_create(&nvis->open_files, sizeof (nv_illumos_file_private_t),
        offsetof(nv_illumos_file_private_t, open_link));
    nvis->all_mappings_revoked = NV_TRUE;
    nvis->safe_to_mmap = NV_TRUE;
    nvis->gpu_wakeup_callback_needed = NV_TRUE;

    nv->primary_vga = nv_pci_is_primary_vga(nvis);

    rm_init_dynamic_power_management(sp, nv, NV_FALSE);

    rm_get_gpu_uuid_raw(sp, nv);

    (void) nv_parse_per_device_option_string(sp, nv);
    if (nv_parse_per_device_binary_option_string(sp, nv) != NV_OK)
    {
        NV_DEV_PRINTF(NV_DBG_WARNINGS, nv,
            "Per-device binary registry key parsing failed during attach\n");
    }

    rm_set_rm_firmware_requested(sp, nv);

    /* Exclude the GPU if its UUID is in NVreg_ExcludedGpus. */
    {
        char *uuid = rm_get_gpu_uuid(sp, nv);

        if (uuid != NULL)
        {
            if (nv_is_uuid_in_gpu_exclusion_list(uuid) &&
                rm_exclude_adapter(sp, nv) == NV_OK)
            {
                nv->flags |= NV_FLAG_EXCLUDE;
                NV_DEV_PRINTF(NV_DBG_INFO, nv, "Excluded %s\n", uuid);
            }
            os_free_mem(uuid);
        }
    }

    rw_enter(&nv_illumos_devices_lock, RW_WRITER);
    rc = nv_assign_minor(nvis);
    rw_exit(&nv_illumos_devices_lock);
    if (rc != 0)
    {
        dev_err(dip, CE_WARN, "too many NVIDIA GPUs");
        goto fail_pm;
    }

    (void) snprintf(name, sizeof (name), "nvidia%u", nvis->minor_num);
    if (ddi_create_minor_node(dip, name, S_IFCHR, nvis->minor_num,
            NV_DDI_NT_NVIDIA, 0) != DDI_SUCCESS)
        goto fail_minor;

    ddi_set_driver_private(dip, nvis);

    if (nv_init_on_probe(nv) && !(nv->flags & NV_FLAG_EXCLUDE))
    {
        sema_p(&nvis->ldata_lock);
        rc = nv_start_device(nv, sp);
        sema_v(&nvis->ldata_lock);
        if (rc != 0)
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: GPU %04x:%02x:%02x.%x init failed during attach (%d).\n",
                0, bus, slot, func, rc);
        }
    }

    rw_enter(&nv_illumos_devices_lock, RW_WRITER);
    nvis->next = nv_illumos_devices;
    nv_illumos_devices = nvis;
    rw_exit(&nv_illumos_devices_lock);

    rm_notify_gpu_addition(sp, nv);

    nvidia_modeset_probe(nvis);

    rm_enable_dynamic_power_management(sp, nv);

    if (nv->ats_support)
        WRITE_ONCE(nv_ats_supported, NV_TRUE);
    else
        WRITE_ONCE(nv_non_ats_device_present, NV_TRUE);

    nv_stack_free(sp);
    ddi_report_dev(dip);
    return (DDI_SUCCESS);

fail_minor:
    rw_enter(&nv_illumos_devices_lock, RW_WRITER);
    nv_minor_bitmap &= ~(1U << nvis->minor_num);
    rw_exit(&nv_illumos_devices_lock);
fail_pm:
    rm_cleanup_dynamic_power_management(sp, nv);
    list_destroy(&nvis->open_files);
fail_private:
    rm_free_private_state(sp, nv);
fail_locks:
    nv_lock_destroy_locks(sp, nv);
fail_free:
    kmem_free(nvis, sizeof (*nvis));
fail_early:
    pci_config_teardown(&cfg);
    nv_stack_free(sp);
    return (DDI_FAILURE);
}

static int
nv_detach_gpu(dev_info_t *dip)
{
    nv_illumos_state_t *nvis = ddi_get_driver_private(dip);
    nv_illumos_state_t **pp;
    nv_state_t *nv;
    nvidia_stack_t *sp = NULL;

    if (nvis == NULL)
        return (DDI_FAILURE);

    nv = NV_STATE_PTR(nvis);

    if (nv_stack_alloc(&sp) != 0)
        return (DDI_FAILURE);

    /* NVKMS drops its reference on the GPU here; it must not hold locks. */
    nvidia_modeset_remove(nv->gpu_id);

    rw_enter(&nv_illumos_devices_lock, RW_WRITER);
    sema_p(&nvis->ldata_lock);
    if (nvis->usage_count != 0)
    {
        sema_v(&nvis->ldata_lock);
        rw_exit(&nv_illumos_devices_lock);
        nv_stack_free(sp);
        return (DDI_FAILURE);
    }

    for (pp = &nv_illumos_devices; *pp != NULL; pp = &(*pp)->next)
    {
        if (*pp == nvis)
        {
            *pp = nvis->next;
            break;
        }
    }
    nv_minor_bitmap &= ~(1U << nvis->minor_num);
    rw_exit(&nv_illumos_devices_lock);

    rm_notify_gpu_removal(sp, nv);
    rm_check_for_gpu_surprise_removal(sp, nv);

    nv_stop_device(nv, sp);

    rm_cleanup_dynamic_power_management(sp, nv);

    ddi_remove_minor_node(dip, NULL);

    nv->removed = NV_TRUE;

    if (nv->flags & NV_FLAG_PERSISTENT_SW_STATE)
    {
        rm_disable_gpu_state_persistence(sp, nv);
        nv_shutdown_adapter(sp, nv);
        nv_dev_free_stacks(nvis);
    }

    sema_v(&nvis->ldata_lock);

    /* Wait out status queries still using nv from nv_get_adapter_state(). */
    rw_enter(&nv_adapter_state_lock, RW_WRITER);
    rw_exit(&nv_adapter_state_lock);

    rm_i2c_remove_adapters(sp, nv);
    rm_free_private_state(sp, nv);

    nv_lock_destroy_locks(sp, nv);
    list_destroy(&nvis->open_files);

    ddi_set_driver_private(dip, NULL);
    pci_config_teardown(&nvis->pci_cfg);
    kmem_free(nvis, sizeof (*nvis));

    nv_stack_free(sp);
    return (DDI_SUCCESS);
}

static int
nvidia_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
    int rc;

    switch (cmd)
    {
        case DDI_ATTACH:
            break;
        case DDI_RESUME:
            if (nv_dip_is_pseudo(dip))
                return (DDI_SUCCESS);
            if (nvswitch_dip_is_nvswitch(dip))
                return nvswitch_resume(dip);
            return nv_resume_gpu(ddi_get_driver_private(dip));
        default:
            return (DDI_FAILURE);
    }

    mutex_enter(&nv_global_lock);
    rc = nv_global_init(dip);
    if (rc != 0)
    {
        mutex_exit(&nv_global_lock);
        return (DDI_FAILURE);
    }

    if (nv_dip_is_pseudo(dip))
        rc = nv_attach_ctl(dip);
    else if (nvswitch_dip_is_nvswitch(dip))
        rc = nvswitch_attach(dip);
    else
        rc = nv_attach_gpu(dip);
    mutex_exit(&nv_global_lock);

    return (rc);
}

static int
nvidia_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
    int rc;

    switch (cmd)
    {
        case DDI_DETACH:
            break;
        case DDI_SUSPEND:
            if (nv_dip_is_pseudo(dip))
                return (DDI_SUCCESS);
            if (nvswitch_dip_is_nvswitch(dip))
                return nvswitch_suspend(dip);
            return nv_suspend_gpu(ddi_get_driver_private(dip));
        default:
            return (DDI_FAILURE);
    }

    mutex_enter(&nv_global_lock);
    if (nv_dip_is_pseudo(dip))
        rc = nv_detach_ctl(dip);
    else if (nvswitch_dip_is_nvswitch(dip))
        rc = nvswitch_detach(dip);
    else
        rc = nv_detach_gpu(dip);
    mutex_exit(&nv_global_lock);

    return (rc);
}

/*
 * Fast reboot: stop the GPU from mastering the bus.  Must not block or
 * allocate.
 */
static int
nvidia_quiesce(dev_info_t *dip)
{
    nv_illumos_state_t *nvis;
    uint16_t cmd;

    if (nv_dip_is_pseudo(dip))
        return (DDI_SUCCESS);

    if (nvswitch_dip_is_nvswitch(dip))
        return nvswitch_quiesce(dip);

    nvis = ddi_get_driver_private(dip);
    if (nvis == NULL)
        return (DDI_SUCCESS);

    cmd = pci_config_get16(nvis->pci_cfg, PCI_CONF_COMM);
    pci_config_put16(nvis->pci_cfg, PCI_CONF_COMM,
        (cmd & ~PCI_COMM_ME) | PCI_COMM_INTX_DISABLE);

    return (DDI_SUCCESS);
}

static int
nvidia_getinfo(dev_info_t *unused, ddi_info_cmd_t cmd, void *arg, void **resultp)
{
    minor_t node = NV_MINOR_NODE(getminor((dev_t)arg));
    dev_info_t *dip = NULL;

    if (NV_MINOR_IS_GPU(node))
    {
        nv_illumos_state_t *nvis = nv_find_minor(node);

        if (nvis != NULL)
            dip = nvis->dip;
    }
    else if (NV_MINOR_IS_NVSWITCH(node))
    {
        dip = nvswitch_minor_to_dip(node);
    }
    else
    {
        dip = nv_ctl_dip;
    }

    switch (cmd)
    {
        case DDI_INFO_DEVT2DEVINFO:
            if (dip == NULL)
                return (DDI_FAILURE);
            *resultp = dip;
            return (DDI_SUCCESS);
        case DDI_INFO_DEVT2INSTANCE:
            if (dip == NULL)
                return (DDI_FAILURE);
            *resultp = (void *)(uintptr_t)ddi_get_instance(dip);
            return (DDI_SUCCESS);
        default:
            return (DDI_FAILURE);
    }
}

static struct dev_ops nvidia_dev_ops = {
    .devo_rev       = DEVO_REV,
    .devo_refcnt    = 0,
    .devo_getinfo   = nvidia_getinfo,
    .devo_identify  = nulldev,
    .devo_probe     = nulldev,
    .devo_attach    = nvidia_attach,
    .devo_detach    = nvidia_detach,
    .devo_reset     = nodev,
    .devo_cb_ops    = &nvidia_cb_ops,
    .devo_bus_ops   = NULL,
    .devo_power     = NULL,
    .devo_quiesce   = nvidia_quiesce,
};

static struct modldrv nvidia_modldrv = {
    &mod_driverops,
    "NVIDIA GPU driver " NV_VERSION_STRING,
    &nvidia_dev_ops
};

static struct modlinkage nvidia_modlinkage = {
    MODREV_1,
    { &nvidia_modldrv, NULL }
};

int
_init(void)
{
    int rc;

    nv_os_init();

    nv_stack_cache = kmem_cache_create("nvidia_stack_cache",
        sizeof (nvidia_stack_t), 16, NULL, NULL, NULL, NULL, NULL, 0);
    if (nv_stack_cache == NULL)
        return (ENOMEM);

    mutex_init(&nv_global_lock, NULL, MUTEX_DRIVER, NULL);
    rw_init(&nv_illumos_devices_lock, NULL, RW_DRIVER, NULL);
    rw_init(&nv_adapter_state_lock, NULL, RW_DRIVER, NULL);
    nv_report_error_init();
    nv_modeset_interface_init();
    nv_intr_init();
    nv_pm_init();

    if (nv_uvm_init() != 0)
    {
        rc = ENOMEM;
        goto fail;
    }

    rc = nv_clone_init();
    if (rc != 0)
        goto fail_uvm;

    rc = mod_install(&nvidia_modlinkage);
    if (rc != 0)
    {
        nv_clone_fini();
        goto fail_uvm;
    }

    return (0);

fail_uvm:
    nv_uvm_exit();
fail:
    nv_pm_fini();
    nv_intr_fini();
    nv_modeset_interface_fini();
    nv_report_error_fini();
    rw_destroy(&nv_illumos_devices_lock);
    rw_destroy(&nv_adapter_state_lock);
    mutex_destroy(&nv_global_lock);
    kmem_cache_destroy(nv_stack_cache);
    return (rc);
}

int
_fini(void)
{
    int rc;

    rc = mod_remove(&nvidia_modlinkage);
    if (rc != 0)
        return (rc);

    mutex_enter(&nv_global_lock);
    nv_global_fini();
    mutex_exit(&nv_global_lock);

    nv_clone_fini();
    nv_uvm_exit();
    nv_pm_fini();
    nv_intr_fini();
    nv_modeset_interface_fini();
    nv_report_error_fini();
    rw_destroy(&nv_illumos_devices_lock);
    rw_destroy(&nv_adapter_state_lock);
    mutex_destroy(&nv_global_lock);
    kmem_cache_destroy(nv_stack_cache);

    return (0);
}

int
_info(struct modinfo *modinfop)
{
    return (mod_info(&nvidia_modlinkage, modinfop));
}
