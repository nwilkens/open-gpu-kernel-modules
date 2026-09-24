/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * illumos kernel interface layer for nvidia-uvm: the nvidia_uvm pseudo
 * driver behind /dev/nvidia-uvm and /dev/nvidia-uvm-tools.
 *
 * Each open(9E) clones a minor and gets a struct linux_file, which the UVM
 * file_operations registered through cdev_add() then drive.  The file is
 * reference counted: the open, every seg_nvuvm segment mapping it, fget()
 * and each entry point in progress hold a reference, and the last one
 * calls the UVM release routine.  UVM starts on the first open, because RM
 * is initialized only when the first nvidia instance attaches.
 */

#include "uvm_illumos.h"
#include "uvm_illumos_mem.h"
#include "uvm_seg.h"

#include <sys/stat.h>
#include <sys/file.h>
#include <sys/open.h>
#include <sys/cred.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/id_space.h>
#include <sys/vnode.h>
#include <sys/model.h>
#include <sys/policy.h>

#include "nvtypes.h"

/* nvidia.kmod */
extern NvBool nv_rm_is_initialized(void);

/* kernel-open/nvidia-uvm/uvm_common.c */
extern int uvm_enable_builtin_tests;

/* kernel-open/nvidia-uvm/uvm.c, through the module_init/exit macros */
extern int nv_uvm_module_init(void);
extern void nv_uvm_module_exit(void);

typedef struct uvm_clone {
    struct linux_file *file;
} uvm_clone_t;

major_t             uvm_illumos_major = DDI_MAJOR_T_NONE;
static dev_info_t  *uvm_illumos_dip;

/* Clone minors; uvm_files_lock orders a lookup against close(9E). */
static void        *uvm_clone_state;
static id_space_t  *uvm_clone_ids;
static kmutex_t     uvm_files_lock;
static volatile uint_t uvm_file_live;

/* UVM file_operations by base minor, from cdev_add(). */
static const struct file_operations *uvm_fops[UVM_NODE_COUNT];

static kmutex_t     uvm_start_lock;
static boolean_t    uvm_started;

/* UVM copies the inode's a_ops into each file's address_space. */
static struct address_space uvm_inode_mapping;
static struct inode uvm_inode = {
    .i_mapping = &uvm_inode_mapping,
};

/*
 * Work that must not run in the caller's context: the last release of a
 * file whose last reference went away under an AS lock, and pollwakeup()
 * for a file whose wait queue UVM woke with its own locks held.
 */
static kmutex_t     uvm_worker_lock;
static kcondvar_t   uvm_worker_cv;
static list_t       uvm_release_list;
static list_t       uvm_poll_list;
static boolean_t    uvm_worker_exit;
static kt_did_t     uvm_worker_did;

static void
uvm_file_free(struct linux_file *f)
{
    if (f->f_acct != NULL)
        uvm_acct_owner_free(f->f_acct);
    mutex_destroy(&f->f_lock);
    kmem_free(f, sizeof (*f));
    atomic_dec_uint(&uvm_file_live);
}

/* Tearing down a VA space frees GPU memory through RM. */
static void
uvm_file_release_stk(void *arg)
{
    struct linux_file *f = arg;

    if (f->f_op->release != NULL)
        (void) f->f_op->release(f->f_inode, f);
}

static void
uvm_file_finish(void *arg)
{
    struct linux_file *f = arg;

    uvm_seg_file_release(f);
    uvm_file_free(f);
}

/*
 * uvm_release() may leave the VA space to its deferred release queue.
 * Until that is done the VA space still has CPU chunks and allocates page
 * tables for the file's owner, so the file and its charges go after it.
 */
static void
uvm_file_release(struct linux_file *f)
{
    uvm_stack_call(uvm_file_release_stk, f);
    uvm_after_deferred_release(uvm_file_finish, f);
}

