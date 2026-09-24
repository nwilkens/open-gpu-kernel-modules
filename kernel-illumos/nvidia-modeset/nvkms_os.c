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
 * nvidia-modeset-os-interface.h hooks for illumos: tunables, memory,
 * time, logging, ref_ptr, timers and semaphores.
 */

#include "nvidia_modeset_illumos.h"

#include <sys/file.h>

#include "nvkms-ioctl.h"

/*
 * Tunables, the equivalent of the Linux module parameters.  Set them in
 * /etc/system, for example "set nvidia_modeset:nvkms_param_debug = 1".
 */
int nvkms_param_output_rounding_fix = 1;
int nvkms_param_disable_hdmi_frl = 0;
int nvkms_param_disable_vrr_memclk_switch = 0;
int nvkms_param_hdmi_deepcolor = 1;
uint_t nvkms_param_max_output_color_depth = 10;
int nvkms_param_opportunistic_display_sync = 1;
uint_t nvkms_param_debug_force_color_space = NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE;
int nvkms_param_enable_overlay_layers = 1;
int nvkms_param_fail_malloc = -1;
int nvkms_param_malloc_verbose = 0;
int nvkms_param_force_frl_rate = 0;
int nvkms_param_conceal_vrr_caps = 0;
int nvkms_param_enhanced_pcon_support = 0;
int nvkms_param_fail_alloc_core_channel = -1;
int nvkms_param_debug = 0;
char *nvkms_param_config_file = NULL;

volatile uint32_t nvkms_alloc_called_count;

NvBool nvkms_test_fail_alloc_core_channel(
    enum NvKmsFailAllocCoreChannelMethod method)
{
    if (method != nvkms_param_fail_alloc_core_channel) {
        return NV_FALSE;
    }

    cmn_err(CE_CONT, "!" NVKMS_LOG_PREFIX
        "Failing core channel allocation using method %d\n",
        nvkms_param_fail_alloc_core_channel);

    return NV_TRUE;
}

enum NvKmsFrlRateForce nvkms_force_frl_rate(void)
{
    switch (nvkms_param_force_frl_rate) {
    case 2:
        return NVKMS_FRL_RATE_FORCE_MAX_DSC;
    case 1:
        return NVKMS_FRL_RATE_FORCE_MAX;
    }

    return NVKMS_FRL_RATE_FORCE_NONE;
}

NvBool nvkms_conceal_vrr_caps(void)
{
    return nvkms_param_conceal_vrr_caps != 0;
}

NvBool nvkms_enhanced_pcon_support(void)
{
    return nvkms_param_enhanced_pcon_support != 0;
}

NvBool nvkms_output_rounding_fix(void)
{
    return nvkms_param_output_rounding_fix != 0;
}

NvBool nvkms_disable_hdmi_frl(void)
{
    return nvkms_param_disable_hdmi_frl != 0;
}

NvBool nvkms_disable_vrr_memclk_switch(void)
{
    return nvkms_param_disable_vrr_memclk_switch != 0;
}

NvBool nvkms_hdmi_deepcolor(void)
{
    return nvkms_param_hdmi_deepcolor != 0;
}

NvU32 nvkms_max_output_color_bpc(void)
{
    switch (nvkms_param_max_output_color_depth) {
    case 6:
        return 6;
    case 8:
        return 8;
    case 12:
        return 12;
    case 10:
    case 0:
    default:
        return 10;
    }
}

NvBool nvkms_opportunistic_display_sync(void)
{
    return nvkms_param_opportunistic_display_sync != 0;
}

enum NvKmsDebugForceColorSpace nvkms_debug_force_color_space(void)
{
    if (nvkms_param_debug_force_color_space >=
        NVKMS_DEBUG_FORCE_COLOR_SPACE_MAX) {
        return NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE;
    }
    return (enum NvKmsDebugForceColorSpace)nvkms_param_debug_force_color_space;
}

NvBool nvkms_enable_overlay_layers(void)
{
    return nvkms_param_enable_overlay_layers != 0;
}

