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
 * NVLink core library OS layer, the /dev/nvidia-nvlink node and the NVLink
 * fabric-management capability.
 *
 * The NVLink and NVSwitch control nodes and the capability live on the
 * pseudo control node, which can attach after RM is up, so they are
 * published from nvlink_ctl_attach() rather than nvlink_drivers_init().
 * The Linux procfs "permissions" file becomes a property of that node.
 *
 * A capability presented by a client is kept as a kernel reference to its
 * open file for the life of the client's open, where Linux keeps a
 * duplicate descriptor in the client process.
 */

#include "nvidia_nvswitch.h"
#include "nvlink_os.h"
#include "nvlink_export.h"
#include "nvlink_proto.h"

#include <sys/ioccom.h>

#define NVLINK_DEVICE_NAME          "nvidia-nvlink"
#define NVLINK_CAP_ROOT             "driver/nvidia-nvlink"
#define NVLINK_CAP_FABRIC_MGMT      "fabric-mgmt"
#define NVLINK_DEVICE_FILE_MODE     0666
#define NVLINK_DEVICE_FILE_MODE_PROP "nvidia-nvlink-device-file-mode"

typedef struct nvlink_file_private
{
    /* capability file held by ACQUIRE_CAPABILITY, or NULL */
    file_t *volatile fabric_mgmt;
} nvlink_file_private_t;

static struct
{
    kmutex_t        lock;
    NvBool          initialized;
    NvBool          opened;
    dev_info_t     *ctl_dip;
} nvlink_drvctx;

static struct
{
    krwlock_t       lock;
    nv_cap_t       *root;
    nv_cap_t       *fabric_mgmt;
} nvlink_caps;

/*
 * Approximates Linux printk_ratelimit(): a burst of 10 messages every
 * 5 seconds, for messages that unprivileged callers can trigger.
 */
NvBool
nvlink_log_ratelimit(void)
{
    static volatile uint64_t begin;
    static volatile uint32_t printed;
    hrtime_t now = gethrtime();
    uint64_t start = begin;

    if ((uint64_t)now - start > 5ULL * NANOSEC &&
        atomic_cas_64(&begin, start, (uint64_t)now) == start)
        atomic_swap_32(&printed, 0);

    return (atomic_inc_32_nv(&printed) <= 10);
}

/*
 * NVLink and NVSwitch commands use the Linux _IOC layout, with a 14-bit
 * argument size.  Commands built with the illumos macros keep only 8 size
 * bits, so arguments above 255 bytes need the Linux layout.  The two
 * layouts disagree on the _IOR and _IOW direction bits, so callers copy the
 * argument in and out regardless of direction; Linux lets any caller pick
 * _IOWR for any command number, so this exposes nothing new.
 */
int
nvlink_ioc_param_size(uint32_t cmd, size_t *sizep)
{
    if (NV_IOC_DIR(cmd) == 0)
    {
        /* _IO: no size on Linux, IOC_VOID on illumos. */
        uint32_t size_bits = cmd & 0x3fff0000U;

        if (size_bits != 0 && size_bits != IOC_VOID)
            return (EINVAL);
        *sizep = 0;
        return (0);
    }

    *sizep = NV_IOC_SIZE(cmd);
    return (0);
}

/*
 * Capabilities.
 */
static int
nvlink_cap_init(const char *path)
{
    nv_cap_t *root, *fabric_mgmt;

    root = os_nv_cap_init(path);
    if (root == NULL)
    {
        nv_printf(NV_DBG_ERRORS,
            "nvidia-nvlink: Failed to initialize capabilities\n");
        return (-1);
    }

    fabric_mgmt = os_nv_cap_create_file_entry(root, NVLINK_CAP_FABRIC_MGMT,
        S_IRUSR);
    if (fabric_mgmt == NULL)
    {
        nv_printf(NV_DBG_ERRORS,
            "nvidia-nvlink: Failed to create fabric-mgmt entry\n");
        os_nv_cap_destroy_entry(root);
        return (-1);
    }

    rw_enter(&nvlink_caps.lock, RW_WRITER);
    nvlink_caps.root = root;
    nvlink_caps.fabric_mgmt = fabric_mgmt;
    rw_exit(&nvlink_caps.lock);

    return (0);
}

