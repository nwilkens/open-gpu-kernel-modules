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
 * illumos kernel interface layer for nvidia-modeset (NVKMS): the
 * nvidia_modeset pseudo driver behind /dev/nvidia-modeset and the NVKMS
 * interface for kernel clients.
 */

#include "nvidia_modeset_illumos.h"

#include <sys/stat.h>
#include <sys/file.h>
#include <sys/open.h>
#include <sys/cred.h>
#include <sys/poll.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/id_space.h>
#include <sys/vnode.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include "nvstatus.h"
#include "nvkms-ioctl.h"

/*************************************************************************
 * NVKMS uses a global lock, nvkms_lock, taken in the driver entry points
 * when calling into core NVKMS.  nvkms_pm_lock synchronizes clients with
 * suspend/resume.
 *************************************************************************/

static ksema_t nvkms_lock;
nvkms_rwsem_t nvkms_pm_lock;

int nvkms_lock_down(NvBool interruptible)
{
    if (!interruptible) {
        sema_p(&nvkms_lock);
        return 0;
    }
    return (sema_p_sig(&nvkms_lock) != 0) ? EINTR : 0;
}

void nvkms_lock_up(void)
{
    sema_v(&nvkms_lock);
}

nvkms_q_t nvkms_kthread_q;
static nvkms_q_t nvkms_deferred_close_kthread_q;

/*************************************************************************
 * The nvkms_per_open structure tracks data that is specific to a
 * single open.
 *************************************************************************/

struct nvkms_per_open {
    void *data;

    enum NvKmsClientType type;

    union {
        struct {
            struct {
                kmutex_t lock;
                NvBool available;
                struct pollhead pollhead;
            } events;
        } user;

        struct {
            struct {
                nvkms_q_item_t q_item;
            } events;
        } kernel;
    } u;

    nvkms_q_item_t deferred_close_q_item;
};

/* Live nvkms_per_open structures, including ones awaiting deferred close. */
static volatile uint32_t nvkms_popen_count;

/* Clone minor slots. */
typedef struct nvkms_minor {
    struct nvkms_per_open *popen;
} nvkms_minor_t;

static void *nvkms_softstate;
static id_space_t *nvkms_minors;
static dev_info_t *nvkms_dip;
static major_t nvkms_major = DDI_MAJOR_T_NONE;
static major_t nvkms_nvidia_major = DDI_MAJOR_T_NONE;

void
nvkms_event_queue_changed(nvkms_per_open_handle_t *pOpenKernel,
                          NvBool eventsAvailable)
{
    struct nvkms_per_open *popen = pOpenKernel;

    switch (popen->type) {
        case NVKMS_CLIENT_USER_SPACE:
            mutex_enter(&popen->u.user.events.lock);
            popen->u.user.events.available = eventsAvailable;
            mutex_exit(&popen->u.user.events.lock);

            if (eventsAvailable) {
                pollwakeup(&popen->u.user.events.pollhead,
                           POLLIN | POLLRDNORM | POLLPRI);
            }
            break;
        case NVKMS_CLIENT_KERNEL_SPACE:
            if (eventsAvailable) {
                (void) nvkms_q_schedule(
                    &nvkms_kthread_q,
                    &popen->u.kernel.events.q_item);
            }
            break;
    }
}

static void nvkms_suspend(NvU32 gpuId)
{
    nvKmsKapiSuspendResume(NV_TRUE /* suspend */);

    if (gpuId == 0) {
        nvkms_rwsem_down_write(&nvkms_pm_lock);
    }

    (void) nvkms_lock_down(NV_FALSE);
    nvKmsSuspend(gpuId);
    nvkms_lock_up();
}

static void nvkms_resume(NvU32 gpuId)
{
    (void) nvkms_lock_down(NV_FALSE);
    nvKmsResume(gpuId);
    nvkms_lock_up();

    if (gpuId == 0) {
        nvkms_rwsem_up_write(&nvkms_pm_lock);
    }

    nvKmsKapiSuspendResume(NV_FALSE /* suspend */);
}

static void nvkms_remove(NvU32 gpuId)
{
    nvKmsKapiRemove(gpuId);

    // Eventually, this function should also terminate all NVKMS clients and
    // free the NVDevEvoRec. Until that is implemented, all NVKMS clients must
    // be closed before a device is removed.
}

static void nvkms_probe(const nv_gpu_info_t *gpu_info)
{
    nvKmsKapiProbe(gpu_info);
}

/*************************************************************************
 * Interface with resman.
 *************************************************************************/