NvBool nvkms_debug_logging(void)
{
    return nvkms_param_debug != 0;
}

/* Tegra nvhost syncpoints and Android extcon do not exist on illumos. */
NvBool nvkms_kernel_supports_syncpts(void)
{
    return NV_FALSE;
}

NvBool nvkms_syncpt_op(
    enum NvKmsSyncPtOp op,
    NvKmsSyncPtOpParams *params)
{
    return NV_FALSE;
}

void nvkms_extcon_report_hdmi(NvBool state)
{
}

void nvkms_extcon_report_hdmi_audio(NvBool state)
{
}

/*************************************************************************
 * nvidia-modeset-os-interface.h functions.  It is assumed that these
 * are called while nvkms_lock is held.
 *************************************************************************/

/* kmem_alloc(0) returns NULL; Linux kmalloc(0) returns a valid pointer. */
static size_t nvkms_kmem_size(size_t size)
{
    return (size == 0) ? 1 : size;
}

void* nvkms_alloc(size_t size, NvBool zero)
{
    int kmflag;

    if (nvkms_param_malloc_verbose || nvkms_param_fail_malloc >= 0) {
        int this_alloc = (int)atomic_inc_32_nv(&nvkms_alloc_called_count) - 1;
        if (nvkms_param_fail_malloc >= 0 &&
            nvkms_param_fail_malloc == this_alloc) {
            cmn_err(CE_WARN, NVKMS_LOG_PREFIX "Failing alloc %d",
                nvkms_param_fail_malloc);
            return NULL;
        }
    }

    size = nvkms_kmem_size(size);

    /*
     * Requests above a page may fail, as vmalloc() can on Linux, rather than
     * sleep indefinitely with nvkms_lock held.
     */
    kmflag = (size <= PAGESIZE) ? KM_SLEEP : (KM_NOSLEEP | KM_NORMALPRI);

    return zero ? kmem_zalloc(size, kmflag) : kmem_alloc(size, kmflag);
}

void nvkms_free(void *ptr, size_t size)
{
    if (ptr != NULL) {
        kmem_free(ptr, nvkms_kmem_size(size));
    }
}

void* nvkms_memset(void *ptr, NvU8 c, size_t size)
{
    return memset(ptr, c, size);
}

void* nvkms_memcpy(void *dest, const void *src, size_t n)
{
    return memcpy(dest, src, n);
}

void* nvkms_memmove(void *dest, const void *src, size_t n)
{
    return memmove(dest, src, n);
}

int nvkms_memcmp(const void *s1, const void *s2, size_t n)
{
    return memcmp(s1, s2, n);
}

size_t nvkms_strlen(const char *s)
{
    return strlen(s);
}

int nvkms_strcmp(const char *s1, const char *s2)
{
    return strcmp(s1, s2);
}

void nvkms_usleep(NvU64 usec)
{
    if (usec < 1000) {
        drv_usecwait((clock_t)usec);
    } else {
        /* Millisecond precision, clamped to ~4 seconds like Linux. */
        NvU64 msec = ((usec + 500) / 1000) & 0xFFF;

        delay(drv_usectohz((clock_t)(msec * 1000)));
    }
}

NvU64 nvkms_get_usec(void)
{
    return (NvU64)gethrtime() / 1000;
}

/*
 * nvkms_copyin()/nvkms_copyout() take no ioctl mode, so the ioctl path
 * records it here while holding nvkms_lock.  Other threads always copy from
 * user space.
 */
static kthread_t *volatile nvkms_copy_thread;
static int nvkms_copy_mode;

void nvkms_copy_mode_enter(int mode)
{
    nvkms_copy_mode = mode & FKIOCTL;
    nvkms_copy_thread = curthread;
}

void nvkms_copy_mode_exit(void)
{
    nvkms_copy_thread = NULL;
    nvkms_copy_mode = 0;
}

static int nvkms_copy_flags(void)
{
    return (nvkms_copy_thread == curthread) ? nvkms_copy_mode : 0;
}

