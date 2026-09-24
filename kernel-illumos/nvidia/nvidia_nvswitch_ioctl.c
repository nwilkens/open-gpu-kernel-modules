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
 * NVSwitch character devices: /dev/nvidia-nvswitchctl and
 * /dev/nvidia-nvswitchN, their ioctls and client events.
 */

#include "nvidia_nvswitch.h"
#include "ioctl_nvswitch.h"

/*
 * Client events.  The library notifies with device_lock held, including
 * from chpoll(9E), where pollwakeup(9F) must not be called, so wakeups are
 * delivered from the bottom-half taskq.
 */
static void
nvswitch_wake_task(void *arg)
{
    nvswitch_dev_t *dev = arg;
    nvswitch_file_private_t *private;
    NvBool wake;

    atomic_swap_32(&dev->wake_pending, 0);

    mutex_enter(&dev->files_lock);
    for (private = list_head(&dev->files); private != NULL;
         private = list_next(&dev->files, private))
    {
        mutex_enter(&private->lock);
        wake = private->wake_pending;
        private->wake_pending = NV_FALSE;
        mutex_exit(&private->lock);

        if (wake)
            pollwakeup(&private->pollhead, POLLIN | POLLPRI);
    }
    mutex_exit(&dev->files_lock);
}

NvlStatus
nvswitch_os_notify_client_event(void *osHandle, void *osPrivate, NvU32 eventId)
{
    nvswitch_file_private_t *private = osPrivate;
    nvswitch_dev_t *dev;

    if (private == NULL || (dev = private->dev) == NULL)
        return -NVL_BAD_ARGS;

    mutex_enter(&private->lock);
    private->event_pending = NV_TRUE;
    private->wake_pending = NV_TRUE;
    mutex_exit(&private->lock);

    if (atomic_cas_32(&dev->wake_pending, 0, 1) == 0)
    {
        taskq_dispatch_ent(dev->bh_tq, nvswitch_wake_task, dev, TQ_NOSLEEP,
            &dev->wake_ent);
    }

    return NVL_SUCCESS;
}

NvlStatus
nvswitch_os_add_client_event(void *osHandle, void *osPrivate, NvU32 eventId)
{
    nvswitch_file_private_t *private = osPrivate;
    NvlStatus status = NVL_SUCCESS;

    if (private == NULL)
        return -NVL_BAD_ARGS;

    mutex_enter(&private->lock);
    if (private->num_events >= NVSWITCH_MAX_CLIENT_EVENTS)
        status = -NVL_NO_MEM;
    else
        private->num_events++;
    mutex_exit(&private->lock);

    return status;
}

NvlStatus
nvswitch_os_remove_client_event(void *osHandle, void *osPrivate)
{
    nvswitch_file_private_t *private = osPrivate;

    if (private != NULL)
    {
        mutex_enter(&private->lock);
        private->num_events = 0;
        mutex_exit(&private->lock);
    }

    return NVL_SUCCESS;
}

/* osPrivate must stay our own pointer, never a user-supplied descriptor. */
NvlStatus
nvswitch_os_get_supported_register_events_params(NvBool *many_events,
    NvBool *os_descriptor)
{
    *many_events = NV_FALSE;
    *os_descriptor = NV_FALSE;
    return NVL_SUCCESS;
}

NvlStatus
nvswitch_os_acquire_fabric_mgmt_cap(void *osPrivate, NvU64 capDescriptor)
{
    nvswitch_file_private_t *private = osPrivate;
    file_t *fp, *old;

    if (private == NULL)
        return -NVL_BAD_ARGS;

    fp = nvlink_fabric_mgmt_hold((int)capDescriptor);
    if (fp == NULL)
        return -NVL_ERR_OPERATING_SYSTEM;

    old = atomic_swap_ptr(&private->fabric_mgmt, fp);
    if (old != NULL)
        nvlink_fabric_mgmt_rele(old);

    return NVL_SUCCESS;
}

