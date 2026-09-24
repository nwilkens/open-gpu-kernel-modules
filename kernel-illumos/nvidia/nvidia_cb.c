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
 * Character device entry points.
 *
 * Every open(9E) clones a minor, so each open instance has its own
 * nv_illumos_file_private_t, the equivalent of Linux's per-struct-file
 * private data.  dup(2) and fork(2) share the instance the way they share a
 * struct file on Linux, and specfs defers close(9E) until the last mapping of
 * the instance is gone.
 *
 * RM can keep an open instance referenced beyond close(2) (through
 * nv_get_file_private()), the way it keeps a struct file referenced on Linux.
 * close(9E) therefore only drops the open's reference; the instance is torn
 * down when the last reference goes, on a taskq if that is not close(9E).
 */

#include "nv-illumos.h"
#include "nv-ioctl.h"

#include <sys/taskq_impl.h>

static void            *nv_clone_ss;
static id_space_t      *nv_clone_ids;
static krwlock_t        nv_clone_lock;

int
nv_clone_init(void)
{
    int rc;

    rc = ddi_soft_state_init(&nv_clone_ss,
        sizeof (nv_illumos_file_private_t), 64);
    if (rc != 0)
        return (rc);

    nv_clone_ids = id_space_create("nvidia_clone", 1, NV_MINOR_CLONE_MAX + 1);
    rw_init(&nv_clone_lock, NULL, RW_DRIVER, NULL);

    return (0);
}

void
nv_clone_fini(void)
{
    rw_destroy(&nv_clone_lock);
    id_space_destroy(nv_clone_ids);
    ddi_soft_state_fini(&nv_clone_ss);
}

static nv_illumos_file_private_t *
nv_clone_alloc(minor_t node, nv_node_kind_t kind)
{
    nv_illumos_file_private_t *nvifp;
    id_t clone;

    clone = id_alloc_nosleep(nv_clone_ids);
    if (clone == -1)
        return (NULL);

    rw_enter(&nv_clone_lock, RW_WRITER);
    if (ddi_soft_state_zalloc(nv_clone_ss, clone) != DDI_SUCCESS)
    {
        rw_exit(&nv_clone_lock);
        id_free(nv_clone_ids, clone);
        return (NULL);
    }
    nvifp = ddi_get_soft_state(nv_clone_ss, clone);

    nvifp->minor = NV_MINOR_MAKE(clone, node);
    nvifp->kind = kind;
    nvifp->refcnt = 1;
    mutex_init(&nvifp->ref_lock, NULL, MUTEX_DRIVER, NULL);
    mutex_init(&nvifp->fp_lock, NULL, MUTEX_DRIVER, DDI_INTR_PRI(nv_intr_pri));
    rw_init(&nvifp->file_va_lock, NULL, RW_DRIVER, NULL);
    list_create(&nvifp->mappings, sizeof (nv_devmap_priv_t),
        offsetof(nv_devmap_priv_t, link));
    rw_exit(&nv_clone_lock);

    return (nvifp);
}

static void
nv_clone_free(nv_illumos_file_private_t *nvifp)
{
    minor_t clone = NV_MINOR_CLONE(nvifp->minor);

    rw_enter(&nv_clone_lock, RW_WRITER);
    list_destroy(&nvifp->mappings);
    rw_destroy(&nvifp->file_va_lock);
    mutex_destroy(&nvifp->fp_lock);
    mutex_destroy(&nvifp->ref_lock);
    ddi_soft_state_free(nv_clone_ss, clone);
    rw_exit(&nv_clone_lock);

    id_free(nv_clone_ids, clone);
}

/*
 * Looks up the open instance of a clone minor without taking a reference.
 * Only valid from entry points specfs serializes against close(9E).
 */