nvidia_modeset_rm_ops_t nvkms_rm_ops;
static const nvidia_modeset_callbacks_t nvkms_rm_callbacks = {
    .suspend = nvkms_suspend,
    .resume  = nvkms_resume,
    .remove  = nvkms_remove,
    .probe   = nvkms_probe,
};

static int nvkms_alloc_rm(void)
{
    NV_STATUS nvstatus;

    nvkms_rm_ops.version_string = NV_VERSION_STRING;

    nvstatus = nvidia_get_rm_ops(&nvkms_rm_ops);

    if (nvstatus != NV_OK) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "Version mismatch: "
            "nvidia(%s) nvidia_modeset(%s)",
            (nvkms_rm_ops.version_string != NULL) ?
                nvkms_rm_ops.version_string : "?", NV_VERSION_STRING);
        return EINVAL;
    }

    return 0;
}

/*
 * Registered only once everything the callbacks touch is set up, and
 * unregistered before teardown.  The nvidia driver must not return from
 * set_callbacks(NULL) while a callback is still running.
 */
static int nvkms_register_callbacks(void)
{
    int ret = nvkms_rm_ops.set_callbacks(&nvkms_rm_callbacks);

    if (ret < 0) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "Failed to register callbacks");
        return -ret;
    }

    return 0;
}

static void nvkms_unregister_callbacks(void)
{
    (void) nvkms_rm_ops.set_callbacks(NULL);
}

/*************************************************************************
 * File descriptor helpers.
 *************************************************************************/

/*
 * Return the vnode's device if fd refers to a character device, else
 * NODEV.  The caller must releasef(fd) if this returns anything but NODEV.
 */
static dev_t nvkms_getf_chardev(int fd)
{
    file_t *fp;
    vnode_t *vp;

    if (fd < 0 || (fp = getf(fd)) == NULL) {
        return NODEV;
    }

    vp = fp->f_vnode;
    if (vp == NULL || vp->v_type != VCHR) {
        releasef(fd);
        return NODEV;
    }

    return vp->v_rdev;
}

NvBool nvkms_fd_is_nvidia_chardev(int fd)
{
    dev_t rdev = nvkms_getf_chardev(fd);
    NvBool ret;

    if (rdev == NODEV) {
        return NV_FALSE;
    }

    ret = (nvkms_nvidia_major != DDI_MAJOR_T_NONE &&
           getmajor(rdev) == nvkms_nvidia_major);

    releasef(fd);

    return ret;
}

void* nvkms_get_per_open_data(int fd)
{
    dev_t rdev = nvkms_getf_chardev(fd);
    void *data = NULL;

    if (rdev == NODEV) {
        return NULL;
    }

    if (getmajor(rdev) == nvkms_major && getminor(rdev) != 0) {
        nvkms_minor_t *slot =
            ddi_get_soft_state(nvkms_softstate, (int)getminor(rdev));

        if (slot != NULL && slot->popen != NULL &&
            slot->popen->type == NVKMS_CLIENT_USER_SPACE) {
            data = slot->popen->data;
        }
    }

    /*
     * The file stays open while fd is held, and core NVKMS holds nvkms_lock
     * here, which keeps nvkms_dev_close() => nvKmsClose() from freeing data
     * after releasef().
     */
    releasef(fd);

    return data;
}

/*************************************************************************
 * Common to both user-space and kapi NVKMS interfaces
 *************************************************************************/

static void nvkms_kapi_event_kthread_q_callback(void *arg)
{
    struct NvKmsKapiDevice *device = arg;

    nvKmsKapiHandleEventQueueChange(device);
}

static struct nvkms_per_open *nvkms_open_common(enum NvKmsClientType type,
                                         struct NvKmsKapiDevice *device,
                                         NvBool interruptible,
                                         int *status)
{
    struct nvkms_per_open *popen = NULL;

    popen = nvkms_alloc(sizeof(*popen), NV_TRUE);

    if (popen == NULL) {
        *status = ENOMEM;
        return NULL;
    }

    popen->type = type;

    switch (popen->type) {
        case NVKMS_CLIENT_USER_SPACE:
            mutex_init(&popen->u.user.events.lock, NULL, MUTEX_DRIVER, NULL);
            break;
        case NVKMS_CLIENT_KERNEL_SPACE:
            nvkms_q_item_init(&popen->u.kernel.events.q_item,
                              nvkms_kapi_event_kthread_q_callback,
                              device);
            break;
    }

    *status = nvkms_lock_down(interruptible);
    if (*status != 0) {
        goto failed;
    }

    popen->data = nvKmsOpen(ddi_get_pid(), type, popen);

    nvkms_lock_up();

    if (popen->data == NULL) {
        *status = EPERM;
        goto failed;
    }

    atomic_inc_32(&nvkms_popen_count);
    *status = 0;

    return popen;

failed:
    if (popen->type == NVKMS_CLIENT_USER_SPACE) {
        mutex_destroy(&popen->u.user.events.lock);
    }
    nvkms_free(popen, sizeof(*popen));

    return NULL;
}