int
nvswitch_os_is_fabric_manager(void *osPrivate)
{
    nvswitch_file_private_t *private = osPrivate;

    return (private != NULL && private->fabric_mgmt != NULL);
}

/*
 * Character device entry points.
 */
static nvswitch_file_private_t *
nvswitch_file_private_alloc(nvswitch_dev_t *dev)
{
    nvswitch_file_private_t *private;

    private = kmem_zalloc(sizeof (*private), KM_SLEEP);
    private->dev = dev;
    mutex_init(&private->lock, NULL, MUTEX_DRIVER, NULL);
    return (private);
}

int
nvswitch_node_open(nv_illumos_file_private_t *nvifp, NvU32 node,
    cred_t *credp)
{
    nvswitch_file_private_t *private;
    nvswitch_dev_t *dev;
    NvU32 index;
    int rc = 0;

    if (!nvswitch_drv.initialized)
        return (ENXIO);

    if (node == NV_MINOR_NVSWITCH_CTL)
    {
        if (sema_p_sig(&nvswitch_drv.driver_lock) != 0)
            return (EINTR);

        if (nvswitch_drv.ctl_dip == NULL)
        {
            rc = ENXIO;
        }
        else
        {
            nvifp->subsys_priv = nvswitch_file_private_alloc(NULL);
            nvswitch_drv.ctl_opens++;
        }

        sema_v(&nvswitch_drv.driver_lock);
        return (rc);
    }

    if (!NV_MINOR_IS_NVSWITCH(node))
        return (ENXIO);
    index = node - NV_MINOR_NVSWITCH_BASE;
    if (index >= NVSWITCH_DEVICE_INSTANCE_MAX)
        return (ENXIO);

    if (sema_p_sig(&nvswitch_drv.driver_lock) != 0)
        return (EINTR);

    nv_printf(NV_DBG_INFO, "nvidia-nvswitch%u: open\n", index);

    dev = nvswitch_drv.devs[index];
    if (dev == NULL || dev->unusable || nvswitch_is_device_blacklisted(dev))
    {
        rc = ENODEV;
        goto done;
    }

    private = nvswitch_file_private_alloc(dev);

    mutex_enter(&dev->files_lock);
    list_insert_tail(&dev->files, private);
    mutex_exit(&dev->files_lock);

    dev->ref_count++;
    nvifp->subsys_priv = private;

done:
    sema_v(&nvswitch_drv.driver_lock);
    return (rc);
}

void
nvswitch_node_close(nv_illumos_file_private_t *nvifp)
{
    nvswitch_file_private_t *private = nvifp->subsys_priv;
    nvswitch_dev_t *dev;
    file_t *fp;

    if (private == NULL)
        return;

    nvifp->subsys_priv = NULL;
    dev = private->dev;

    sema_p(&nvswitch_drv.driver_lock);

    if (dev != NULL)
    {
        nv_printf(NV_DBG_INFO, "%s: release\n", dev->name);

        sema_p(&dev->device_lock);
        (void) nvswitch_lib_remove_client_events(dev->lib_device, private);
        sema_v(&dev->device_lock);

        mutex_enter(&dev->files_lock);
        list_remove(&dev->files, private);
        mutex_exit(&dev->files_lock);

        dev->ref_count--;
    }
    else
    {
        nvswitch_drv.ctl_opens--;
    }

    sema_v(&nvswitch_drv.driver_lock);

    /* Nothing can reach private now; dev may already be detached. */
    fp = atomic_swap_ptr(&private->fabric_mgmt, NULL);
    if (fp != NULL)
        nvlink_fabric_mgmt_rele(fp);

    pollwakeup(&private->pollhead, POLLERR);
    pollhead_clean(&private->pollhead);
    mutex_destroy(&private->lock);
    kmem_free(private, sizeof (*private));
}

