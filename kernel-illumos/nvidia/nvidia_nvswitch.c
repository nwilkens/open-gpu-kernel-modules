/*
 * SPDX-FileCopyrightText: Copyright (c) 2016-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * NVSwitch devices: attach/detach of NVSwitch PCI functions, BAR mapping,
 * interrupts and background tasks.  The character device entry points are
 * in nvidia_nvswitch_ioctl.c and the library's OS services in
 * nvidia_nvswitch_os.c.
 *
 * The Linux driver_mutex and device_mutex are binary semaphores here, so
 * user-context callers can be interrupted as with mutex_lock_interruptible().
 * Lock order: driver_lock, device_lock.  files_lock is never taken with
 * device_lock held.
 *
 * There is no driver procfs; what Linux shows under
 * /proc/driver/nvidia-nvswitch is published as devinfo properties of each
 * NVSwitch node and of the control node.  illumos x86 has no in-kernel I2C
 * client framework, so no I2C adapters are registered, as on Linux without
 * CONFIG_I2C.
 */

#include "nvidia_nvswitch.h"
#include "nvlink_proto.h"
#include "ioctl_nvswitch.h"

#include <sys/disp.h>

nvswitch_drv_t nvswitch_drv;

static const ddi_device_acc_attr_t nvswitch_reg_acc_attr = {
    DDI_DEVICE_ATTR_V0,
    DDI_STRUCTURE_LE_ACC,
    DDI_STRICTORDER_ACC
};

static const struct
{
    NvlStatus   status;
    int         err;
} nvswitch_status_map[] = {
    { NVL_ERR_GENERIC,                  EIO     },
    { NVL_NO_MEM,                       ENOMEM  },
    { NVL_BAD_ARGS,                     EINVAL  },
    { NVL_ERR_INVALID_STATE,            EIO     },
    { NVL_ERR_NOT_SUPPORTED,            ENOTSUP },
    { NVL_NOT_FOUND,                    EINVAL  },
    { NVL_ERR_STATE_IN_USE,             EBUSY   },
    { NVL_ERR_NOT_IMPLEMENTED,          ENOSYS  },
    { NVL_ERR_INSUFFICIENT_PERMISSIONS, EPERM   },
    { NVL_ERR_OPERATING_SYSTEM,         EIO     },
    { NVL_MORE_PROCESSING_REQUIRED,     EAGAIN  },
    { NVL_SUCCESS,                      0       },
};

int
nvswitch_map_status(NvlStatus status)
{
    size_t i;

    for (i = 0; i < NV_ARRAY_ELEMENTS(nvswitch_status_map); i++)
    {
        if (nvswitch_status_map[i].status == status ||
            nvswitch_status_map[i].status == -status)
            return (nvswitch_status_map[i].err);
    }

    return (EIO);
}

NvBool
nvswitch_is_device_blacklisted(nvswitch_dev_t *dev)
{
    NVSWITCH_DEVICE_FABRIC_STATE device_fabric_state = 0;
    NvlStatus status;

    status = nvswitch_lib_read_fabric_state(dev->lib_device,
        &device_fabric_state, NULL, NULL);
    if (status != NVL_SUCCESS)
    {
        nv_printf(NV_DBG_INFO, "%s: Failed to read fabric state, %x\n",
            dev->name, status);
        return NV_FALSE;
    }

    return (device_fabric_state == NVSWITCH_DEVICE_FABRIC_STATE_BLACKLISTED);
}

/*
 * Interrupts.  The library does not support MSI-X (Linux bug 3018806); MSI
 * vectors are never shared, and the pin interrupt is only used when the
 * library asks for it.
 */
static void
nvswitch_isr_bh(void *arg)
{
    nvswitch_dev_t *dev = arg;
    NvlStatus retval;

    atomic_swap_32(&dev->bh_pending, 0);

    sema_p(&dev->device_lock);

    if (dev->intr_stopping)
    {
        sema_v(&dev->device_lock);
        return;
    }

    retval = nvswitch_lib_service_interrupts(dev->lib_device);

    if (dev->irq_mechanism == NVSWITCH_IRQ_PIN)
        nvswitch_lib_enable_interrupts(dev->lib_device);

    sema_v(&dev->device_lock);

    if (retval != NVL_SUCCESS && nvlink_log_ratelimit())
    {
        dev_err(dev->dip, CE_WARN, "%s: Interrupts disabled to avoid a storm",
            dev->name);
    }
}