void
uvm_file_hold(struct linux_file *f)
{
    mutex_enter(&f->f_lock);
    VERIFY(f->f_count > 0);
    f->f_count++;
    mutex_exit(&f->f_lock);
}

/* Returns B_TRUE when the caller dropped the last reference. */
static boolean_t
uvm_file_drop(struct linux_file *f)
{
    boolean_t last;

    mutex_enter(&f->f_lock);
    VERIFY(f->f_count > 0);
    last = (--f->f_count == 0);
    mutex_exit(&f->f_lock);

    return (last);
}

/* Drops a reference; the last release runs on the worker thread. */
void
uvm_file_rele(struct linux_file *f)
{
    if (!uvm_file_drop(f))
        return;

    mutex_enter(&uvm_worker_lock);
    list_insert_tail(&uvm_release_list, f);
    cv_signal(&uvm_worker_cv);
    mutex_exit(&uvm_worker_lock);
}

/* Holds the open file behind a clone dev_t of this driver, or NULL. */
struct linux_file *
uvm_file_hold_dev(dev_t dev)
{
    minor_t minor = getminor(dev);
    uint_t clone = UVM_MINOR_CLONE(minor);
    struct linux_file *f = NULL;
    uvm_clone_t *slot;

    if (getmajor(dev) != uvm_illumos_major || clone == 0 ||
        clone > UVM_CLONE_MAX)
        return (NULL);

    mutex_enter(&uvm_files_lock);
    slot = ddi_get_soft_state(uvm_clone_state, (int)clone);
    if (slot != NULL && slot->file != NULL && slot->file->f_minor == minor) {
        f = slot->file;
        uvm_file_hold(f);
    }
    mutex_exit(&uvm_files_lock);

    return (f);
}

void
uvm_file_queue_pollwakeup(struct linux_file *f)
{
    mutex_enter(&f->f_lock);
    if (f->f_count == 0 || !f->f_open || f->f_poll_queued) {
        mutex_exit(&f->f_lock);
        return;
    }
    f->f_count++;
    f->f_poll_queued = B_TRUE;
    mutex_exit(&f->f_lock);

    mutex_enter(&uvm_worker_lock);
    list_insert_tail(&uvm_poll_list, f);
    cv_signal(&uvm_worker_cv);
    mutex_exit(&uvm_worker_lock);
}

static void
uvm_worker_main(void *arg)
{
    struct linux_file *f;

    mutex_enter(&uvm_worker_lock);
    for (;;) {
        if ((f = list_remove_head(&uvm_poll_list)) != NULL) {
            mutex_exit(&uvm_worker_lock);

            mutex_enter(&f->f_lock);
            f->f_poll_queued = B_FALSE;
            mutex_exit(&f->f_lock);
            pollwakeup(&f->f_pollhead, POLLIN | POLLRDNORM);
            if (uvm_file_drop(f))
                uvm_file_release(f);

            mutex_enter(&uvm_worker_lock);
        } else if ((f = list_remove_head(&uvm_release_list)) != NULL) {
            mutex_exit(&uvm_worker_lock);
            uvm_file_release(f);
            mutex_enter(&uvm_worker_lock);
        } else if (uvm_worker_exit) {
            break;
        } else {
            cv_wait(&uvm_worker_cv, &uvm_worker_lock);
        }
    }
    mutex_exit(&uvm_worker_lock);

    thread_exit();
}

static void
uvm_module_init_stk(void *arg)
{
    *(int *)arg = nv_uvm_module_init();
}

/* Start UVM once RM is up; a failed start is retried by the next open. */
static int
uvm_start(void)
{
    int rc = 0;

    mutex_enter(&uvm_start_lock);
    if (!uvm_started) {
        if (!nv_rm_is_initialized() || uvm_illumos_dip == NULL) {
            rc = ENXIO;
        } else {
            uvm_params_apply(uvm_illumos_dip);
            uvm_stack_call(uvm_module_init_stk, &rc);
            if (rc == 0)
                uvm_started = B_TRUE;
            else
                rc = (rc < 0) ? -rc : EIO;
        }
    }
    mutex_exit(&uvm_start_lock);

    return (rc);
}