int
nvswitch_node_chpoll(nv_illumos_file_private_t *nvifp, short events,
    int anyyet, short *reventsp, struct pollhead **phpp)
{
    nvswitch_file_private_t *private = nvifp->subsys_priv;
    NVSWITCH_CLIENT_EVENT *client_event;
    nvswitch_dev_t *dev;
    short revents = 0;

    if (private == NULL)
        return (ENXIO);

    dev = private->dev;

    /* The control node has no poll method on Linux: always ready. */
    if (dev == NULL)
    {
        revents = events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
        goto done;
    }

    if (sema_p_sig(&dev->device_lock) != 0)
        return (EINTR);

    if (dev->unusable)
    {
        revents = POLLHUP;
    }
    else if (nvswitch_lib_get_client_event(dev->lib_device, private,
                 &client_event) != NVL_SUCCESS)
    {
        nv_printf(NV_DBG_INFO, "%s: no events registered for fd\n",
            dev->name);
        revents = POLLERR;
    }
    else if (events & (POLLIN | POLLPRI))
    {
        mutex_enter(&private->lock);
        if (private->event_pending)
        {
            revents = events & (POLLIN | POLLPRI);
            private->event_pending = NV_FALSE;
        }
        mutex_exit(&private->lock);
    }

    sema_v(&dev->device_lock);

done:
    *reventsp = revents;
    if ((revents == 0 && !anyyet) || (events & POLLET))
        *phpp = &private->pollhead;

    return (0);
}

/*
 * ioctls.  Arguments are copied in before and out after the locks are
 * held, so a slow user buffer cannot stall other clients.
 */
typedef struct
{
    void   *params;
    size_t  size;
} nvswitch_ioctl_state_t;

static int
nvswitch_ioctl_state_start(nvswitch_ioctl_state_t *state, uint32_t cmd,
    intptr_t arg, int mode)
{
    int rc;

    state->params = NULL;
    state->size = 0;

    if ((rc = nvlink_ioc_param_size(cmd, &state->size)) != 0 ||
        state->size == 0)
        return (rc);

    state->params = kmem_zalloc(state->size, KM_SLEEP);
    if (ddi_copyin((void *)arg, state->params, state->size, mode) != 0)
    {
        kmem_free(state->params, state->size);
        state->params = NULL;
        return (EFAULT);
    }

    return (0);
}

static int
nvswitch_ioctl_state_sync(nvswitch_ioctl_state_t *state, intptr_t arg,
    int mode)
{
    if (state->size == 0)
        return (0);

    return (ddi_copyout(state->params, (void *)arg, state->size, mode) != 0 ?
        EFAULT : 0);
}

static void
nvswitch_ioctl_state_cleanup(nvswitch_ioctl_state_t *state)
{
    if (state->params != NULL)
        kmem_free(state->params, state->size);
    state->params = NULL;
}

static int
nvswitch_device_ioctl(nvswitch_file_private_t *private, uint32_t cmd,
    intptr_t arg, int mode)
{
    nvswitch_dev_t *dev = private->dev;
    nvswitch_ioctl_state_t state;
    NvlStatus retval;
    int rc;

    if (NV_IOC_TYPE(cmd) != NVSWITCH_DEV_IO_TYPE)
        return (EINVAL);

    if ((rc = nvswitch_ioctl_state_start(&state, cmd, arg, mode)) != 0)
        return (rc);

    if (sema_p_sig(&dev->device_lock) != 0)
    {
        nvswitch_ioctl_state_cleanup(&state);
        return (EINTR);
    }

    if (dev->unusable)
    {
        nv_printf(NV_DBG_INFO, "%s: a stale fd detected\n", dev->name);
        rc = ENODEV;
    }
    else if (nvswitch_is_device_blacklisted(dev))
    {
        nv_printf(NV_DBG_INFO, "%s: ioctl attempted on blacklisted device\n",
            dev->name);
        rc = ENODEV;
    }
    else
    {
        retval = nvswitch_lib_ctrl(dev->lib_device, NV_IOC_NR(cmd),
            state.params, state.size, private);
        rc = nvswitch_map_status(retval);
    }

    sema_v(&dev->device_lock);

    if (rc == 0)
        rc = nvswitch_ioctl_state_sync(&state, arg, mode);

    nvswitch_ioctl_state_cleanup(&state);
    return (rc);
}