nv_illumos_file_private_t *
nv_clone_lookup(minor_t minor)
{
    nv_illumos_file_private_t *nvifp;
    minor_t clone = NV_MINOR_CLONE(minor);

    if (clone == 0)
        return (NULL);

    rw_enter(&nv_clone_lock, RW_READER);
    nvifp = ddi_get_soft_state(nv_clone_ss, clone);
    if (nvifp != NULL && nvifp->minor != minor)
        nvifp = NULL;
    rw_exit(&nv_clone_lock);

    return (nvifp);
}

/* Takes a reference on a still-open instance. */
nv_illumos_file_private_t *
nv_file_private_hold(dev_t dev)
{
    nv_illumos_file_private_t *nvifp;
    minor_t minor = getminor(dev);
    minor_t clone = NV_MINOR_CLONE(minor);

    if (clone == 0)
        return (NULL);

    rw_enter(&nv_clone_lock, RW_READER);
    nvifp = ddi_get_soft_state(nv_clone_ss, clone);
    if (nvifp != NULL)
    {
        mutex_enter(&nvifp->ref_lock);
        if (nvifp->minor == minor && !nvifp->closing)
        {
            nvifp->refcnt++;
            mutex_exit(&nvifp->ref_lock);
        }
        else
        {
            mutex_exit(&nvifp->ref_lock);
            nvifp = NULL;
        }
    }
    rw_exit(&nv_clone_lock);

    return (nvifp);
}

static void nv_file_private_destroy(nv_illumos_file_private_t *);

static void
nv_file_private_destroy_task(void *arg)
{
    nv_file_private_destroy(arg);
}

void
nv_file_private_rele(nv_illumos_file_private_t *nvifp)
{
    NvBool last;

    mutex_enter(&nvifp->ref_lock);
    ASSERT(nvifp->refcnt > 0);
    last = (--nvifp->refcnt == 0);
    mutex_exit(&nvifp->ref_lock);

    /*
     * The last reference outside close(9E) belongs to RM, possibly inside an
     * RM call; tear down on the taskq so RM is not re-entered.
     */
    if (last)
    {
        taskq_dispatch_ent(nv_global_queue.tq, nv_file_private_destroy_task,
            nvifp, TQ_SLEEP, &nvifp->destroy_ent);
    }
}

static void
nv_file_private_teardown(nv_illumos_file_private_t *nvifp)
{
    nvidia_event_t *nvet;
    nv_alloc_mapping_list_node_t *node;

    while ((nvet = nvifp->event_head) != NULL)
    {
        nvifp->event_head = nvet->next;
        kmem_free(nvet, sizeof (*nvet));
    }
    nvifp->event_tail = NULL;

    while ((node = nvifp->file_mapping_list) != NULL)
    {
        nvifp->file_mapping_list = node->pNext;

        if (node->context.alloc != NULL)
            nv_alloc_rele(node->context.alloc);
        if (node->context.page_array != NULL)
            os_free_mem(node->context.page_array);
        if (node->context.memArea.pRanges != NULL)
            os_free_mem(node->context.memArea.pRanges);
        kmem_free(node, sizeof (*node));
    }

    if (nvifp->attached_gpus != NULL)
    {
        kmem_free(nvifp->attached_gpus,
            nvifp->num_attached_gpus * sizeof (NvU32));
        nvifp->attached_gpus = NULL;
        nvifp->num_attached_gpus = 0;
    }

    nv_stack_free(nvifp->sp);
    nvifp->sp = NULL;
}

static void
nvidia_close_ctl(nv_illumos_file_private_t *nvifp)
{
    nv_illumos_state_t *nvis = &nv_ctl_device;
    nv_state_t *nv = NV_STATE_PTR(nvis);
    nvidia_stack_t *sp = nvifp->sp;
    size_t i;

    sema_p(&nvis->ldata_lock);
    if (--nvis->usage_count == 0)
        nv->flags &= ~NV_FLAG_INITIALIZED;
    sema_v(&nvis->ldata_lock);

    rm_cleanup_file_private(sp, nv, &nvifp->nvfp);

    for (i = 0; i < nvifp->num_attached_gpus; i++)
    {
        if (nvifp->attached_gpus[i] != 0)
            nvidia_dev_put(nvifp->attached_gpus[i], sp, NV_FALSE);
    }
}