static void
nvswitch_schedule_bh(nvswitch_dev_t *dev)
{
    if (atomic_cas_32(&dev->bh_pending, 0, 1) == 0)
    {
        taskq_dispatch_ent(dev->bh_tq, nvswitch_isr_bh, dev, TQ_NOSLEEP,
            &dev->bh_ent);
    }
}

static uint_t
nvswitch_isr(caddr_t arg1, caddr_t arg2)
{
    nvswitch_dev_t *dev = (nvswitch_dev_t *)arg1;
    NvlStatus retval;

    if (!dev->intr_enabled)
        return (DDI_INTR_UNCLAIMED);

    if (dev->irq_mechanism == NVSWITCH_IRQ_MSI)
    {
        nvswitch_schedule_bh(dev);
        return (DDI_INTR_CLAIMED);
    }

    retval = nvswitch_lib_check_interrupts(dev->lib_device);

    if (retval == -NVL_MORE_PROCESSING_REQUIRED)
    {
        nvswitch_lib_disable_interrupts(dev->lib_device);
        nvswitch_schedule_bh(dev);
        return (DDI_INTR_CLAIMED);
    }

    if (retval != NVL_SUCCESS && retval != -NVL_PCI_ERROR &&
        nvlink_log_ratelimit())
    {
        cmn_err(CE_WARN, "nvidia-nvswitch: unrecoverable error in ISR");
    }

    return (DDI_INTR_UNCLAIMED);
}

static int
nvswitch_intr_alloc(nvswitch_dev_t *dev, int type)
{
    int count, actual = 0;
    uint_t pri;

    if (ddi_intr_get_nintrs(dev->dip, type, &count) != DDI_SUCCESS ||
        count < 1)
        return (DDI_FAILURE);

    if (ddi_intr_alloc(dev->dip, &dev->intr_hdl, type, 0, 1, &actual,
            DDI_INTR_ALLOC_STRICT) != DDI_SUCCESS || actual != 1)
        return (DDI_FAILURE);

    /* Match the priority of the GPU vectors. */
    if (ddi_intr_get_pri(dev->intr_hdl, &pri) != DDI_SUCCESS)
        goto fail;
    if (pri != nv_intr_pri &&
        ddi_intr_set_pri(dev->intr_hdl, nv_intr_pri) == DDI_SUCCESS)
        pri = nv_intr_pri;
    if (pri >= ddi_intr_get_hilevel_pri())
    {
        dev_err(dev->dip, CE_WARN, "interrupt priority %u is high-level", pri);
        goto fail;
    }

    if (ddi_intr_add_handler(dev->intr_hdl, nvswitch_isr, (caddr_t)dev,
            NULL) != DDI_SUCCESS)
        goto fail;

    (void) ddi_intr_get_cap(dev->intr_hdl, &dev->intr_cap);
    dev->intr_type = type;
    return (DDI_SUCCESS);

fail:
    (void) ddi_intr_free(dev->intr_hdl);
    dev->intr_hdl = NULL;
    return (DDI_FAILURE);
}