/*
 * The Linux file and character device interfaces that UVM calls.
 */
struct linux_file *
linux_fget(unsigned int fd)
{
    struct linux_file *f = NULL;
    file_t *fp;
    vnode_t *vp;

    if (fd > INT_MAX || (fp = getf((int)fd)) == NULL)
        return (NULL);

    vp = fp->f_vnode;
    if (vp != NULL && vp->v_type == VCHR &&
        getmajor(vp->v_rdev) == uvm_illumos_major)
        f = uvm_file_hold_dev(vp->v_rdev);

    releasef((int)fd);

    return (f);
}

void
linux_fput(struct linux_file *f)
{
    uvm_file_rele(f);
}

int
linux_cdev_add(struct cdev *cdev, dev_t dev, unsigned int count)
{
    minor_t node = getminor(dev);

    if (count != 1 || node >= UVM_NODE_COUNT || uvm_fops[node] != NULL)
        return (-EINVAL);

    cdev->dev = dev;
    cdev->count = count;
    uvm_fops[node] = cdev->ops;

    return (0);
}

void
linux_cdev_del(struct cdev *cdev)
{
    minor_t node = getminor(cdev->dev);

    if (node < UVM_NODE_COUNT)
        uvm_fops[node] = NULL;
}

/*
 * Character device entry points.
 */
static int
uvm_dev_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
    minor_t node = getminor(*devp);
    const struct file_operations *fops;
    struct linux_file *f;
    uvm_clone_t *slot;
    id_t clone;
    int rc;

    if (otyp != OTYP_CHR)
        return (EINVAL);

    /* Only the base nodes may be opened; each open gets a clone minor. */
    if (node >= UVM_NODE_COUNT)
        return (ENXIO);

    rc = uvm_start();
    if (rc != 0)
        return (rc);

    fops = uvm_fops[node];
    if (fops == NULL || fops->open == NULL)
        return (ENXIO);

    clone = id_alloc_nosleep(uvm_clone_ids);
    if (clone == -1)
        return (ENOSPC);

    if (ddi_soft_state_zalloc(uvm_clone_state, (int)clone) != DDI_SUCCESS) {
        id_free(uvm_clone_ids, clone);
        return (ENOMEM);
    }

    f = kmem_zalloc(sizeof (*f), KM_SLEEP);
    f->f_acct = uvm_acct_owner_create(&rc);
    if (f->f_acct == NULL) {
        kmem_free(f, sizeof (*f));
        ddi_soft_state_free(uvm_clone_state, (int)clone);
        id_free(uvm_clone_ids, clone);
        return (rc);
    }
    mutex_init(&f->f_lock, NULL, MUTEX_DRIVER, NULL);
    f->f_op = fops;
    f->f_inode = &uvm_inode;
    f->f_minor = UVM_MINOR_MAKE((minor_t)clone, node);
    f->f_node = node;
    f->f_count = 1;
    f->f_open = B_TRUE;
    list_link_init(&f->f_release_link);
    list_link_init(&f->f_poll_link);
    atomic_inc_uint(&uvm_file_live);

    rc = fops->open(&uvm_inode, f);
    if (rc != 0) {
        uvm_file_free(f);
        ddi_soft_state_free(uvm_clone_state, (int)clone);
        id_free(uvm_clone_ids, clone);
        return ((rc < 0) ? -rc : EIO);
    }
    if (f->f_mapping != NULL)
        f->f_mapping->am_owner = f->f_acct;

    mutex_enter(&uvm_files_lock);
    slot = ddi_get_soft_state(uvm_clone_state, (int)clone);
    slot->file = f;
    mutex_exit(&uvm_files_lock);

    *devp = makedevice(getmajor(*devp), f->f_minor);

    return (0);
}