static int
nvswitch_ctl_check_version(NVSWITCH_CHECK_VERSION_PARAMS *p)
{
    NvlStatus retval;

    p->is_compatible = 0;
    p->user.version[NVSWITCH_VERSION_STRING_LENGTH - 1] = '\0';

    retval = nvswitch_lib_check_api_version(p->user.version,
        p->kernel.version, NVSWITCH_VERSION_STRING_LENGTH);
    if (retval == NVL_SUCCESS)
    {
        p->is_compatible = 1;
    }
    else if (retval == -NVL_ERR_NOT_SUPPORTED)
    {
        if (nvlink_log_ratelimit())
            cmn_err(CE_WARN, "nvidia-nvswitch: Version mismatch, "
                "kernel version %s user version %s",
                p->kernel.version, p->user.version);
    }
    else
    {
        return (nvswitch_map_status(retval));
    }

    return (0);
}

static int
nvswitch_ctl_check_version_v2(NVSWITCH_CHECK_VERSION_V2_PARAMS *p)
{
    NvlStatus retval;
    NvU32 i, u_major_len = 0, k_major_len = 0;

    p->is_compatible = 0;
    p->user.version[NVSWITCH_VERSION_STRING_LENGTH - 1] = '\0';

    if (p->cmd != NVSWITCH_CHECK_VERSION_CMD_STRICT &&
        p->cmd != NVSWITCH_CHECK_VERSION_CMD_RELAXED &&
        p->cmd != NVSWITCH_CHECK_VERSION_CMD_QUERY)
        return (EINVAL);

    retval = nvswitch_lib_check_api_version(p->user.version,
        p->kernel.version, NVSWITCH_VERSION_STRING_LENGTH);

    if (p->cmd == NVSWITCH_CHECK_VERSION_CMD_QUERY)
        return (0);

    if (retval == NVL_SUCCESS)
    {
        p->is_compatible = 1;
        return (0);
    }

    if (retval != -NVL_ERR_NOT_SUPPORTED)
        return (nvswitch_map_status(retval));

    if (p->cmd == NVSWITCH_CHECK_VERSION_CMD_RELAXED)
    {
        for (i = 0; i < NVSWITCH_VERSION_STRING_LENGTH &&
             p->user.version[i] != '\0' && p->user.version[i] != '.'; i++)
            u_major_len++;
        for (i = 0; i < NVSWITCH_VERSION_STRING_LENGTH &&
             p->kernel.version[i] != '\0' && p->kernel.version[i] != '.'; i++)
            k_major_len++;

        if (u_major_len == k_major_len &&
            strncmp(p->user.version, p->kernel.version, u_major_len) == 0)
        {
            p->is_compatible = 1;
            nv_printf(NV_DBG_INFO, "nvidia-nvswitch: Minor version mismatch "
                "tolerated (process: %s, pid: %d), kernel %s user %s\n",
                PTOU(curproc)->u_comm, (int)curproc->p_pid,
                p->kernel.version, p->user.version);
            return (0);
        }
    }

    if (nvlink_log_ratelimit())
    {
        cmn_err(CE_WARN, "nvidia-nvswitch: Version mismatch (process: %s, "
            "pid: %d), kernel version %s user version %s",
            PTOU(curproc)->u_comm, (int)curproc->p_pid,
            p->kernel.version, p->user.version);
    }

    return (0);
}

static void
nvswitch_ctl_get_devices(NVSWITCH_GET_DEVICES_PARAMS *p)
{
    nvswitch_dev_t *dev;
    int i, index = 0;

    for (i = 0; i < NVSWITCH_DEVICE_INSTANCE_MAX; i++)
    {
        if ((dev = nvswitch_drv.devs[i]) == NULL)
            continue;

        p->info[index].deviceInstance = dev->minor;
        p->info[index].pciDomain = 0;
        p->info[index].pciBus = dev->bus;
        p->info[index].pciDevice = dev->slot;
        p->info[index].pciFunction = dev->func;
        index++;
    }

    p->deviceCount = index;
}