static int
nvswitch_initialize_device_interrupt(nvswitch_dev_t *dev)
{
    int types = 0, rc;
    NvBool pin = NV_FALSE;

    (void) ddi_intr_get_supported_types(dev->dip, &types);
    if (nvswitch_lib_use_pin_irq(dev->lib_device))
        pin = NV_TRUE;

    dev->irq_mechanism = NVSWITCH_IRQ_NONE;

    if ((types & DDI_INTR_TYPE_MSI) &&
        nvswitch_intr_alloc(dev, DDI_INTR_TYPE_MSI) == DDI_SUCCESS)
    {
        dev->irq_mechanism = NVSWITCH_IRQ_MSI;
        dev_err(dev->dip, CE_CONT, "?%s: using MSI\n", dev->name);
    }

    if (dev->irq_mechanism == NVSWITCH_IRQ_NONE && pin &&
        (types & DDI_INTR_TYPE_FIXED) &&
        nvswitch_intr_alloc(dev, DDI_INTR_TYPE_FIXED) == DDI_SUCCESS)
    {
        dev->irq_mechanism = NVSWITCH_IRQ_PIN;
        dev_err(dev->dip, CE_CONT, "?%s: using PCI pin\n", dev->name);
    }

    if (dev->irq_mechanism == NVSWITCH_IRQ_NONE)
    {
        dev_err(dev->dip, CE_WARN, "%s: No supported interrupt mechanism was "
            "found. This device supports:%s%s%s", dev->name,
            (types & DDI_INTR_TYPE_MSIX) ? " MSI-X" : "",
            (types & DDI_INTR_TYPE_MSI) ? " MSI" : "",
            pin ? " PCI Pin" : "");
        return (EINVAL);
    }

    dev->bh_tq = taskq_create(dev->sname, 1, maxclsyspri, 1, 1,
        TASKQ_PREPOPULATE);
    dev->bh_pending = 0;
    dev->wake_pending = 0;
    dev->intr_stopping = NV_FALSE;
    dev->intr_enabled = NV_TRUE;
    membar_producer();

    if (dev->intr_cap & DDI_INTR_FLAG_BLOCK)
        rc = ddi_intr_block_enable(&dev->intr_hdl, 1);
    else
        rc = ddi_intr_enable(dev->intr_hdl);

    if (rc != DDI_SUCCESS)
    {
        dev_err(dev->dip, CE_WARN, "%s: failed to enable interrupts",
            dev->name);
        dev->intr_enabled = NV_FALSE;
        (void) ddi_intr_remove_handler(dev->intr_hdl);
        (void) ddi_intr_free(dev->intr_hdl);
        dev->intr_hdl = NULL;
        taskq_destroy(dev->bh_tq);
        dev->bh_tq = NULL;
        return (EIO);
    }

    return (0);
}

static void
nvswitch_shutdown_device_interrupt(nvswitch_dev_t *dev)
{
    if (dev->intr_hdl == NULL)
        return;

    if (dev->intr_cap & DDI_INTR_FLAG_BLOCK)
        (void) ddi_intr_block_disable(&dev->intr_hdl, 1);
    else
        (void) ddi_intr_disable(dev->intr_hdl);

    dev->intr_enabled = NV_FALSE;
    (void) ddi_intr_remove_handler(dev->intr_hdl);
    (void) ddi_intr_free(dev->intr_hdl);
    dev->intr_hdl = NULL;

    /* Runs any queued bottom half and wakeup. */
    taskq_destroy(dev->bh_tq);
    dev->bh_tq = NULL;
}

/*
 * Background tasks, as the Linux per-device kthread queue.
 */
static void
nvswitch_task_dispatch(void *arg)
{
    nvswitch_dev_t *dev = arg;
    clock_t deadline;
    NvU64 nsec;

    mutex_enter(&dev->task_lock);
    while (dev->task_q_ready)
    {
        mutex_exit(&dev->task_lock);

        sema_p(&dev->device_lock);
        nsec = nvswitch_lib_deferred_task_dispatcher(dev->lib_device);
        sema_v(&dev->device_lock);

        /* An invalid device reports NV_U64_MAX. */
        deadline = ddi_get_lbolt() +
            MAX(drv_usectohz((clock_t)MIN(nsec / 1000, (NvU64)INT32_MAX)), 1);

        mutex_enter(&dev->task_lock);
        while (dev->task_q_ready &&
               cv_timedwait(&dev->task_cv, &dev->task_lock, deadline) != -1)
            ;
    }
    mutex_exit(&dev->task_lock);
}

static void
nvswitch_init_background_tasks(nvswitch_dev_t *dev)
{
    char name[32];

    (void) snprintf(name, sizeof (name), "%s_task", dev->sname);
    dev->task_tq = taskq_create(name, 1, minclsyspri, 1, 1, 0);

    mutex_enter(&dev->task_lock);
    dev->task_q_ready = NV_TRUE;
    mutex_exit(&dev->task_lock);

    taskq_dispatch_ent(dev->task_tq, nvswitch_task_dispatch, dev, TQ_SLEEP,
        &dev->task_ent);
}