/* Call with nvkms_pm_lock held for reading. */
static void nvkms_close_pm_locked(struct nvkms_per_open *popen)
{
    /*
     * Don't use an interruptible wait: we need to free resources
     * during close, so we have no choice but to wait to take the
     * lock.
     */

    (void) nvkms_lock_down(NV_FALSE);

    nvKmsClose(popen->data);

    popen->data = NULL;

    nvkms_lock_up();
}

/*
 * Call without nvkms_pm_lock: the flush below waits for queued timers, which
 * block on nvkms_pm_lock behind a waiting suspend.
 */
static void nvkms_close_finish(struct nvkms_per_open *popen)
{
    if (popen->type == NVKMS_CLIENT_KERNEL_SPACE) {
        /*
         * Flush any outstanding nvkms_kapi_event_kthread_q_callback() work
         * items before freeing popen.
         *
         * Note that this must be done after the nvKmsClose() call, to
         * guarantee that no more nvkms_kapi_event_kthread_q_callback() work
         * items get scheduled.
         *
         * Also, note that though popen->data is freed, any subsequent
         * nvkms_kapi_event_kthread_q_callback()'s for this popen should be
         * safe: if any nvkms_kapi_event_kthread_q_callback()-initiated work
         * attempts to call back into NVKMS, the popen->data==NULL check in
         * nvkms_ioctl_common() should reject the request.
         */

        nvkms_q_flush(&nvkms_kthread_q);
    } else {
        mutex_destroy(&popen->u.user.events.lock);
    }

    nvkms_free(popen, sizeof(*popen));

    atomic_dec_32(&nvkms_popen_count);
}

static void nvkms_close_pm_unlocked(void *data)
{
    struct nvkms_per_open *popen = data;

    nvkms_rwsem_down_read(&nvkms_pm_lock);

    nvkms_close_pm_locked(popen);

    nvkms_rwsem_up_read(&nvkms_pm_lock);

    nvkms_close_finish(popen);
}

static void nvkms_close_popen(struct nvkms_per_open *popen)
{
    if (nvkms_rwsem_down_read_trylock(&nvkms_pm_lock)) {
        nvkms_close_pm_locked(popen);
        nvkms_rwsem_up_read(&nvkms_pm_lock);
        nvkms_close_finish(popen);
    } else {
        nvkms_q_item_init(&popen->deferred_close_q_item,
                          nvkms_close_pm_unlocked,
                          popen);
        nvkms_queue_work(&nvkms_deferred_close_kthread_q,
                         &popen->deferred_close_q_item);
    }
}

/*
 * Returns 0 or an illumos errno.  mode is the ioctl(9E) mode for user
 * clients (only FKIOCTL is used) and FKIOCTL for kernel clients.
 */
static int nvkms_ioctl_common
(
    struct nvkms_per_open *popen,
    NvU32 cmd, NvU64 address, const size_t size,
    NvBool interruptible, int mode
)
{
    NvBool ret;
    int status;

    status = nvkms_lock_down(interruptible);
    if (status != 0) {
        return status;
    }

    if (popen->data != NULL) {
        nvkms_copy_mode_enter(mode);
        ret = nvKmsIoctl(popen->data, cmd, address, size);
        nvkms_copy_mode_exit();
    } else {
        ret = NV_FALSE;
    }

    nvkms_lock_up();

    return ret ? 0 : EPERM;
}

/*************************************************************************
 * NVKMS interface for kernel space NVKMS clients like KAPI
 *************************************************************************/

struct nvkms_per_open* nvkms_open_from_kapi
(
    struct NvKmsKapiDevice *device
)
{
    int status = 0;
    struct nvkms_per_open *ret;

    nvkms_rwsem_down_read(&nvkms_pm_lock);
    ret = nvkms_open_common(NVKMS_CLIENT_KERNEL_SPACE,
                            device,
                            NV_FALSE /* interruptible */,
                            &status);
    nvkms_rwsem_up_read(&nvkms_pm_lock);

    return ret;
}