static void
nvswitch_ctl_get_devices_v2(NVSWITCH_GET_DEVICES_V2_PARAMS *p)
{
    nvswitch_dev_t *dev;
    int i, index = 0;

    for (i = 0; i < NVSWITCH_DEVICE_INSTANCE_MAX; i++)
    {
        if ((dev = nvswitch_drv.devs[i]) == NULL)
            continue;

        p->info[index].deviceInstance = dev->minor;
        bcopy(&dev->uuid, &p->info[index].uuid, sizeof (dev->uuid));
        p->info[index].pciDomain = 0;
        p->info[index].pciBus = dev->bus;
        p->info[index].pciDevice = dev->slot;
        p->info[index].pciFunction = dev->func;
        p->info[index].physId = dev->phys_id;

        if (dev->lib_device != NULL)
        {
            sema_p(&dev->device_lock);
            (void) nvswitch_lib_read_fabric_state(dev->lib_device,
                &p->info[index].deviceState, &p->info[index].deviceReason,
                &p->info[index].driverState);
            p->info[index].bTnvlEnabled =
                nvswitch_lib_is_tnvl_enabled(dev->lib_device);
            sema_v(&dev->device_lock);
        }
        index++;
    }

    p->deviceCount = index;
}

static int
nvswitch_ctl_cmd_dispatch(unsigned int cmd, void *params, size_t size)
{
    switch (cmd)
    {
        case CTRL_NVSWITCH_CHECK_VERSION:
            if (size != sizeof (NVSWITCH_CHECK_VERSION_PARAMS))
                return (EINVAL);
            return (nvswitch_ctl_check_version(params));

        case CTRL_NVSWITCH_GET_DEVICES:
            if (size != sizeof (NVSWITCH_GET_DEVICES_PARAMS))
                return (EINVAL);
            nvswitch_ctl_get_devices(params);
            return (0);

        case CTRL_NVSWITCH_GET_DEVICES_V2:
            if (size != sizeof (NVSWITCH_GET_DEVICES_V2_PARAMS))
                return (EINVAL);
            nvswitch_ctl_get_devices_v2(params);
            return (0);

        case CTRL_NVSWITCH_CHECK_VERSION_V2:
            if (size != sizeof (NVSWITCH_CHECK_VERSION_V2_PARAMS))
                return (EINVAL);
            return (nvswitch_ctl_check_version_v2(params));

        default:
            return (EINVAL);
    }
}

static int
nvswitch_ctl_ioctl(uint32_t cmd, intptr_t arg, int mode)
{
    nvswitch_ioctl_state_t state;
    int rc;

    if (NV_IOC_TYPE(cmd) != NVSWITCH_CTL_IO_TYPE)
        return (EINVAL);

    if ((rc = nvswitch_ioctl_state_start(&state, cmd, arg, mode)) != 0)
        return (rc);

    if (sema_p_sig(&nvswitch_drv.driver_lock) != 0)
    {
        nvswitch_ioctl_state_cleanup(&state);
        return (EINTR);
    }

    rc = nvswitch_ctl_cmd_dispatch(NV_IOC_NR(cmd), state.params, state.size);

    sema_v(&nvswitch_drv.driver_lock);

    if (rc == 0)
        rc = nvswitch_ioctl_state_sync(&state, arg, mode);

    nvswitch_ioctl_state_cleanup(&state);
    return (rc);
}

int
nvswitch_node_ioctl(nv_illumos_file_private_t *nvifp, int cmd, intptr_t arg,
    int mode, cred_t *credp)
{
    nvswitch_file_private_t *private = nvifp->subsys_priv;

    if (private == NULL)
        return (ENXIO);

    if (private->dev == NULL)
        return (nvswitch_ctl_ioctl((uint32_t)cmd, arg, mode));

    return (nvswitch_device_ioctl(private, (uint32_t)cmd, arg, mode));
}