static void
nvswitch_deinit_background_tasks(nvswitch_dev_t *dev)
{
    if (dev->task_tq == NULL)
        return;

    mutex_enter(&dev->task_lock);
    dev->task_q_ready = NV_FALSE;
    cv_broadcast(&dev->task_cv);
    mutex_exit(&dev->task_lock);

    taskq_destroy(dev->task_tq);
    dev->task_tq = NULL;
}

/*
 * PCI resources.
 */
static int
nvswitch_find_bar0(nvswitch_dev_t *dev, const char *prop, uint_t *index,
    NvU64 *addr)
{
    pci_regspec_t *regs;
    uint_t nelem, n, i;
    int rc = DDI_FAILURE;

    if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dev->dip, DDI_PROP_DONTPASS,
            (char *)prop, (int **)&regs, &nelem) != DDI_PROP_SUCCESS)
        return (DDI_FAILURE);

    n = nelem / (sizeof (pci_regspec_t) / sizeof (int));
    for (i = 0; i < n; i++)
    {
        if (PCI_REG_REG_G(regs[i].pci_phys_hi) == PCI_CONF_BASE0)
        {
            *index = i;
            *addr = ((NvU64)regs[i].pci_phys_mid << 32) | regs[i].pci_phys_low;
            rc = DDI_SUCCESS;
            break;
        }
    }

    ddi_prop_free(regs);
    return (rc);
}

static int
nvswitch_map_bar0(nvswitch_dev_t *dev)
{
    uint_t rnumber;
    NvU64 unused;

    if (nvswitch_find_bar0(dev, "reg", &rnumber, &unused) != DDI_SUCCESS ||
        ddi_dev_regsize(dev->dip, rnumber, &dev->bar0_size) != DDI_SUCCESS ||
        dev->bar0_size <= 0)
        return (ENXIO);

    if (ddi_regs_map_setup(dev->dip, rnumber, &dev->bar0, 0, dev->bar0_size,
            &nvswitch_reg_acc_attr, &dev->bar0_hdl) != DDI_SUCCESS)
        return (ENOMEM);

    return (0);
}

/*
 * The library accesses registers through pBar with plain volatile loads and
 * stores, which is what the access handle does for a memory BAR on x86.
 */
static void
nvswitch_load_bar_info(nvswitch_dev_t *dev)
{
    nvlink_pci_info *info;
    uint32_t bar;
    uint_t unused;
    NvU64 phys = 0;

    nvswitch_lib_get_device_info(dev->lib_device, &info);

    info->bars[0].offset = PCI_CONF_BASE0;
    bar = pci_config_get32(dev->pci_cfg, PCI_CONF_BASE0);
    info->bars[0].busAddress = bar & PCI_BASE_M_ADDR_M;
    if ((bar & PCI_BASE_TYPE_M) == PCI_BASE_TYPE_ALL)
    {
        bar = pci_config_get32(dev->pci_cfg, PCI_CONF_BASE0 + 4);
        info->bars[0].busAddress |= ((NvU64)bar) << 32;
    }

    (void) nvswitch_find_bar0(dev, "assigned-addresses", &unused, &phys);
    info->bars[0].baseAddr = phys;
    info->bars[0].barSize = (NvU64)dev->bar0_size;
    info->bars[0].pBar = dev->bar0;
}

/*
 * Device bring-up and teardown, following Linux nvswitch_probe() and
 * nvswitch_remove().
 */