int nvkms_copyin(void *kptr, NvU64 uaddr, size_t n)
{
    if (!nvKmsNvU64AddressIsSafe(uaddr)) {
        return -EINVAL;
    }

    if (ddi_copyin(nvKmsNvU64ToPointer(uaddr), kptr, n,
                   nvkms_copy_flags()) != 0) {
        return -EFAULT;
    }

    return 0;
}

int nvkms_copyout(NvU64 uaddr, const void *kptr, size_t n)
{
    if (!nvKmsNvU64AddressIsSafe(uaddr)) {
        return -EINVAL;
    }

    if (ddi_copyout(kptr, nvKmsNvU64ToPointer(uaddr), n,
                    nvkms_copy_flags()) != 0) {
        return -EFAULT;
    }

    return 0;
}

void nvkms_yield(void)
{
    kpreempt(KPREEMPT_SYNC);
}

void nvkms_dump_stack(void)
{
    traceback((caddr_t)getfp());
}

int nvkms_snprintf(char *str, size_t size, const char *format, ...)
{
    int ret;
    va_list ap;

    va_start(ap, format);
    ret = (int)vsnprintf(str, size, format, ap);
    va_end(ap);

    return ret;
}

int nvkms_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    return (int)vsnprintf(str, size, format, ap);
}

void nvkms_log(const int level, const char *gpuPrefix, const char *msg)
{
    if (gpuPrefix == NULL) {
        gpuPrefix = "";
    }
    if (msg == NULL) {
        msg = "";
    }

    switch (level) {
    default:
    case NVKMS_LOG_LEVEL_INFO:
        cmn_err(CE_CONT, "!" NVKMS_LOG_PREFIX "%s%s\n", gpuPrefix, msg);
        break;
    case NVKMS_LOG_LEVEL_WARN:
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "%s%s", gpuPrefix, msg);
        break;
    case NVKMS_LOG_LEVEL_ERROR:
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "ERROR: %s%s", gpuPrefix, msg);
        break;
    }
}

/*************************************************************************
 * ref_ptr implementation.
 *************************************************************************/

struct nvkms_ref_ptr {
    volatile uint32_t refcnt;
    // Access to ptr is guarded by the nvkms_lock.
    void *ptr;
};

struct nvkms_ref_ptr* nvkms_alloc_ref_ptr(void *ptr)
{
    struct nvkms_ref_ptr *ref_ptr = nvkms_alloc(sizeof(*ref_ptr), NV_FALSE);
    if (ref_ptr) {
        // The ref_ptr owner counts as a reference on the ref_ptr itself.
        ref_ptr->refcnt = 1;
        ref_ptr->ptr = ptr;
    }
    return ref_ptr;
}

void nvkms_free_ref_ptr(struct nvkms_ref_ptr *ref_ptr)
{
    if (ref_ptr) {
        ref_ptr->ptr = NULL;
        // Release the owner's reference of the ref_ptr.
        (void) nvkms_dec_ref(ref_ptr);
    }
}

void nvkms_inc_ref(struct nvkms_ref_ptr *ref_ptr)
{
    atomic_inc_32(&ref_ptr->refcnt);
}

void* nvkms_dec_ref(struct nvkms_ref_ptr *ref_ptr)
{
    void *ptr = ref_ptr->ptr;

    membar_exit();
    if (atomic_dec_32_nv(&ref_ptr->refcnt) == 0) {
        membar_enter();
        nvkms_free(ref_ptr, sizeof(*ref_ptr));
    }
    return ptr;
}

/*************************************************************************
 * Timer support
 *
 * Core NVKMS needs to be able to schedule work to execute in the
 * future, within thread context.
 *
 * timeout(9F) callbacks run in callout context, so from there schedule
 * nvkms_kthread_q_callback() on nvkms_kthread_q, which runs it in a
 * kernel thread.
 *************************************************************************/