static int
uvm_dev_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
    minor_t minor = getminor(dev);
    uint_t clone = UVM_MINOR_CLONE(minor);
    struct linux_file *f = NULL;
    uvm_clone_t *slot;

    if (clone == 0 || clone > UVM_CLONE_MAX)
        return (ENXIO);

    mutex_enter(&uvm_files_lock);
    slot = ddi_get_soft_state(uvm_clone_state, (int)clone);
    if (slot != NULL && slot->file != NULL && slot->file->f_minor == minor) {
        f = slot->file;
        slot->file = NULL;
        mutex_enter(&f->f_lock);
        f->f_open = B_FALSE;
        mutex_exit(&f->f_lock);
    }
    mutex_exit(&uvm_files_lock);

    if (f == NULL)
        return (ENXIO);

    ddi_soft_state_free(uvm_clone_state, (int)clone);
    id_free(uvm_clone_ids, (id_t)clone);

    pollwakeup(&f->f_pollhead, POLLERR);
    pollhead_clean(&f->f_pollhead);

    /* Mappings of the file keep it until they are gone, as on Linux. */
    if (uvm_file_drop(f))
        uvm_file_release(f);

    return (0);
}

typedef struct uvm_ioctl_baton {
    long              (*uib_ioctl)(struct linux_file *, unsigned int,
                          unsigned long);
    struct linux_file  *uib_file;
    unsigned int        uib_cmd;
    unsigned long       uib_arg;
    long                uib_ret;
} uvm_ioctl_baton_t;

static void
uvm_dev_ioctl_stk(void *arg)
{
    uvm_ioctl_baton_t *b = arg;

    b->uib_ret = b->uib_ioctl(b->uib_file, b->uib_cmd, b->uib_arg);
}

static int
uvm_dev_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
    long (*ioctl)(struct linux_file *, unsigned int, unsigned long);
    struct linux_file *f;
    uvm_ioctl_baton_t b;
    long ret;

    /* UVM copies parameters with copyin() and copyout(). */
    if (mode & FKIOCTL)
        return (ENOTSUP);

    f = uvm_file_hold_dev(dev);
    if (f == NULL)
        return (ENXIO);

    /*
     * With the builtin tests enabled, the test ioctls and the test flags of
     * regular ones (UVM_MIGRATE, UVM_POPULATE_PAGEABLE) can expose kernel
     * state, so every ioctl needs sys_config.
     */
    if (uvm_enable_builtin_tests != 0 &&
        secpolicy_sys_config(credp, B_FALSE) != 0) {
        uvm_file_rele(f);
        return (EPERM);
    }

    if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_NONE)
        ioctl = f->f_op->unlocked_ioctl;
    else
        ioctl = f->f_op->compat_ioctl;

    if (ioctl != NULL) {
        b.uib_ioctl = ioctl;
        b.uib_file = f;
        b.uib_cmd = (unsigned int)cmd;
        b.uib_arg = (unsigned long)arg;
        uvm_stack_call(uvm_dev_ioctl_stk, &b);
        ret = b.uib_ret;
    } else {
        ret = -ENOTTY;
    }

    uvm_file_rele(f);

    if (ret < 0)
        return ((ret >= -MAX_ERRNO) ? (int)-ret : EINVAL);

    *rvalp = (int)ret;
    return (0);
}

/*
 * The ioctl rule for the builtin tests, for mmap of a test file passed to
 * another process.  credp is the opener's, so check the caller's.
 */
static int
uvm_dev_segmap(dev_t dev, off_t off, struct as *as, caddr_t *addrp,
    off_t len, uint_t prot, uint_t maxprot, uint_t flags, cred_t *credp)
{
    if (uvm_enable_builtin_tests != 0 &&
        secpolicy_sys_config(CRED(), B_FALSE) != 0)
        return (EPERM);

    return (uvm_seg_segmap(dev, off, as, addrp, len, prot, maxprot, flags,
        credp));
}