static int
nvswitch_init_device(nvswitch_dev_t *dev)
{
    NvlStatus retval;
    int rc;

    retval = nvswitch_lib_register_device(0, dev->bus, dev->slot, dev->func,
        dev->device_id, dev->os_ctx, dev->minor, &dev->lib_device);
    if (retval != NVL_SUCCESS)
    {
        dev_err(dev->dip, CE_WARN, "%s: Failed to register device : %d",
            dev->name, retval);
        return (ENODEV);
    }

    nvswitch_load_bar_info(dev);

    retval = nvswitch_lib_initialize_device(dev->lib_device);
    if (retval != NVL_SUCCESS)
    {
        dev_err(dev->dip, CE_WARN, "%s: Failed to initialize device : %d",
            dev->name, retval);
        rc = ENODEV;
        goto init_device_failed;
    }

    nvswitch_lib_get_uuid(dev->lib_device, &dev->uuid);

    if (nvswitch_lib_get_bios_version(dev->lib_device, &dev->bios_ver) !=
            NVL_SUCCESS)
        dev->bios_ver = 0;

    if (nvswitch_lib_get_physid(dev->lib_device, &dev->phys_id) !=
            NVL_SUCCESS)
        dev->phys_id = NVSWITCH_INVALID_PHYS_ID;

    rc = nvswitch_initialize_device_interrupt(dev);
    if (rc != 0)
    {
        dev_err(dev->dip, CE_WARN, "%s: Failed to initialize interrupt : %d",
            dev->name, rc);
        goto init_intr_failed;
    }

    if (nvswitch_is_device_blacklisted(dev))
    {
        dev_err(dev->dip, CE_WARN, "%s: Blacklisted nvswitch device",
            dev->name);
        /* Keep device registered for HAL access and Fabric State updates */
        return (0);
    }

    nvswitch_lib_enable_interrupts(dev->lib_device);
    return (0);

init_intr_failed:
    (void) nvswitch_lib_shutdown_device(dev->lib_device);
init_device_failed:
    nvswitch_lib_unregister_device(dev->lib_device);
    dev->lib_device = NULL;
    return (rc);
}

static int
nvswitch_post_init_device(nvswitch_dev_t *dev)
{
    NvU32 valid_ports_mask;

    if (nvswitch_lib_is_i2c_supported(dev->lib_device) &&
        nvswitch_lib_get_valid_ports_mask(dev->lib_device,
            &valid_ports_mask) != NVL_SUCCESS)
    {
        dev_err(dev->dip, CE_WARN, "Failed to get valid I2C ports mask.");
        return (ENODEV);
    }

    if (nvswitch_lib_post_init_device(dev->lib_device) != NVL_SUCCESS)
        return (ENODEV);

    return (0);
}

static void
nvswitch_deinit_device(nvswitch_dev_t *dev)
{
    /* A queued pin bottom half must not re-enable interrupts. */
    sema_p(&dev->device_lock);
    dev->intr_stopping = NV_TRUE;
    nvswitch_lib_disable_interrupts(dev->lib_device);
    sema_v(&dev->device_lock);

    nvswitch_shutdown_device_interrupt(dev);

    (void) nvswitch_lib_shutdown_device(dev->lib_device);
    nvswitch_lib_unregister_device(dev->lib_device);
    dev->lib_device = NULL;
}

static void
nvswitch_publish_info(nvswitch_dev_t *dev)
{
    char uuid_string[NVSWITCH_UUID_STRING_LENGTH] = { 0 };
    char bios[32];

    if (dev->bios_ver != 0)
    {
        (void) snprintf(bios, sizeof (bios),
            "%02llx.%02llx.%02llx.%02llx.%02llx",
            (unsigned long long)(dev->bios_ver >> 32),
            (unsigned long long)((dev->bios_ver >> 24) & 0xFF),
            (unsigned long long)((dev->bios_ver >> 16) & 0xFF),
            (unsigned long long)((dev->bios_ver >> 8) & 0xFF),
            (unsigned long long)(dev->bios_ver & 0xFF));
    }
    else
    {
        (void) strlcpy(bios, "N/A", sizeof (bios));
    }

    (void) nvswitch_uuid_to_string(&dev->uuid, uuid_string,
        sizeof (uuid_string));

    (void) ddi_prop_update_string(DDI_DEV_T_NONE, dev->dip,
        "nvswitch-bios-version", bios);
    (void) ddi_prop_update_string(DDI_DEV_T_NONE, dev->dip,
        "nvswitch-uuid", uuid_string);
    (void) ddi_prop_update_int(DDI_DEV_T_NONE, dev->dip,
        "nvswitch-physical-location-id", (int)dev->phys_id);
    (void) ddi_prop_update_int(DDI_DEV_T_NONE, dev->dip,
        "nvswitch-device-instance", dev->minor);
}