struct nvkms_timer_t {
    nvkms_q_item_t q_item;
    timeout_id_t kernel_timer;
    NvBool cancel;
    NvBool complete;
    NvBool isRefPtr;
    NvBool kernel_timer_created;
    nvkms_timer_proc_t *proc;
    void *dataPtr;
    NvU32 dataU32;
    list_node_t timers_list;
};

/* Pending timers; the lock is also taken from interrupt threads. */
static struct {
    kmutex_t lock;
    list_t list;
} nvkms_timers;

void nvkms_queue_work(nvkms_q_t *q, nvkms_q_item_t *q_item)
{
    /*
     * nvkms_q_schedule() only fails if the item is already scheduled or
     * the queue is stopped. Neither of those should happen in NVKMS.
     */
    if (!nvkms_q_schedule(q, q_item)) {
        cmn_err(CE_WARN, NVKMS_LOG_PREFIX "failed to queue work item %p",
            (void *)q_item);
    }
}

static void nvkms_kthread_q_callback(void *arg)
{
    struct nvkms_timer_t *timer = arg;
    void *dataPtr;

    /*
     * We can delete this timer from pending timers list - it's being
     * processed now.
     */
    mutex_enter(&nvkms_timers.lock);
    list_remove(&nvkms_timers.list, timer);
    mutex_exit(&nvkms_timers.lock);

    /*
     * Make sure the timeout(9F) callback for this timer has finished, so
     * that module unload can cancel pending timers and then drain the
     * queue.
     */
    if (timer->kernel_timer_created) {
        (void) untimeout(timer->kernel_timer);
    }

    /*
     * Block during system suspend & resume in order to defer handling of
     * events such as DP_IRQ and hotplugs until after resume.
     */
    nvkms_rwsem_down_read(&nvkms_pm_lock);

    (void) nvkms_lock_down(NV_FALSE);

    if (timer->isRefPtr) {
        // If the object this timer refers to was destroyed, treat the timer as
        // canceled.
        dataPtr = nvkms_dec_ref(timer->dataPtr);
        if (!dataPtr) {
            timer->cancel = NV_TRUE;
        }
    } else {
        dataPtr = timer->dataPtr;
    }

    if (!timer->cancel) {
        timer->proc(dataPtr, timer->dataU32);
        timer->complete = NV_TRUE;
    }

    if (timer->isRefPtr) {
        kmem_free(timer, sizeof(*timer));
    } else if (timer->cancel) {
        nvkms_free(timer, sizeof(*timer));
    }

    nvkms_lock_up();

    nvkms_rwsem_up_read(&nvkms_pm_lock);
}

static void nvkms_timer_callback(void *arg)
{
    struct nvkms_timer_t *nvkms_timer = arg;

    nvkms_queue_work(&nvkms_kthread_q, &nvkms_timer->q_item);
}

static clock_t nvkms_usec_to_ticks(NvU64 usec)
{
    const NvU64 max_usec = (NvU64)(LONG_MAX / 2);

    return drv_usectohz((clock_t)((usec > max_usec) ? max_usec : usec));
}

static void
nvkms_init_timer(struct nvkms_timer_t *timer, nvkms_timer_proc_t *proc,
                 void *dataPtr, NvU32 dataU32, NvBool isRefPtr, NvU64 usec)
{
    bzero(timer, sizeof(*timer));
    timer->cancel = NV_FALSE;
    timer->complete = NV_FALSE;
    timer->isRefPtr = isRefPtr;

    timer->proc = proc;
    timer->dataPtr = dataPtr;
    timer->dataU32 = dataU32;

    nvkms_q_item_init(&timer->q_item, nvkms_kthread_q_callback, timer);

    /*
     * Hold the list lock until the timer is armed or queued, so that the
     * queue callback, which removes it from the list, cannot run on a
     * half-initialized timer.
     */
    mutex_enter(&nvkms_timers.lock);
    list_insert_head(&nvkms_timers.list, timer);

    if (usec == 0) {
        timer->kernel_timer_created = NV_FALSE;
        nvkms_queue_work(&nvkms_kthread_q, &timer->q_item);
    } else {
        timer->kernel_timer_created = NV_TRUE;
        timer->kernel_timer = timeout(nvkms_timer_callback, timer,
                                      nvkms_usec_to_ticks(usec));
    }
    mutex_exit(&nvkms_timers.lock);
}