static int
uvm_dev_chpoll(dev_t dev, short events, int anyyet, short *reventsp,
    struct pollhead **phpp)
{
    struct linux_file *f;
    poll_table pt = { NULL };
    unsigned int mask;

    f = uvm_file_hold_dev(dev);
    if (f == NULL)
        return (ENXIO);

    if (f->f_op->poll == NULL) {
        uvm_file_rele(f);
        return (ENXIO);
    }

    mask = f->f_op->poll(f, &pt);
    *reventsp = (short)(mask & (events | POLLERR | POLLHUP | POLLNVAL));
    if ((*reventsp == 0 && !anyyet) || (events & POLLET))
        *phpp = &f->f_pollhead;

    uvm_file_rele(f);

    return (0);
}

/*
 * Device configuration.
 */
static int
uvm_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
    switch (cmd) {
    case DDI_INFO_DEVT2DEVINFO:
        if (uvm_illumos_dip == NULL)
            return (DDI_FAILURE);
        *result = uvm_illumos_dip;
        return (DDI_SUCCESS);
    case DDI_INFO_DEVT2INSTANCE:
        *result = (void *)0;
        return (DDI_SUCCESS);
    default:
        return (DDI_FAILURE);
    }
}

static int
uvm_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
    switch (cmd) {
    case DDI_ATTACH:
        break;
    case DDI_RESUME:
        /* UVM suspend and resume are driven by the nvidia driver. */
        return (DDI_SUCCESS);
    default:
        return (DDI_FAILURE);
    }

    if (ddi_get_instance(dip) != 0 || uvm_illumos_dip != NULL)
        return (DDI_FAILURE);

    if (ddi_create_minor_node(dip, "nvidia-uvm", S_IFCHR, UVM_NODE_UVM,
        DDI_PSEUDO, 0) != DDI_SUCCESS ||
        ddi_create_minor_node(dip, "nvidia-uvm-tools", S_IFCHR,
        UVM_NODE_TOOLS, DDI_PSEUDO, 0) != DDI_SUCCESS) {
        ddi_remove_minor_node(dip, NULL);
        return (DDI_FAILURE);
    }

    uvm_illumos_major = ddi_driver_major(dip);
    uvm_illumos_dip = dip;
    ddi_report_dev(dip);

    if (!uvm_stack_split_supported()) {
        cmn_err(CE_WARN, "%s: no thread_splitstack(); UVM faults and "
            "ioctls run on the caller's stack", UVM_ILLUMOS_NAME);
    }

    return (DDI_SUCCESS);
}

static int
uvm_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
    switch (cmd) {
    case DDI_DETACH:
        break;
    case DDI_SUSPEND:
        return (DDI_SUCCESS);
    default:
        return (DDI_FAILURE);
    }

    ddi_remove_minor_node(dip, NULL);
    uvm_illumos_dip = NULL;

    return (DDI_SUCCESS);
}

static struct cb_ops uvm_cb_ops = {
    .cb_open        = uvm_dev_open,
    .cb_close       = uvm_dev_close,
    .cb_strategy    = nodev,
    .cb_print       = nodev,
    .cb_dump        = nodev,
    .cb_read        = nodev,
    .cb_write       = nodev,
    .cb_ioctl       = uvm_dev_ioctl,
    .cb_devmap      = nodev,
    .cb_mmap        = nodev,
    .cb_segmap      = uvm_dev_segmap,
    .cb_chpoll      = uvm_dev_chpoll,
    .cb_prop_op     = ddi_prop_op,
    .cb_str         = NULL,
    .cb_flag        = D_NEW | D_MP,
    .cb_rev         = CB_REV,
    .cb_aread       = nodev,
    .cb_awrite      = nodev,
};