static void
nvswitch_dev_free(nvswitch_dev_t *dev)
{
    if (dev->os_ctx != NULL)
        nvswitch_os_ctx_destroy(dev->os_ctx);
    if (dev->bar0_hdl != NULL)
        ddi_regs_map_free(&dev->bar0_hdl);
    if (dev->pci_cfg != NULL)
        pci_config_teardown(&dev->pci_cfg);

    list_destroy(&dev->files);
    mutex_destroy(&dev->files_lock);
    cv_destroy(&dev->task_cv);
    mutex_destroy(&dev->task_lock);
    sema_destroy(&dev->device_lock);
    kmem_free(dev, sizeof (*dev));
}

/* Attached switches are found without property lookups for quiesce(9E). */
NvBool
nvswitch_dip_is_nvswitch(dev_info_t *dip)
{
    int vendor, class, i;

    for (i = 0; i < NVSWITCH_DEVICE_INSTANCE_MAX; i++)
    {
        if (nvswitch_drv.dips[i] == dip)
            return NV_TRUE;
    }

    vendor = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
        "vendor-id", -1);
    class = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
        "class-code", -1);

    return (vendor == PCI_VENDOR_ID_NVIDIA && class != -1 &&
            ((class >> 8) & 0xffff) == PCI_CLASS_BRIDGE_NVSWITCH);
}

int
nvswitch_attach(dev_info_t *dip)
{
    nvswitch_dev_t *dev;
    ddi_acc_handle_t cfg;
    uint16_t vendor, device, cmd;
    uint32_t class;
    NvU8 bus, slot, func;
    char node[32];
    int minor, rc;

    if (!nvswitch_drv.initialized)
        return (DDI_FAILURE);

    if (nv_pci_dip_bdf(dip, &bus, &slot, &func) != DDI_SUCCESS)
        return (DDI_FAILURE);

    if (pci_config_setup(dip, &cfg) != DDI_SUCCESS)
        return (DDI_FAILURE);

    vendor = pci_config_get16(cfg, PCI_CONF_VENID);
    device = pci_config_get16(cfg, PCI_CONF_DEVID);
    class = pci_config_get32(cfg, PCI_CONF_REVID) >> 8;

    if (!nvswitch_lib_validate_device_id(device))
    {
        pci_config_teardown(&cfg);
        return (DDI_FAILURE);
    }

    dev_err(dip, CE_CONT, "?nvidia-nvswitch: Probing device "
        "%04x:%02x:%02x.%x, Vendor Id = 0x%x, Device Id = 0x%x, "
        "Class = 0x%x\n", 0, bus, slot, func, vendor, device, class);

    sema_p(&nvswitch_drv.driver_lock);

    for (minor = 0; minor < NVSWITCH_DEVICE_INSTANCE_MAX; minor++)
    {
        if (nvswitch_drv.devs[minor] == NULL)
            break;
    }
    if (minor == NVSWITCH_DEVICE_INSTANCE_MAX)
    {
        sema_v(&nvswitch_drv.driver_lock);
        pci_config_teardown(&cfg);
        return (DDI_FAILURE);
    }

    dev = kmem_zalloc(sizeof (*dev), KM_SLEEP);
    sema_init(&dev->device_lock, 1, NULL, SEMA_DRIVER, NULL);
    mutex_init(&dev->task_lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&dev->task_cv, NULL, CV_DRIVER, NULL);
    mutex_init(&dev->files_lock, NULL, MUTEX_DRIVER, NULL);
    list_create(&dev->files, sizeof (nvswitch_file_private_t),
        offsetof(nvswitch_file_private_t, link));

    (void) snprintf(dev->name, sizeof (dev->name), NVSWITCH_DRIVER_NAME "%d",
        minor);
    (void) snprintf(dev->sname, sizeof (dev->sname), NVSWITCH_SHORT_NAME "%d",
        minor);
    dev->minor = minor;
    dev->dip = dip;
    dev->pci_cfg = cfg;
    dev->device_id = device;
    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->os_ctx = nvswitch_os_ctx_create(dip);

    /* pci_enable_device() and pci_set_master() */
    cmd = pci_config_get16(cfg, PCI_CONF_COMM);
    pci_config_put16(cfg, PCI_CONF_COMM, cmd | PCI_COMM_MAE | PCI_COMM_ME);

    rc = nvswitch_map_bar0(dev);
    if (rc != 0)
    {
        dev_err(dip, CE_WARN, "%s: Failed to map BAR0 region : %d",
            dev->name, rc);
        goto fail_bar;
    }

    rc = nvswitch_init_device(dev);
    if (rc != 0)
    {
        dev_err(dip, CE_WARN, "%s: Failed to initialize device : %d",
            dev->name, rc);
        goto fail_bar;
    }

    if (nvswitch_is_device_blacklisted(dev))
    {
        nvswitch_lib_post_init_blacklist_device(dev->lib_device);
    }
    else
    {
        /* Held because post-init may service the SOE with interrupts on. */
        sema_p(&dev->device_lock);
        rc = nvswitch_post_init_device(dev);
        sema_v(&dev->device_lock);
        if (rc != 0)
        {
            dev_err(dip, CE_WARN, "%s:Failed during device post init : %d",
                dev->name, rc);
            goto fail_device;
        }
    }

    nvswitch_init_background_tasks(dev);

    (void) snprintf(node, sizeof (node), NVSWITCH_DRIVER_NAME "%d", minor);
    if (ddi_create_minor_node(dip, node, S_IFCHR,
            NV_MINOR_NVSWITCH_BASE + minor, NV_DDI_NT_NVIDIA, 0) != DDI_SUCCESS)
    {
        rc = ENXIO;
        goto fail_tasks;
    }

    nvswitch_publish_info(dev);
    ddi_set_driver_private(dip, dev);

    nvswitch_drv.devs[minor] = dev;
    nvswitch_drv.dips[minor] = dip;
    nvswitch_drv.count++;

    sema_v(&nvswitch_drv.driver_lock);

    ddi_report_dev(dip);
    return (DDI_SUCCESS);

fail_tasks:
    nvswitch_deinit_background_tasks(dev);
fail_device:
    nvswitch_deinit_device(dev);
fail_bar:
    pci_config_put16(cfg, PCI_CONF_COMM, cmd);
    nvswitch_dev_free(dev);
    sema_v(&nvswitch_drv.driver_lock);
    return (DDI_FAILURE);
}