static void
nvidia_close_gpu(nv_illumos_file_private_t *nvifp)
{
    nv_illumos_state_t *nvis = nvifp->nvis;
    nv_state_t *nv = NV_STATE_PTR(nvis);
    nvidia_stack_t *sp = nvifp->sp;

    rm_cleanup_file_private(sp, nv, &nvifp->nvfp);

    sema_p(&nvis->mmap_lock);
    list_remove(&nvis->open_files, nvifp);
    sema_v(&nvis->mmap_lock);

    sema_p(&nvis->ldata_lock);
    nv_close_device(nv, sp);
    sema_v(&nvis->ldata_lock);
}

static void
nv_file_private_destroy(nv_illumos_file_private_t *nvifp)
{
    /* RM may still be posting events that reference this open instance. */
    while (nv_event_posting_refcount_read(&nvifp->nvfp) != 0)
        delay(1);

    switch (nvifp->kind)
    {
        case NV_NODE_CTL:
            nvidia_close_ctl(nvifp);
            break;
        case NV_NODE_GPU:
            if (nvifp->nvis != NULL)
                nvidia_close_gpu(nvifp);
            break;
        case NV_NODE_CAP:
            nv_caps_close(nvifp);
            break;
        case NV_NODE_NVLINK:
            nvlink_node_close(nvifp);
            break;
        case NV_NODE_NVSWITCH_CTL:
        case NV_NODE_NVSWITCH:
            nvswitch_node_close(nvifp);
            break;
        case NV_NODE_IMEX:
            break;
    }

    nv_file_private_teardown(nvifp);
    nv_clone_free(nvifp);
}

static int
nv_node_kind(minor_t node, nv_node_kind_t *kind)
{
    if (node == NV_MINOR_CTL)
        *kind = NV_NODE_CTL;
    else if (NV_MINOR_IS_GPU(node))
        *kind = NV_NODE_GPU;
    else if (NV_MINOR_IS_CAP(node))
        *kind = NV_NODE_CAP;
    else if (NV_MINOR_IS_IMEX(node))
        *kind = NV_NODE_IMEX;
    else if (node == NV_MINOR_NVLINK)
        *kind = NV_NODE_NVLINK;
    else if (node == NV_MINOR_NVSWITCH_CTL)
        *kind = NV_NODE_NVSWITCH_CTL;
    else if (NV_MINOR_IS_NVSWITCH(node))
        *kind = NV_NODE_NVSWITCH;
    else
        return (ENXIO);

    return (0);
}