static void
nvlink_cap_exit(void)
{
    nv_cap_t *root, *fabric_mgmt;

    rw_enter(&nvlink_caps.lock, RW_WRITER);
    root = nvlink_caps.root;
    fabric_mgmt = nvlink_caps.fabric_mgmt;
    nvlink_caps.root = NULL;
    nvlink_caps.fabric_mgmt = NULL;
    rw_exit(&nvlink_caps.lock);

    if (fabric_mgmt != NULL)
        os_nv_cap_destroy_entry(fabric_mgmt);
    if (root != NULL)
        os_nv_cap_destroy_entry(root);
}

/* Returns a held capability file if fd is the fabric-mgmt capability. */
file_t *
nvlink_fabric_mgmt_hold(int fd)
{
    file_t *fp;

    if (fd < 0)
        return (NULL);

    rw_enter(&nvlink_caps.lock, RW_READER);
    fp = nv_cap_hold_file(nvlink_caps.fabric_mgmt, fd);
    rw_exit(&nvlink_caps.lock);

    if (fp == NULL && nvlink_log_ratelimit())
    {
        nv_printf(NV_DBG_ERRORS,
            "nvidia-nvlink: Failed to validate the fabric mgmt capability\n");
    }

    return (fp);
}

void
nvlink_fabric_mgmt_rele(file_t *fp)
{
    nv_cap_rele_file(fp);
}

/* RM's variant hands the capability back as a descriptor, as on Linux. */
NV_STATUS NV_API_CALL nv_acquire_fabric_mgmt_cap(int fd, int *duped_fd)
{
    *duped_fd = -1;

    if (fd >= 0)
    {
        rw_enter(&nvlink_caps.lock, RW_READER);
        *duped_fd = os_nv_cap_validate_and_dup_fd(nvlink_caps.fabric_mgmt, fd);
        rw_exit(&nvlink_caps.lock);
    }

    if (*duped_fd < 0)
        return NV_ERR_INSUFFICIENT_PERMISSIONS;

    return NV_OK;
}

NvlStatus
nvlink_acquire_fabric_mgmt_cap(void *osPrivate, NvU64 capDescriptor)
{
    nvlink_file_private_t *private_data = osPrivate;
    file_t *fp, *old;

    if (private_data == NULL)
        return NVL_BAD_ARGS;

    fp = nvlink_fabric_mgmt_hold((int)capDescriptor);
    if (fp == NULL)
        return NVL_ERR_OPERATING_SYSTEM;

    old = atomic_swap_ptr(&private_data->fabric_mgmt, fp);
    if (old != NULL)
        nvlink_fabric_mgmt_rele(old);

    return NVL_SUCCESS;
}

int
nvlink_is_fabric_manager(void *osPrivate)
{
    nvlink_file_private_t *private_data = osPrivate;

    return (private_data != NULL && private_data->fabric_mgmt != NULL);
}

int
nvlink_is_admin(void)
{
    return (os_is_administrator() ? 1 : 0);
}

/*
 * /dev/nvidia-nvlink.  Opens are exclusive, as on Linux.  Only privileged
 * callers may open it, so an ordinary user cannot hold the node and lock
 * out the fabric manager.
 */
int
nvlink_node_open(nv_illumos_file_private_t *nvifp, cred_t *credp)
{
    nvlink_file_private_t *private;

    nv_printf(NV_DBG_INFO, "nvidia-nvlink: nvlink driver open\n");

    if (!nvlink_drvctx.initialized)
        return (ENXIO);

    if (drv_priv(credp) != 0)
        return (EPERM);

    mutex_enter(&nvlink_drvctx.lock);

    if (nvlink_drvctx.ctl_dip == NULL)
    {
        mutex_exit(&nvlink_drvctx.lock);
        return (ENXIO);
    }

    if (nvlink_drvctx.opened)
    {
        mutex_exit(&nvlink_drvctx.lock);
        return (EBUSY);
    }

    private = kmem_zalloc(sizeof (*private), KM_SLEEP);
    nvifp->subsys_priv = private;
    nvlink_drvctx.opened = NV_TRUE;

    mutex_exit(&nvlink_drvctx.lock);
    return (0);
}

void
nvlink_node_close(nv_illumos_file_private_t *nvifp)
{
    nvlink_file_private_t *private = nvifp->subsys_priv;
    file_t *fp;

    nv_printf(NV_DBG_INFO, "nvidia-nvlink: nvlink driver close\n");

    if (private == NULL)
        return;

    fp = atomic_swap_ptr(&private->fabric_mgmt, NULL);
    if (fp != NULL)
        nvlink_fabric_mgmt_rele(fp);

    mutex_enter(&nvlink_drvctx.lock);
    nvifp->subsys_priv = NULL;
    kmem_free(private, sizeof (*private));
    nvlink_drvctx.opened = NV_FALSE;
    mutex_exit(&nvlink_drvctx.lock);
}