nvkms_timer_handle_t*
nvkms_alloc_timer(nvkms_timer_proc_t *proc,
                  void *dataPtr, NvU32 dataU32,
                  NvU64 usec)
{
    // nvkms_alloc_timer cannot be called from an interrupt context.
    struct nvkms_timer_t *timer = nvkms_alloc(sizeof(*timer), NV_FALSE);
    if (timer) {
        nvkms_init_timer(timer, proc, dataPtr, dataU32, NV_FALSE, usec);
    }
    return timer;
}

NvBool
nvkms_alloc_timer_with_ref_ptr(nvkms_timer_proc_t *proc,
                               struct nvkms_ref_ptr *ref_ptr,
                               NvU32 dataU32, NvU64 usec)
{
    // Called from RM event callbacks, which may run in interrupt context.
    struct nvkms_timer_t *timer = kmem_alloc(sizeof(*timer), KM_NOSLEEP);
    if (timer) {
        // Reference the ref_ptr to make sure that it doesn't get freed before
        // the timer fires.
        nvkms_inc_ref(ref_ptr);
        nvkms_init_timer(timer, proc, ref_ptr, dataU32, NV_TRUE, usec);
    }

    return timer != NULL;
}

void nvkms_free_timer(nvkms_timer_handle_t *handle)
{
    struct nvkms_timer_t *timer = handle;

    if (timer == NULL) {
        return;
    }

    if (timer->complete) {
        nvkms_free(timer, sizeof(*timer));
        return;
    }

    timer->cancel = NV_TRUE;
}

void nvkms_timers_init(void)
{
    mutex_init(&nvkms_timers.lock, NULL, MUTEX_DRIVER, NULL);
    list_create(&nvkms_timers.list, sizeof(struct nvkms_timer_t),
                offsetof(struct nvkms_timer_t, timers_list));
}

void nvkms_timers_fini(void)
{
    list_destroy(&nvkms_timers.list);
    mutex_destroy(&nvkms_timers.lock);
}

/* Cancel armed timers at unload; queued ones are drained by nvkms_q_stop(). */
void nvkms_cancel_timers(void)
{
    struct nvkms_timer_t *timer;

restart:
    mutex_enter(&nvkms_timers.lock);

    for (timer = list_head(&nvkms_timers.list); timer != NULL;
         timer = list_next(&nvkms_timers.list, timer)) {
        /*
         * untimeout() returns -1 if the callout already ran; its queue item
         * then frees the timer.  Otherwise the timer never fired and is
         * ours to free.
         */
        if (timer->kernel_timer_created &&
            untimeout(timer->kernel_timer) != -1) {
            list_remove(&nvkms_timers.list, timer);
            mutex_exit(&nvkms_timers.lock);

            if (timer->isRefPtr) {
                (void) nvkms_dec_ref(timer->dataPtr);
                kmem_free(timer, sizeof(*timer));
            } else {
                nvkms_free(timer, sizeof(*timer));
            }

            goto restart;
        }
    }

    mutex_exit(&nvkms_timers.lock);
}

/*
 * illumos has no backlight class; this matches Linux built without
 * CONFIG_BACKLIGHT_CLASS_DEVICE.
 */
struct nvkms_backlight_device*
nvkms_register_backlight(NvU32 gpu_id, NvU32 display_id, void *drv_priv,
                         NvU32 current_brightness)
{
    return NULL;
}

void nvkms_unregister_backlight(struct nvkms_backlight_device *nvkms_bd)
{
}

/*************************************************************************
 * APIs for locking.
 *************************************************************************/

/* Linux semaphores may be released by a thread other than the holder. */
struct nvkms_sema_t {
    ksema_t os_sema;
};