static int
nvidia_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
    nv_illumos_file_private_t *nvifp;
    nv_illumos_state_t *nvis;
    minor_t minor = getminor(*devp);
    minor_t node = NV_MINOR_NODE(minor);
    nv_node_kind_t kind;
    int rc;

    if (otyp != OTYP_CHR)
        return (EINVAL);

    /* Clone minors are not openable by name. */
    if (NV_MINOR_CLONE(minor) != 0)
        return (ENXIO);

    if ((rc = nv_node_kind(node, &kind)) != 0)
        return (rc);

    if (kind == NV_NODE_IMEX && !nv_caps_imex_minor_valid(node))
        return (ENXIO);

    nvifp = nv_clone_alloc(node, kind);
    if (nvifp == NULL)
        return (EBUSY);

    if (nv_stack_alloc(&nvifp->sp) != 0)
    {
        nv_clone_free(nvifp);
        return (ENOMEM);
    }

    switch (kind)
    {
        case NV_NODE_CTL:
            nvis = &nv_ctl_device;

            sema_p(&nvis->ldata_lock);
            if (nvis->usage_count == 0)
                nvis->nv_state.flags |= (NV_FLAG_INITIALIZED | NV_FLAG_CONTROL);
            nvis->usage_count++;
            sema_v(&nvis->ldata_lock);

            nvifp->nvis = nvis;
            rc = 0;
            break;

        case NV_NODE_GPU:
            nvis = nv_find_minor_locked(node);
            if (nvis == NULL)
            {
                rc = ENXIO;
                break;
            }

            rc = nv_open_device(NV_STATE_PTR(nvis), nvifp->sp);
            nvifp->open_rc = rc;
            if (rc == 0)
            {
                nvifp->adapter_status = NV_OK;

                sema_p(&nvis->mmap_lock);
                list_insert_tail(&nvis->open_files, nvifp);
                sema_v(&nvis->mmap_lock);

                nvifp->nvis = nvis;
            }
            sema_v(&nvis->ldata_lock);
            break;

        case NV_NODE_CAP:
            rc = nv_caps_open(nvifp, node, credp);
            break;

        case NV_NODE_IMEX:
            rc = 0;
            break;

        case NV_NODE_NVLINK:
            rc = nvlink_node_open(nvifp, credp);
            break;

        case NV_NODE_NVSWITCH_CTL:
        case NV_NODE_NVSWITCH:
            rc = nvswitch_node_open(nvifp, node, credp);
            break;
    }

    if (rc != 0)
    {
        nv_stack_free(nvifp->sp);
        nv_clone_free(nvifp);
        return (rc);
    }

    *devp = makedevice(getmajor(*devp), nvifp->minor);
    return (0);
}

static int
nvidia_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
    nv_illumos_file_private_t *nvifp;
    NvBool last;

    nvifp = nv_clone_lookup(getminor(dev));
    if (nvifp == NULL)
        return (ENXIO);

    /* chpoll(9E): no poll cache may keep this pollhead past close. */
    pollwakeup(&nvifp->pollhead, POLLERR);
    pollhead_clean(&nvifp->pollhead);

    mutex_enter(&nvifp->ref_lock);
    nvifp->closing = NV_TRUE;
    last = (--nvifp->refcnt == 0);
    mutex_exit(&nvifp->ref_lock);

    if (last)
        nv_file_private_destroy(nvifp);

    return (0);
}

static int
nvidia_chpoll(dev_t dev, short events, int anyyet, short *reventsp,
    struct pollhead **phpp)
{
    nv_illumos_file_private_t *nvifp;
    short revents = 0;

    nvifp = nv_file_private_hold(dev);
    if (nvifp == NULL)
        return (ENXIO);

    if (nvifp->kind == NV_NODE_NVSWITCH || nvifp->kind == NV_NODE_NVSWITCH_CTL)
    {
        int rc = nvswitch_node_chpoll(nvifp, events, anyyet, reventsp, phpp);

        nv_file_private_rele(nvifp);
        return (rc);
    }

    if (nvifp->kind != NV_NODE_CTL && nvifp->kind != NV_NODE_GPU)
    {
        nv_file_private_rele(nvifp);
        return (ENXIO);
    }

    if (nvifp->nvis == NULL)
    {
        *reventsp = POLLERR;
        nv_file_private_rele(nvifp);
        return (0);
    }

    if (NV_IS_DEVICE_IN_SURPRISE_REMOVAL(NV_STATE_PTR(nvifp->nvis)))
    {
        *reventsp = POLLHUP;
        nv_file_private_rele(nvifp);
        return (0);
    }

    mutex_enter(&nvifp->fp_lock);
    if (nvifp->event_head != NULL || nvifp->dataless_event_pending)
    {
        revents = (POLLPRI | POLLIN) & events;
        nvifp->dataless_event_pending = NV_FALSE;
    }
    mutex_exit(&nvifp->fp_lock);

    *reventsp = revents;
    if ((revents == 0 && !anyyet) || (events & POLLET))
        *phpp = &nvifp->pollhead;

    nv_file_private_rele(nvifp);
    return (0);
}