int
nvswitch_detach(dev_info_t *dip)
{
    nvswitch_dev_t *dev = ddi_get_driver_private(dip);
    uint16_t cmd;

    if (dev == NULL)
        return (DDI_FAILURE);

    sema_p(&nvswitch_drv.driver_lock);

    if (dev->ref_count != 0)
    {
        sema_v(&nvswitch_drv.driver_lock);
        return (DDI_FAILURE);
    }

    dev_err(dip, CE_CONT, "?%s: removing device %04x:%02x:%02x.%x\n",
        dev->name, 0, dev->bus, dev->slot, dev->func);

    sema_p(&dev->device_lock);
    dev->unusable = NV_TRUE;
    sema_v(&dev->device_lock);

    nvswitch_drv.devs[dev->minor] = NULL;
    nvswitch_drv.dips[dev->minor] = NULL;
    nvswitch_drv.count--;

    ddi_remove_minor_node(dip, NULL);

    nvswitch_deinit_background_tasks(dev);
    nvswitch_deinit_device(dev);

    ddi_set_driver_private(dip, NULL);
    (void) ddi_prop_remove_all(dip);

    /* pci_clear_master() */
    cmd = pci_config_get16(dev->pci_cfg, PCI_CONF_COMM);
    pci_config_put16(dev->pci_cfg, PCI_CONF_COMM, cmd & ~PCI_COMM_ME);

    nvswitch_dev_free(dev);

    sema_v(&nvswitch_drv.driver_lock);
    return (DDI_SUCCESS);
}

/* The Linux driver has no power management callbacks. */
int
nvswitch_suspend(dev_info_t *dip)
{
    return (DDI_FAILURE);
}

int
nvswitch_resume(dev_info_t *dip)
{
    return (DDI_SUCCESS);
}