nvkms_sema_handle_t* nvkms_sema_alloc(void)
{
    nvkms_sema_handle_t *sema = nvkms_alloc(sizeof(*sema), NV_TRUE);

    if (sema != NULL) {
        sema_init(&sema->os_sema, 1, NULL, SEMA_DRIVER, NULL);
    }

    return sema;
}

void nvkms_sema_free(nvkms_sema_handle_t *sema)
{
    if (sema != NULL) {
        sema_destroy(&sema->os_sema);
        nvkms_free(sema, sizeof(*sema));
    }
}

void nvkms_sema_down(nvkms_sema_handle_t *sema)
{
    sema_p(&sema->os_sema);
}

void nvkms_sema_up(nvkms_sema_handle_t *sema)
{
    sema_v(&sema->os_sema);
}

/*
 * The core's procfs files (nvKmsGetProcFiles) are not exported: illumos
 * has no procfs for drivers, the same as Linux without CONFIG_PROC_FS.
 */

/*************************************************************************
 * Interface with resman.
 *************************************************************************/

void nvkms_call_rm(void *ops)
{
    nvidia_modeset_stack_ptr stack = NULL;

    if (nvkms_rm_ops.alloc_stack(&stack) != 0) {
        return;
    }

    nvkms_rm_ops.op(stack, ops);

    nvkms_rm_ops.free_stack(stack);
}

/*************************************************************************
 * GPU open/close and queries through resman.
 *************************************************************************/

NvBool nvkms_open_gpu(NvU32 gpuId, NvBool reset_aware)
{
    nvidia_modeset_stack_ptr stack = NULL;
    NvBool ret;

    if (nvkms_rm_ops.alloc_stack(&stack) != 0) {
        return NV_FALSE;
    }

    ret = nvkms_rm_ops.open_gpu(gpuId, stack, reset_aware) == 0;

    nvkms_rm_ops.free_stack(stack);

    return ret;
}

void nvkms_close_gpu(NvU32 gpuId, NvBool reset_aware)
{
    nvidia_modeset_stack_ptr stack = NULL;

    if (nvkms_rm_ops.alloc_stack(&stack) != 0) {
        return;
    }

    nvkms_rm_ops.close_gpu(gpuId, stack, reset_aware);

    nvkms_rm_ops.free_stack(stack);
}

NvU32 nvkms_enumerate_gpus(nv_gpu_info_t *gpu_info)
{
    return nvkms_rm_ops.enumerate_gpus(gpu_info);
}

NvBool nvkms_allow_write_combining(void)
{
    return nvkms_rm_ops.system_info.allow_write_combining;
}

/*************************************************************************
 * NVKMS KAPI functions, for kernel modules that depend on
 * drv/nvidia_modeset.
 ************************************************************************/

NvBool nvKmsKapiGetFunctionsTable
(
    struct NvKmsKapiFunctionsTable *funcsTable
)
{
    return nvKmsKapiGetFunctionsTableInternal(funcsTable);
}

NvU32 nvKmsKapiF16ToF32(NvU16 a)
{
    return nvKmsKapiF16ToF32Internal(a);
}

NvU16 nvKmsKapiF32ToF16(NvU32 a)
{
    return nvKmsKapiF32ToF16Internal(a);
}

NvU32 nvKmsKapiF32Mul(NvU32 a, NvU32 b)
{
    return nvKmsKapiF32MulInternal(a, b);
}

NvU32 nvKmsKapiF32Div(NvU32 a, NvU32 b)
{
    return nvKmsKapiF32DivInternal(a, b);
}

NvU32 nvKmsKapiF32Add(NvU32 a, NvU32 b)
{
    return nvKmsKapiF32AddInternal(a, b);
}

NvU32 nvKmsKapiF32ToUI32RMinMag(NvU32 a, NvBool exact)
{
    return nvKmsKapiF32ToUI32RMinMagInternal(a, exact);
}

NvU32 nvKmsKapiUI32ToF32(NvU32 a)
{
    return nvKmsKapiUI32ToF32Internal(a);
}