void NV_API_CALL nv_post_event(
    nv_event_t *event,
    NvHandle    handle,
    NvU32       index,
    NvU32       info32,
    NvU16       info16,
    NvBool      data_valid
)
{
    nv_illumos_file_private_t *nvifp = nv_get_nvifp_from_nvfp(event->nvfp);
    nvidia_event_t *nvet;

    mutex_enter(&nvifp->fp_lock);

    if (data_valid)
    {
        nvet = kmem_alloc(sizeof (*nvet), KM_NOSLEEP);
        if (nvet == NULL)
        {
            mutex_exit(&nvifp->fp_lock);
            return;
        }

        nvet->event = *event;
        nvet->event.hObject = handle;
        nvet->event.index = index;
        nvet->event.info32 = info32;
        nvet->event.info16 = info16;
        nvet->next = NULL;

        if (nvifp->event_tail != NULL)
            nvifp->event_tail->next = nvet;
        if (nvifp->event_head == NULL)
            nvifp->event_head = nvet;
        nvifp->event_tail = nvet;
    }
    else
    {
        nvifp->dataless_event_pending = NV_TRUE;
    }

    mutex_exit(&nvifp->fp_lock);

    pollwakeup(&nvifp->pollhead, POLLIN | POLLPRI);
}

int NV_API_CALL nv_get_event(
    nv_file_private_t  *nvfp,
    nv_event_t         *event,
    NvU32              *pending
)
{
    nv_illumos_file_private_t *nvifp = nv_get_nvifp_from_nvfp(nvfp);
    nvidia_event_t *nvet;

    mutex_enter(&nvifp->fp_lock);

    nvet = nvifp->event_head;
    if (nvet == NULL)
    {
        mutex_exit(&nvifp->fp_lock);
        return NV_ERR_GENERIC;
    }

    *event = nvet->event;
    if (nvifp->event_tail == nvet)
        nvifp->event_tail = NULL;
    nvifp->event_head = nvet->next;
    *pending = (nvifp->event_head != NULL);

    mutex_exit(&nvifp->fp_lock);

    kmem_free(nvet, sizeof (*nvet));
    return NV_OK;
}

/*
 * Resolves a file descriptor of the calling process to an nvidia control or
 * GPU open instance.  The returned instance is held until
 * nv_put_file_private().
 */
nv_file_private_t* NV_API_CALL nv_get_file_private(
    NvS32 fd,
    NvBool ctl,
    void **os_private
)
{
    nv_illumos_file_private_t *nvifp = NULL;
    file_t *fp;
    vnode_t *vp;
    dev_t rdev;
    minor_t node;

    *os_private = NULL;

    if (fd < 0 || (fp = getf(fd)) == NULL)
        return NULL;

    vp = fp->f_vnode;
    if (vp == NULL || vp->v_type != VCHR || nv_ctl_dip == NULL)
        goto done;

    rdev = vp->v_rdev;
    if (getmajor(rdev) != ddi_driver_major(nv_ctl_dip))
        goto done;

    node = NV_MINOR_NODE(getminor(rdev));
    if (ctl ? (node != NV_MINOR_CTL) : !NV_MINOR_IS_GPU(node))
        goto done;

    nvifp = nv_file_private_hold(rdev);

done:
    releasef(fd);

    if (nvifp == NULL)
        return NULL;

    *os_private = nvifp;
    return &nvifp->nvfp;
}

void NV_API_CALL nv_put_file_private(void *os_private)
{
    if (os_private != NULL)
        nv_file_private_rele(os_private);
}