void nvkms_close_from_kapi(struct nvkms_per_open *popen)
{
    nvkms_close_pm_unlocked(popen);
}

NvBool nvkms_ioctl_from_kapi_try_pmlock
(
    struct nvkms_per_open *popen,
    NvU32 cmd, void *params_address, const size_t param_size
)
{
    NvBool ret;

    // XXX PM lock must be allowed to fail, see bug 4432810.
    if (!nvkms_rwsem_down_read_trylock(&nvkms_pm_lock)) {
        return NV_FALSE;
    }

    ret = nvkms_ioctl_common(popen,
                             cmd,
                             (NvU64)(NvUPtr)params_address, param_size,
                             NV_FALSE /* interruptible */, FKIOCTL) == 0;
    nvkms_rwsem_up_read(&nvkms_pm_lock);

    return ret;
}

NvBool nvkms_ioctl_from_kapi
(
    struct nvkms_per_open *popen,
    NvU32 cmd, void *params_address, const size_t param_size
)
{
    NvBool ret;

    nvkms_rwsem_down_read(&nvkms_pm_lock);
    ret = nvkms_ioctl_common(popen,
                             cmd,
                             (NvU64)(NvUPtr)params_address, param_size,
                             NV_FALSE /* interruptible */, FKIOCTL) == 0;
    nvkms_rwsem_up_read(&nvkms_pm_lock);

    return ret;
}

/*************************************************************************
 * Character device entry points.
 *************************************************************************/

static struct nvkms_per_open *nvkms_popen_from_dev(dev_t dev)
{
    nvkms_minor_t *slot;

    if (getminor(dev) == 0) {
        return NULL;
    }

    slot = ddi_get_soft_state(nvkms_softstate, (int)getminor(dev));

    return (slot != NULL) ? slot->popen : NULL;
}

static int nvkms_dev_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
    struct nvkms_per_open *popen;
    nvkms_minor_t *slot;
    id_t minor;
    int status;

    if (otyp != OTYP_CHR) {
        return EINVAL;
    }

    /* Only the base node may be opened; each open gets a clone minor. */
    if (getminor(*devp) != 0) {
        return ENXIO;
    }

    minor = id_allocff_nosleep(nvkms_minors);
    if (minor == -1) {
        return ENOSPC;
    }

    if (ddi_soft_state_zalloc(nvkms_softstate, (int)minor) != DDI_SUCCESS) {
        id_free(nvkms_minors, minor);
        return ENOMEM;
    }
    slot = ddi_get_soft_state(nvkms_softstate, (int)minor);

    status = nvkms_rwsem_down_read_sig(&nvkms_pm_lock);
    if (status != 0) {
        goto failed;
    }

    popen = nvkms_open_common(NVKMS_CLIENT_USER_SPACE,
                              NULL,
                              NV_TRUE /* interruptible */,
                              &status);

    nvkms_rwsem_up_read(&nvkms_pm_lock);

    if (popen == NULL) {
        goto failed;
    }

    slot->popen = popen;
    *devp = makedevice(getmajor(*devp), (minor_t)minor);

    return 0;

failed:
    ddi_soft_state_free(nvkms_softstate, (int)minor);
    id_free(nvkms_minors, minor);

    return status;
}

static int nvkms_dev_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
    minor_t minor = getminor(dev);
    struct nvkms_per_open *popen = nvkms_popen_from_dev(dev);

    if (popen == NULL) {
        return ENXIO;
    }

    pollwakeup(&popen->u.user.events.pollhead, POLLERR);
    pollhead_clean(&popen->u.user.events.pollhead);

    ddi_soft_state_free(nvkms_softstate, (int)minor);
    id_free(nvkms_minors, (id_t)minor);

    nvkms_close_popen(popen);

    return 0;
}