int
nvlink_node_ioctl(nv_illumos_file_private_t *nvifp, int cmd, intptr_t arg,
    int mode, cred_t *credp)
{
    nvlink_ioctrl_params ctrl_params;
    uint32_t ucmd = (uint32_t)cmd;
    void *buf = NULL;
    size_t size;
    int rc;

    if (nvifp->subsys_priv == NULL)
        return (ENXIO);

    if ((rc = nvlink_ioc_param_size(ucmd, &size)) != 0)
        return (rc);

    if (size != 0)
    {
        buf = kmem_zalloc(size, KM_SLEEP);
        if (ddi_copyin((void *)arg, buf, size, mode) != 0)
        {
            rc = EFAULT;
            goto done;
        }
    }

    bzero(&ctrl_params, sizeof (ctrl_params));
    ctrl_params.osPrivate = nvifp->subsys_priv;
    ctrl_params.cmd = NV_IOC_NR(ucmd);
    ctrl_params.buf = buf;
    ctrl_params.size = (NvU32)size;

    if (nvlink_lib_ioctl_ctrl(&ctrl_params) != NVL_SUCCESS)
    {
        rc = EINVAL;
        goto done;
    }

    if (size != 0 && ddi_copyout(buf, (void *)arg, size, mode) != 0)
        rc = EFAULT;

done:
    if (buf != NULL)
        kmem_free(buf, size);
    return (rc);
}

/*
 * Driver setup.
 */
int
nvlink_core_init(void)
{
    NvlStatus status;

    if (nvlink_drvctx.initialized)
    {
        nv_printf(NV_DBG_ERRORS,
            "nvidia-nvlink: nvlink core interface already initialized\n");
        return (EBUSY);
    }

    mutex_init(&nvlink_drvctx.lock, NULL, MUTEX_DRIVER, NULL);
    rw_init(&nvlink_caps.lock, NULL, RW_DRIVER, NULL);

    status = nvlink_lib_initialize();
    if (status != NVL_SUCCESS)
    {
        nv_printf(NV_DBG_ERRORS,
            "nvidia-nvlink: Failed to initialize driver : %d\n", status);
        rw_destroy(&nvlink_caps.lock);
        mutex_destroy(&nvlink_drvctx.lock);
        return (ENODEV);
    }

    nvlink_drvctx.initialized = NV_TRUE;
    return (0);
}

void
nvlink_core_exit(void)
{
    if (!nvlink_drvctx.initialized)
        return;

    nvlink_cap_exit();
    (void) nvlink_lib_unload();

    nvlink_drvctx.initialized = NV_FALSE;
    rw_destroy(&nvlink_caps.lock);
    mutex_destroy(&nvlink_drvctx.lock);
}

int
nvlink_drivers_init(void)
{
    int rc;

    rc = nvlink_core_init();
    if (rc != 0)
    {
        nv_printf(NV_DBG_INFO, "NVRM: NVLink core init failed.\n");
        return (rc);
    }

    rc = nvswitch_init();
    if (rc != 0)
    {
        nv_printf(NV_DBG_INFO, "NVRM: NVSwitch init failed.\n");
        nvlink_core_exit();
    }

    return (rc);
}

void
nvlink_drivers_exit(void)
{
    nvswitch_exit();
    nvlink_core_exit();
}

/*
 * Called once nv_ctl_dip is set.  A failure leaves the control node usable
 * for GPUs, without NVLink and NVSwitch management.
 */