static int
nvidia_devmap(dev_t dev, devmap_cookie_t dhp, offset_t off, size_t len,
    size_t *maplen, uint_t model)
{
    nv_illumos_file_private_t *nvifp;
    nv_alloc_mapping_list_node_t *node;
    int rc;

    nvifp = nv_file_private_hold(dev);
    if (nvifp == NULL)
        return (ENXIO);

    /* Do not allow mmap on an fd into which RM objects have been exported. */
    if ((nvifp->kind != NV_NODE_CTL && nvifp->kind != NV_NODE_GPU) ||
        nvifp->nvfp.handles != NULL || nvifp->nvis == NULL)
    {
        nv_file_private_rele(nvifp);
        return (EINVAL);
    }

    rw_enter(&nvifp->file_va_lock, RW_READER);
    node = nvifp->file_mapping_list;
    if (node == NULL || !node->context.valid)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: VM: invalid mmap context\n");
        rc = EINVAL;
    }
    else if (NV_IS_CTL_DEVICE(NV_STATE_PTR(nvifp->nvis)))
    {
        rc = nv_devmap_sysmem(nvifp, dhp, &node->context, off, len, maplen);
    }
    else
    {
        rc = nv_devmap_devmem(nvifp, dhp, &node->context, off, len, maplen);
    }
    rw_exit(&nvifp->file_va_lock);

    nv_file_private_rele(nvifp);
    return (rc);
}

/*
 * Enforce the protection RM granted for this mapping before the segment is
 * created, so a read-only mapping cannot be made writable later.
 */
static int
nvidia_segmap(dev_t dev, off_t off, struct as *as, caddr_t *addrp, off_t len,
    unsigned int prot, unsigned int maxprot, unsigned int flags, cred_t *credp)
{
    nv_illumos_file_private_t *nvifp;
    nv_alloc_mapping_list_node_t *node;
    NvU32 ctx_prot = 0;
    NvBool valid = NV_FALSE;

    nvifp = nv_file_private_hold(dev);
    if (nvifp == NULL)
        return (ENXIO);

    rw_enter(&nvifp->file_va_lock, RW_READER);
    node = nvifp->file_mapping_list;
    if (node != NULL && node->context.valid)
    {
        ctx_prot = node->context.prot;
        valid = NV_TRUE;
    }
    rw_exit(&nvifp->file_va_lock);
    nv_file_private_rele(nvifp);

    if (!valid)
        return (EINVAL);

    if ((flags & MAP_TYPE) != MAP_SHARED)
        return (EINVAL);

    if ((ctx_prot & NV_PROTECT_WRITEABLE) == 0)
    {
        if (prot & PROT_WRITE)
            return (EACCES);
        maxprot &= ~PROT_WRITE;
    }

    return devmap_setup(dev, (offset_t)off, as, addrp, (size_t)len, prot,
        maxprot, flags, credp);
}

nv_alloc_mapping_list_node_t** NV_API_CALL
nv_acquire_file_va(nv_file_private_t *nvfp, NvBool bWrite)
{
    nv_illumos_file_private_t *nvifp = nv_get_nvifp_from_nvfp(nvfp);

    rw_enter(&nvifp->file_va_lock, bWrite ? RW_WRITER : RW_READER);
    return &nvifp->file_mapping_list;
}

void NV_API_CALL
nv_release_file_va(nv_file_private_t *nvfp, NvBool bWrite)
{
    nv_illumos_file_private_t *nvifp = nv_get_nvifp_from_nvfp(nvfp);

    rw_exit(&nvifp->file_va_lock);
}

struct cb_ops nvidia_cb_ops = {
    .cb_open        = nvidia_open,
    .cb_close       = nvidia_close,
    .cb_strategy    = nodev,
    .cb_print       = nodev,
    .cb_dump        = nodev,
    .cb_read        = nodev,
    .cb_write       = nodev,
    .cb_ioctl       = nvidia_ioctl,
    .cb_devmap      = nvidia_devmap,
    .cb_mmap        = nodev,
    .cb_segmap      = nvidia_segmap,
    .cb_chpoll      = nvidia_chpoll,
    .cb_prop_op     = ddi_prop_op,
    .cb_str         = NULL,
    .cb_flag        = D_NEW | D_MP | D_64BIT | D_DEVMAP,
    .cb_rev         = CB_REV,
    .cb_aread       = nodev,
    .cb_awrite      = nodev,
};