static int nvkms_dev_ioctl(dev_t dev, int cmd, intptr_t arg, int mode,
                           cred_t *credp, int *rvalp)
{
    struct NvKmsIoctlParams params;
    struct nvkms_per_open *popen = nvkms_popen_from_dev(dev);
    int status;

    if ((popen == NULL) || (popen->data == NULL)) {
        return EINVAL;
    }

    /*
     * The only supported ioctl is NVKMS_IOCTL_CMD.  Matching the whole
     * command also fixes its encoded size at sizeof(struct
     * NvKmsIoctlParams); nvKmsIoctl() rejects any params.size that differs
     * from the size of params.cmd's structure before touching memory.
     */
    if ((unsigned int)cmd != (unsigned int)NVKMS_IOCTL_IOWR) {
        return ENOTTY;
    }

    if (ddi_copyin((void *)arg, &params, sizeof(params), mode) != 0) {
        return EFAULT;
    }

    status = nvkms_rwsem_down_read_sig(&nvkms_pm_lock);
    if (status != 0) {
        return status;
    }

    status = nvkms_ioctl_common(popen,
                                params.cmd,
                                params.address,
                                params.size,
                                NV_TRUE /* interruptible */,
                                mode);

    nvkms_rwsem_up_read(&nvkms_pm_lock);

    *rvalp = 0;

    return status;
}

static int nvkms_dev_chpoll(dev_t dev, short events, int anyyet,
                            short *reventsp, struct pollhead **phpp)
{
    struct nvkms_per_open *popen = nvkms_popen_from_dev(dev);
    short revents = 0;

    if ((popen == NULL) || (popen->data == NULL)) {
        return ENXIO;
    }

    mutex_enter(&popen->u.user.events.lock);

    if (popen->u.user.events.available) {
        revents = events & (POLLIN | POLLRDNORM | POLLPRI);
    }

    *reventsp = revents;
    if ((revents == 0 && !anyyet) || (events & POLLET)) {
        *phpp = &popen->u.user.events.pollhead;
    }

    mutex_exit(&popen->u.user.events.lock);

    return 0;
}

/*************************************************************************
 * Device configuration.
 *************************************************************************/

static int nvkms_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg,
                         void **result)
{
    switch (cmd) {
    case DDI_INFO_DEVT2DEVINFO:
        if (nvkms_dip == NULL) {
            return DDI_FAILURE;
        }
        *result = nvkms_dip;
        return DDI_SUCCESS;
    case DDI_INFO_DEVT2INSTANCE:
        *result = (void *)0;
        return DDI_SUCCESS;
    default:
        return DDI_FAILURE;
    }
}

static int nvkms_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
    switch (cmd) {
    case DDI_ATTACH:
        break;
    case DDI_RESUME:
        /* NVKMS suspend/resume is driven by the nvidia driver callbacks. */
        return DDI_SUCCESS;
    default:
        return DDI_FAILURE;
    }

    if (ddi_get_instance(dip) != 0 || nvkms_dip != NULL) {
        return DDI_FAILURE;
    }

    if (ddi_create_minor_node(dip, NVKMS_MINOR_NAME, S_IFCHR, 0,
                              DDI_PSEUDO, 0) != DDI_SUCCESS) {
        return DDI_FAILURE;
    }

    nvkms_major = ddi_driver_major(dip);
    nvkms_dip = dip;
    ddi_report_dev(dip);

    return DDI_SUCCESS;
}

static int nvkms_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
    switch (cmd) {
    case DDI_DETACH:
        break;
    case DDI_SUSPEND:
        return DDI_SUCCESS;
    default:
        return DDI_FAILURE;
    }

    ddi_remove_minor_node(dip, NULL);
    nvkms_dip = NULL;

    return DDI_SUCCESS;
}

static struct cb_ops nvkms_cb_ops = {
    .cb_open        = nvkms_dev_open,
    .cb_close       = nvkms_dev_close,
    .cb_strategy    = nodev,
    .cb_print       = nodev,
    .cb_dump        = nodev,
    .cb_read        = nodev,
    .cb_write       = nodev,
    .cb_ioctl       = nvkms_dev_ioctl,
    .cb_devmap      = nodev,
    .cb_mmap        = nodev,
    .cb_segmap      = nodev,
    .cb_chpoll      = nvkms_dev_chpoll,
    .cb_prop_op     = ddi_prop_op,
    .cb_str         = NULL,
    .cb_flag        = D_NEW | D_MP,
    .cb_rev         = CB_REV,
    .cb_aread       = nodev,
    .cb_awrite      = nodev,
};

static struct dev_ops nvkms_dev_ops = {
    .devo_rev       = DEVO_REV,
    .devo_refcnt    = 0,
    .devo_getinfo   = nvkms_getinfo,
    .devo_identify  = nulldev,
    .devo_probe     = nulldev,
    .devo_attach    = nvkms_attach,
    .devo_detach    = nvkms_detach,
    .devo_reset     = nodev,
    .devo_cb_ops    = &nvkms_cb_ops,
    .devo_bus_ops   = NULL,
    .devo_power     = NULL,
    .devo_quiesce   = ddi_quiesce_not_needed,
};