/* Fast reboot: stop DMA and interrupts.  Must not block. */
int
nvswitch_quiesce(dev_info_t *dip)
{
    nvswitch_dev_t *dev = ddi_get_driver_private(dip);
    uint16_t cmd;

    if (dev == NULL || dev->pci_cfg == NULL)
        return (DDI_SUCCESS);

    cmd = pci_config_get16(dev->pci_cfg, PCI_CONF_COMM);
    pci_config_put16(dev->pci_cfg, PCI_CONF_COMM,
        (cmd & ~PCI_COMM_ME) | PCI_COMM_INTX_DISABLE);

    return (DDI_SUCCESS);
}

dev_info_t *
nvswitch_minor_to_dip(NvU32 node)
{
    NvU32 index;

    if (node == NV_MINOR_NVSWITCH_CTL)
        return (nv_ctl_dip);

    if (!NV_MINOR_IS_NVSWITCH(node))
        return (NULL);

    index = node - NV_MINOR_NVSWITCH_BASE;
    if (index >= NVSWITCH_DEVICE_INSTANCE_MAX)
        return (NULL);

    return (nvswitch_drv.dips[index]);
}

/*
 * Driver setup, called through nvlink_drivers_init()/exit() and the control
 * node attach hooks.
 */
int
nvswitch_init(void)
{
    CTASSERT(NVSWITCH_DEVICE_INSTANCE_MAX == NVSWITCH_MAX_DEVICES);
    CTASSERT(NVSWITCH_DEVICE_INSTANCE_MAX <= NV_MINOR_NVSWITCH_COUNT);

    if (nvswitch_drv.initialized)
    {
        nv_printf(NV_DBG_ERRORS,
            "nvidia-nvswitch: Interface already initialized\n");
        return (EBUSY);
    }

    sema_init(&nvswitch_drv.driver_lock, 1, NULL, SEMA_DRIVER, NULL);
    nvswitch_drv.initialized = NV_TRUE;

    return (0);
}

void
nvswitch_exit(void)
{
    if (!nvswitch_drv.initialized)
        return;

    if (nvswitch_drv.count != 0)
    {
        nv_printf(NV_DBG_ERRORS,
            "nvidia-nvswitch: %u devices still attached at exit\n",
            nvswitch_drv.count);
    }

    nvswitch_drv.initialized = NV_FALSE;
    sema_destroy(&nvswitch_drv.driver_lock);
}

int
nvswitch_ctl_attach(dev_info_t *dip)
{
    if (!nvswitch_drv.initialized)
        return (DDI_FAILURE);

    if (ddi_create_minor_node(dip, NVSWITCH_CTL_NAME, S_IFCHR,
            NV_MINOR_NVSWITCH_CTL, NV_DDI_NT_NVIDIA, 0) != DDI_SUCCESS)
    {
        dev_err(dip, CE_WARN, "failed to create the %s node",
            NVSWITCH_CTL_NAME);
        return (DDI_FAILURE);
    }

    (void) ddi_prop_update_int(DDI_DEV_T_NONE, dip,
        NVSWITCH_DEVICE_FILE_MODE_PROP, NVSWITCH_DEVICE_FILE_MODE);

    sema_p(&nvswitch_drv.driver_lock);
    nvswitch_drv.ctl_dip = dip;
    sema_v(&nvswitch_drv.driver_lock);

    return (DDI_SUCCESS);
}

/* The switches depend on the fabric-mgmt capability of the control node. */
int
nvswitch_ctl_detach(dev_info_t *dip)
{
    if (!nvswitch_drv.initialized)
        return (DDI_SUCCESS);

    sema_p(&nvswitch_drv.driver_lock);

    if (nvswitch_drv.count != 0 || nvswitch_drv.ctl_opens != 0)
    {
        sema_v(&nvswitch_drv.driver_lock);
        return (DDI_FAILURE);
    }

    nvswitch_drv.ctl_dip = NULL;
    ddi_remove_minor_node(dip, NVSWITCH_CTL_NAME);
    (void) ddi_prop_remove(DDI_DEV_T_NONE, dip, NVSWITCH_DEVICE_FILE_MODE_PROP);

    sema_v(&nvswitch_drv.driver_lock);
    return (DDI_SUCCESS);
}