static struct dev_ops uvm_dev_ops = {
    .devo_rev       = DEVO_REV,
    .devo_refcnt    = 0,
    .devo_getinfo   = uvm_getinfo,
    .devo_identify  = nulldev,
    .devo_probe     = nulldev,
    .devo_attach    = uvm_attach,
    .devo_detach    = uvm_detach,
    .devo_reset     = nodev,
    .devo_cb_ops    = &uvm_cb_ops,
    .devo_bus_ops   = NULL,
    .devo_power     = NULL,
    .devo_quiesce   = ddi_quiesce_not_needed,
};

static struct modldrv uvm_modldrv = {
    &mod_driverops,
    "NVIDIA UVM " NV_VERSION_STRING,
    &uvm_dev_ops,
};

static struct modlinkage uvm_modlinkage = {
    MODREV_1,
    { &uvm_modldrv, NULL }
};

static void
uvm_worker_stop(void)
{
    mutex_enter(&uvm_worker_lock);
    uvm_worker_exit = B_TRUE;
    cv_broadcast(&uvm_worker_cv);
    mutex_exit(&uvm_worker_lock);
    thread_join(uvm_worker_did);
}

static void
uvm_globals_fini(void)
{
    list_destroy(&uvm_poll_list);
    list_destroy(&uvm_release_list);
    cv_destroy(&uvm_worker_cv);
    mutex_destroy(&uvm_worker_lock);
    mutex_destroy(&uvm_start_lock);
    mutex_destroy(&uvm_files_lock);
    id_space_destroy(uvm_clone_ids);
    ddi_soft_state_fini(&uvm_clone_state);
}

int
_init(void)
{
    int rc;

    uvm_params_init();

    rc = ddi_soft_state_init(&uvm_clone_state, sizeof (uvm_clone_t), 0);
    if (rc != 0)
        return (rc);

    uvm_clone_ids = id_space_create("nvidia_uvm_clones", 1,
        UVM_CLONE_MAX + 1);
    mutex_init(&uvm_files_lock, NULL, MUTEX_DRIVER, NULL);
    mutex_init(&uvm_start_lock, NULL, MUTEX_DRIVER, NULL);
    mutex_init(&uvm_worker_lock, NULL, MUTEX_DRIVER, NULL);
    cv_init(&uvm_worker_cv, NULL, CV_DRIVER, NULL);
    list_create(&uvm_release_list, sizeof (struct linux_file),
        offsetof(struct linux_file, f_release_link));
    list_create(&uvm_poll_list, sizeof (struct linux_file),
        offsetof(struct linux_file, f_poll_link));
    uvm_worker_exit = B_FALSE;
    uvm_started = B_FALSE;

    rc = uvm_kpi_init();
    if (rc != 0)
        goto fail_globals;

    rc = uvm_page_init();
    if (rc != 0)
        goto fail_kpi;

    uvm_worker_did = uvm_thread_create(uvm_worker_main, NULL)->t_did;
    uvm_pm_init();

    rc = mod_install(&uvm_modlinkage);
    if (rc != 0)
        goto fail_worker;

    return (0);

fail_worker:
    uvm_pm_fini();
    uvm_worker_stop();
    uvm_page_fini();
fail_kpi:
    uvm_kpi_fini();
fail_globals:
    uvm_globals_fini();
    return (rc);
}

int
_fini(void)
{
    int rc;

    /* Files and segments call into this module; suspend holds UVM locks. */
    if (uvm_file_live != 0 || uvm_seg_count() != 0 || uvm_pin_busy() ||
        uvm_pm_is_suspended())
        return (EBUSY);

    rc = mod_remove(&uvm_modlinkage);
    if (rc != 0)
        return (rc);

    if (uvm_started) {
        nv_uvm_module_exit();
        uvm_started = B_FALSE;
    }

    uvm_pm_fini();
    uvm_worker_stop();
    uvm_page_fini();
    uvm_kpi_fini();
    uvm_globals_fini();
    uvm_params_fini();

    return (0);
}

int
_info(struct modinfo *modinfop)
{
    return (mod_info(&uvm_modlinkage, modinfop));
}