static struct modldrv nvkms_modldrv = {
    &mod_driverops,
    "NVIDIA modeset " NV_VERSION_STRING,
    &nvkms_dev_ops,
};

static struct modlinkage nvkms_modlinkage = {
    MODREV_1,
    { &nvkms_modldrv, NULL }
};

/*************************************************************************
 * Module loading support code.
 *************************************************************************/

static void nvkms_teardown(void)
{
    nvkms_unregister_callbacks();

    (void) nvkms_lock_down(NV_FALSE);
    nvKmsModuleUnload();
    nvkms_lock_up();

    /*
     * At this point, any pending tasks should be marked canceled, but
     * we still need to drain them, so that nvkms_kthread_q_callback() doesn't
     * get called after the module is unloaded.
     */
    nvkms_cancel_timers();

    nvkms_q_stop(&nvkms_deferred_close_kthread_q);
    nvkms_q_stop(&nvkms_kthread_q);

    nvkms_timers_fini();
    nvkms_rwsem_destroy(&nvkms_pm_lock);
    sema_destroy(&nvkms_lock);

    if (nvkms_param_malloc_verbose) {
        cmn_err(CE_CONT, "!" NVKMS_LOG_PREFIX "Total allocations: %u\n",
            nvkms_alloc_called_count);
    }
}

static int nvkms_setup(void)
{
    int ret;

    nvkms_alloc_called_count = 0;
    nvkms_popen_count = 0;
    nvkms_nvidia_major = ddi_name_to_major(NVKMS_NVIDIA_DRIVER_NAME);

    sema_init(&nvkms_lock, 1, NULL, SEMA_DRIVER, NULL);
    nvkms_rwsem_init(&nvkms_pm_lock);
    nvkms_timers_init();

    ret = nvkms_alloc_rm();
    if (ret != 0) {
        goto fail_rm;
    }

    ret = nvkms_q_init(&nvkms_kthread_q, "nvidia_modeset_kthread_q");
    if (ret != 0) {
        goto fail_rm;
    }

    ret = nvkms_q_init(&nvkms_deferred_close_kthread_q,
                       "nvidia_modeset_deferred_close_kthread_q");
    if (ret != 0) {
        goto fail_deferred_close_kthread;
    }

    (void) nvkms_lock_down(NV_FALSE);
    if (!nvKmsModuleLoad()) {
        nvkms_lock_up();
        ret = ENOMEM;
        goto fail_module_load;
    }
    nvkms_lock_up();

    nvkms_read_config_file();

    ret = nvkms_register_callbacks();
    if (ret != 0) {
        (void) nvkms_lock_down(NV_FALSE);
        nvKmsModuleUnload();
        nvkms_lock_up();
        nvkms_cancel_timers();
        goto fail_module_load;
    }

    return 0;

fail_module_load:
    nvkms_q_stop(&nvkms_deferred_close_kthread_q);
fail_deferred_close_kthread:
    nvkms_q_stop(&nvkms_kthread_q);
fail_rm:
    nvkms_timers_fini();
    nvkms_rwsem_destroy(&nvkms_pm_lock);
    sema_destroy(&nvkms_lock);

    return ret;
}

int _init(void)
{
    int ret;

    ret = ddi_soft_state_init(&nvkms_softstate, sizeof(nvkms_minor_t), 0);
    if (ret != 0) {
        return ret;
    }

    nvkms_minors = id_space_create("nvidia_modeset_minors", 1,
                                   L_MAXMIN32 + 1);
    if (nvkms_minors == NULL) {
        ddi_soft_state_fini(&nvkms_softstate);
        return ENOMEM;
    }

    ret = nvkms_setup();
    if (ret != 0) {
        goto fail;
    }

    ret = mod_install(&nvkms_modlinkage);
    if (ret != 0) {
        nvkms_teardown();
        goto fail;
    }

    return 0;

fail:
    id_space_destroy(nvkms_minors);
    ddi_soft_state_fini(&nvkms_softstate);
    return ret;
}

int _fini(void)
{
    int ret;

    if (nvkms_popen_count != 0) {
        return EBUSY;
    }

    ret = mod_remove(&nvkms_modlinkage);
    if (ret != 0) {
        return ret;
    }

    nvkms_teardown();

    id_space_destroy(nvkms_minors);
    ddi_soft_state_fini(&nvkms_softstate);

    return 0;
}

int _info(struct modinfo *modinfop)
{
    return mod_info(&nvkms_modlinkage, modinfop);
}