void
nvlink_ctl_attach(dev_info_t *dip)
{
    mutex_enter(&nvlink_drvctx.lock);

    if (!nvlink_drvctx.initialized || nvlink_drvctx.ctl_dip != NULL)
    {
        mutex_exit(&nvlink_drvctx.lock);
        return;
    }

    if (ddi_create_minor_node(dip, NVLINK_DEVICE_NAME, S_IFCHR,
            NV_MINOR_NVLINK, NV_DDI_NT_NVIDIA, 0) != DDI_SUCCESS)
    {
        dev_err(dip, CE_WARN, "failed to create the %s node",
            NVLINK_DEVICE_NAME);
        goto fail;
    }

    if (nvlink_cap_init(NVLINK_CAP_ROOT) != 0)
    {
        nv_printf(NV_DBG_ERRORS, "nvidia-nvlink: Unable to create capability\n");
        goto fail_minor;
    }

    if (nvswitch_ctl_attach(dip) != DDI_SUCCESS)
        goto fail_cap;

    (void) ddi_prop_update_int(DDI_DEV_T_NONE, dip,
        NVLINK_DEVICE_FILE_MODE_PROP, NVLINK_DEVICE_FILE_MODE);

    nvlink_drvctx.ctl_dip = dip;
    mutex_exit(&nvlink_drvctx.lock);
    return;

fail_cap:
    nvlink_cap_exit();
fail_minor:
    ddi_remove_minor_node(dip, NVLINK_DEVICE_NAME);
fail:
    mutex_exit(&nvlink_drvctx.lock);
}

int
nvlink_ctl_detach(dev_info_t *dip)
{
    mutex_enter(&nvlink_drvctx.lock);

    if (nvlink_drvctx.ctl_dip != dip)
    {
        mutex_exit(&nvlink_drvctx.lock);
        return (DDI_SUCCESS);
    }

    if (nvlink_drvctx.opened || nvswitch_ctl_detach(dip) != DDI_SUCCESS)
    {
        mutex_exit(&nvlink_drvctx.lock);
        return (DDI_FAILURE);
    }

    nvlink_cap_exit();
    ddi_remove_minor_node(dip, NVLINK_DEVICE_NAME);
    (void) ddi_prop_remove(DDI_DEV_T_NONE, dip, NVLINK_DEVICE_FILE_MODE_PROP);
    nvlink_drvctx.ctl_dip = NULL;

    mutex_exit(&nvlink_drvctx.lock);
    return (DDI_SUCCESS);
}

/*
 * OS services for the NVLink core library.
 */
void *
nvlink_malloc(NvLength size)
{
    void *ptr;

    return (os_alloc_mem(&ptr, size) == NV_OK) ? ptr : NULL;
}

void
nvlink_free(void *ptr)
{
    os_free_mem(ptr);
}

char *
nvlink_strcpy(char *dest, const char *src)
{
    return strcpy(dest, src);
}

int
nvlink_strcmp(const char *dest, const char *src)
{
    return strcmp(dest, src);
}

NvLength
nvlink_strlen(const char *s)
{
    return strlen(s);
}

void *
nvlink_memset(void *dest, int value, NvLength size)
{
    return memset(dest, value, size);
}

void *
nvlink_memcpy(void *dest, const void *src, NvLength size)
{
    return memcpy(dest, src, size);
}

/*
 * Library locks.  src/common/nvlink/kernel/nvlink/nvlink_lock.c enables the
 * top-level lock only for NV_LINUX; these back it once NV_SUNOS is added.
 * Like Linux these are semaphores without an owner.
 */
void *
nvlink_allocLock(void)
{
    ksema_t *sema;

    sema = nvlink_malloc(sizeof (*sema));
    if (sema == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "nvidia-nvlink: Failed to allocate sema!\n");
        return NULL;
    }

    sema_init(sema, 1, NULL, SEMA_DRIVER, NULL);
    return sema;
}

void
nvlink_acquireLock(void *hLock)
{
    sema_p(hLock);
}

void
nvlink_releaseLock(void *hLock)
{
    sema_v(hLock);
}

void
nvlink_freeLock(void *hLock)
{
    if (hLock == NULL)
        return;

    sema_destroy(hLock);
    nvlink_free(hLock);
}

NvBool
nvlink_isLockOwner(void *hLock)
{
    return NV_TRUE;
}

void
nvlink_sleep(unsigned int ms)
{
    if (!nv_may_sleep() && ms > NV_MAX_ISR_DELAY_MS)
    {
        if (nvlink_log_ratelimit())
        {
            nv_printf(NV_DBG_ERRORS, "nvidia-nvlink: NVLink: requested sleep "
                "duration %u msec exceeded %u msec\n", ms, NV_MAX_ISR_DELAY_MS);
        }
        return;
    }

    (void) os_delay(ms);
}

void
nvlink_assert(int cond)
{
    if (cond == 0 && nvlink_log_ratelimit())
        nv_printf(NV_DBG_ERRORS, "nvidia-nvlink: NVLink: Assertion failed!\n");
}

NvU64
nvlink_get_platform_time(void)
{
    return (NvU64)gethrtime();
}
