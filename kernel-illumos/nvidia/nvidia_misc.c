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
 * Timers, power management, error reporting and platform queries.
 */

#include "nv-illumos.h"

#include <sys/sdt.h>
#include <sys/smbios.h>
#include <sys/callo.h>
#include <sys/framebuffer.h>

/*
 * RC watchdog: RM's callback runs once a second while it returns NV_OK.
 */
static void
nvidia_rc_timer_callback(void *arg)
{
    nv_illumos_state_t *nvis = arg;
    nv_state_t *nv = NV_STATE_PTR(nvis);

    if (NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv))
    {
        nv_printf(NV_DBG_INFO,
            "NVRM: GPU is lost, skipping device timer callbacks\n");
        return;
    }

    if (rm_run_rc_callback(nvis->sp[NV_DEV_STACK_TIMER], nv) != NV_OK)
        return;

    mutex_enter(&nvis->timer_lock);
    if (nvis->rc_timer_armed)
        nvis->rc_timer = timeout(nvidia_rc_timer_callback, nvis,
            drv_usectohz(MICROSEC));
    mutex_exit(&nvis->timer_lock);
}

int NV_API_CALL nv_start_rc_timer(nv_state_t *nv)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);

    if (nv->rc_timer_enabled)
        return -1;

    mutex_enter(&nvis->timer_lock);
    nvis->rc_timer_armed = NV_TRUE;
    nvis->rc_timer = timeout(nvidia_rc_timer_callback, nvis,
        drv_usectohz(MICROSEC));
    mutex_exit(&nvis->timer_lock);

    nv->rc_timer_enabled = 1;
    return 0;
}

int NV_API_CALL nv_stop_rc_timer(nv_state_t *nv)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);
    timeout_id_t id;

    if (!nv->rc_timer_enabled)
        return -1;

    nv->rc_timer_enabled = 0;

    /*
     * The callback re-arms under timer_lock, so clear rc_timer_armed first;
     * untimeout() then waits for a running callback to finish.
     */
    mutex_enter(&nvis->timer_lock);
    nvis->rc_timer_armed = NV_FALSE;
    id = nvis->rc_timer;
    nvis->rc_timer = 0;
    mutex_exit(&nvis->timer_lock);

    if (id != 0)
        (void) untimeout(id);

    return 0;
}

/*
 * Nanosecond timers.  RM may re-arm a timer from inside its own callback, so
 * cancellation only waits for a running callback when called from another
 * thread.
 */
struct nv_nano_timer {
    kmutex_t            lock;
    callout_id_t        id;
    kthread_t          *cb_thread;
    nv_illumos_state_t *nvis;
    void               *pTmrEvent;
};

static void
nvidia_nano_timer_callback(void *arg)
{
    nv_nano_timer_t *nt = arg;
    nvidia_stack_t *sp = NULL;

    mutex_enter(&nt->lock);
    nt->id = 0;
    nt->cb_thread = curthread;
    mutex_exit(&nt->lock);

    if (nv_stack_alloc(&sp) == 0)
    {
        if (rm_run_nano_timer_callback(sp, NV_STATE_PTR(nt->nvis),
                nt->pTmrEvent) != NV_OK)
            nv_printf(NV_DBG_ERRORS, "NVRM: Error in service of callback \n");
        nv_stack_free(sp);
    }
    else
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: no cache memory \n");
    }

    mutex_enter(&nt->lock);
    nt->cb_thread = NULL;
    mutex_exit(&nt->lock);
}

static void
nv_nano_timer_cancel_locked(nv_nano_timer_t *nt)
{
    callout_id_t id = nt->id;
    NvBool self = (nt->cb_thread == curthread);

    nt->id = 0;
    if (id == 0)
        return;

    mutex_exit(&nt->lock);
    (void) untimeout_generic(id, self ? 1 : 0);
    mutex_enter(&nt->lock);
}

void NV_API_CALL nv_create_nano_timer(nv_state_t *nv, void *pTmrEvent,
    nv_nano_timer_t **pnv_nstimer)
{
    nv_nano_timer_t *nt;

    nt = kmem_zalloc(sizeof (*nt), KM_NOSLEEP);
    if (nt == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: Not able to create timer object \n");
        *pnv_nstimer = NULL;
        return;
    }

    mutex_init(&nt->lock, NULL, MUTEX_DRIVER, NULL);
    nt->nvis = NV_GET_NVIS(nv);
    nt->pTmrEvent = pTmrEvent;
    *pnv_nstimer = nt;
}

void NV_API_CALL nv_start_nano_timer(nv_state_t *nv, nv_nano_timer_t *nt,
    NvU64 time_ns)
{
    callout_id_t id;

    mutex_enter(&nt->lock);
    nv_nano_timer_cancel_locked(nt);
    mutex_exit(&nt->lock);

    id = timeout_generic(CALLOUT_NORMAL, nvidia_nano_timer_callback, nt,
        (hrtime_t)MIN(time_ns, (NvU64)INT64_MAX), 1, 0);

    mutex_enter(&nt->lock);
    nt->id = id;
    mutex_exit(&nt->lock);
}

void NV_API_CALL nv_cancel_nano_timer(nv_state_t *nv, nv_nano_timer_t *nt)
{
    mutex_enter(&nt->lock);
    nv_nano_timer_cancel_locked(nt);
    mutex_exit(&nt->lock);
}

void NV_API_CALL nv_destroy_nano_timer(nv_state_t *nv, nv_nano_timer_t *nt)
{
    nv_cancel_nano_timer(nv, nt);
    mutex_destroy(&nt->lock);
    kmem_free(nt, sizeof (*nt));
}

/*
 * Runtime D3 power management (RTD3/GC6).  illumos has no runtime power
 * management for PCI devices driven by ACPI power resources, so the dynamic
 * power paths report unavailable exactly as Linux does without
 * CONFIG_PM_RUNTIME.
 */
NvBool NV_API_CALL nv_dynamic_power_available(nv_state_t *nv)
{
    return NV_FALSE;
}

NV_STATUS NV_API_CALL nv_indicate_idle(nv_state_t *nv)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_indicate_not_idle(nv_state_t *nv)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_idle_holdoff(nv_state_t *nv)
{
}

void NV_API_CALL nv_allow_runtime_suspend(nv_state_t *nv)
{
}

void NV_API_CALL nv_disallow_runtime_suspend(nv_state_t *nv)
{
}

void NV_API_CALL nv_audio_dynamic_power(nv_state_t *nv)
{
}

/* illumos suspends to RAM through ACPI S3 only; there is no s2idle. */
NvBool NV_API_CALL nv_s2idle_pm_configured(void)
{
    return NV_FALSE;
}

/* SMBIOS chassis types 9 (laptop) and 10 (notebook). */
NvBool NV_API_CALL nv_is_chassis_notebook(void)
{
    smbios_struct_t s;
    smbios_chassis_t ch;

    if (ksmbios == NULL ||
        smbios_lookup_type(ksmbios, SMB_TYPE_CHASSIS, &s) != 0 ||
        smbios_info_chassis(ksmbios, s.smbstr_id, &ch) != 0)
        return NV_FALSE;

    return (ch.smbc_type == SMB_CHT_LAPTOP || ch.smbc_type == SMB_CHT_NOTEBOOK);
}

/*
 * Boot framebuffer (EFI GOP or VESA) that the console draws into, if it
 * lives in this GPU's BAR.
 */
void NV_API_CALL nv_get_screen_info(
    nv_state_t  *nv,
    NvU64       *pPhysicalAddress,
    NvU32       *pFbWidth,
    NvU32       *pFbHeight,
    NvU32       *pFbDepth,
    NvU32       *pFbPitch,
    NvU64       *pFbSize
)
{
    *pPhysicalAddress = 0;
    *pFbWidth = *pFbHeight = *pFbDepth = *pFbPitch = 0;
    *pFbSize = 0;

    if (fb_info.fb_type != FB_TYPE_RGB && fb_info.fb_type != FB_TYPE_INDEXED)
        return;

    if (!NV_IS_CONSOLE_MAPPED(nv, fb_info.paddr))
        return;

    *pPhysicalAddress = fb_info.paddr;
    *pFbWidth = fb_info.screen.x;
    *pFbHeight = fb_info.screen.y;
    *pFbDepth = fb_info.depth;
    *pFbPitch = fb_info.pitch;
    *pFbSize = fb_info.fb_size;
}

/*
 * The legacy VGA aperture is always decoded on x86 PCs; illumos does not
 * track memory resources claimed within it, so the requested range stands.
 */
void NV_API_CALL nv_get_updated_emu_seg(NvU32 *start, NvU32 *end)
{
}

/*
 * Xid errors: exposed as the nvidia:::dev-xid DTrace probe and to one
 * registered consumer, the counterparts of the Linux tracepoint and
 * nvidia_register_error_cb().
 */
typedef void (*nv_report_error_cb_t)(dev_info_t *, uint32_t, char *, size_t);

static nv_report_error_cb_t nv_error_cb_handle;
static kmutex_t nv_error_cb_lock;

void
nv_report_error_init(void)
{
    mutex_init(&nv_error_cb_lock, NULL, MUTEX_DRIVER, NULL);
}

void
nv_report_error_fini(void)
{
    mutex_destroy(&nv_error_cb_lock);
}

int
nvidia_register_error_cb(nv_report_error_cb_t cb)
{
    int rc = 0;

    if (cb == NULL)
        return (EINVAL);

    mutex_enter(&nv_error_cb_lock);
    if (nv_error_cb_handle != NULL)
        rc = EBUSY;
    else
        nv_error_cb_handle = cb;
    mutex_exit(&nv_error_cb_lock);

    return (rc);
}

int
nvidia_unregister_error_cb(void)
{
    int rc = 0;

    mutex_enter(&nv_error_cb_lock);
    if (nv_error_cb_handle == NULL)
        rc = EPERM;
    else
        nv_error_cb_handle = NULL;
    mutex_exit(&nv_error_cb_lock);

    return (rc);
}

NV_STATUS NV_API_CALL nv_log_error(nv_state_t *nv, NvU32 error_number,
    const char *format, va_list ap)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);
    char *buffer;
    size_t len;
    va_list ap2;

    va_copy(ap2, ap);
    len = vsnprintf(NULL, 0, format, ap2) + 1;
    va_end(ap2);

    buffer = kmem_alloc(len, nv_may_sleep() ? KM_SLEEP : KM_NOSLEEP);
    if (buffer == NULL)
        return NV_OK;

    (void) vsnprintf(buffer, len, format, ap);

    DTRACE_PROBE3(dev__xid, dev_info_t *, nvis->dip, uint32_t, error_number,
        char *, buffer);

    mutex_enter(&nv_error_cb_lock);
    if (nv_error_cb_handle != NULL)
        nv_error_cb_handle(nvis->dip, error_number, buffer, len);
    mutex_exit(&nv_error_cb_lock);

    kmem_free(buffer, len);
    return NV_OK;
}

/* CXL memory devices are not supported by illumos. */
void NV_API_CALL nv_pci_cxl_set_caching(nv_state_t *nv, NvBool enable)
{
}

#define NV_PCI_EXT_CAP_START_OFFSET         0x100
#define NV_PCI_EXT_CAP_END_OFFSET           0x1000
#define NV_PCI_EXT_CAP_MAX_ITERATIONS       \
    ((NV_PCI_EXT_CAP_END_OFFSET - NV_PCI_EXT_CAP_START_OFFSET) / 8)
#define NV_PCI_EXT_CAP_ID(hdr0)             ((hdr0) & 0xffff)
#define NV_PCI_EXT_CAP_NEXT_OFFSET(hdr0)    (((hdr0) & 0xfff00000) >> 20)
#define NV_PCI_EXT_CAP_ID_DVSEC             0x0023
#define NV_PCI_DVSEC_HEADER_1_OFFSET        0x4
#define NV_PCI_DVSEC_VENDOR_ID(hdr1)        ((hdr1) & 0xffff)
#define NV_PCI_DVSEC_LEN(hdr1)              (((hdr1) & 0xfff00000) >> 20)
#define NV_PCI_DVSEC_HEADER_2_OFFSET        0x8
#define NV_PCI_DVSEC_DESIGNATED_ID(hdr2)    ((hdr2) & 0xffff)
#define NV_PCI_DVSEC_NVIDIA_VENDOR_ID       0x10de
#define NV_PCI_DVSEC_GPU_PDI_DESIGNATED_ID  0x3
#define NV_PCI_DVSEC_GPU_PDI_CAP_LEN        0x14
#define NV_PCI_DVSEC_GPU_PDI_LOW_OFFSET     0xc
#define NV_PCI_DVSEC_GPU_PDI_HIGH_OFFSET    0x10

/*
 * Find NVIDIA's designated vendor-specific extended capability with the
 * given id, validating the chain against loops and out-of-range offsets.
 */
static NV_STATUS
nv_pci_find_nvidia_dvsec(void *handle, NvU16 target, NvU32 *offset,
    NvU32 *length)
{
    NvU32 cap = NV_PCI_EXT_CAP_START_OFFSET;
    NvU32 hdr0, hdr1, hdr2, i;

    for (i = 0; i < NV_PCI_EXT_CAP_MAX_ITERATIONS; i++)
    {
        if (cap < NV_PCI_EXT_CAP_START_OFFSET ||
            cap >= NV_PCI_EXT_CAP_END_OFFSET || (cap & 3) != 0)
            return NV_ERR_INVALID_OFFSET;

        if (os_pci_read_dword(handle, cap, &hdr0) != NV_OK)
            return NV_ERR_INVALID_READ;

        if (NV_PCI_EXT_CAP_ID(hdr0) == NV_PCI_EXT_CAP_ID_DVSEC)
        {
            if (os_pci_read_dword(handle, cap + NV_PCI_DVSEC_HEADER_1_OFFSET,
                    &hdr1) != NV_OK ||
                os_pci_read_dword(handle, cap + NV_PCI_DVSEC_HEADER_2_OFFSET,
                    &hdr2) != NV_OK)
                return NV_ERR_INVALID_READ;

            if (NV_PCI_DVSEC_VENDOR_ID(hdr1) == NV_PCI_DVSEC_NVIDIA_VENDOR_ID &&
                NV_PCI_DVSEC_DESIGNATED_ID(hdr2) == target)
            {
                *offset = cap;
                *length = NV_PCI_DVSEC_LEN(hdr1);
                return NV_OK;
            }
        }

        cap = NV_PCI_EXT_CAP_NEXT_OFFSET(hdr0);
        if (cap == 0)
            return NV_ERR_NOT_SUPPORTED;
    }

    return NV_ERR_CYCLE_DETECTED;
}

NV_STATUS NV_API_CALL nv_pci_read_gpu_pdi_from_dvsec(nv_state_t *nv, NvU64 *pdi)
{
    NvU32 offset, length, lo, hi;
    NvU64 value;
    NV_STATUS status;

    if (nv == NULL || pdi == NULL || nv->handle == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    status = nv_pci_find_nvidia_dvsec(nv->handle,
        NV_PCI_DVSEC_GPU_PDI_DESIGNATED_ID, &offset, &length);
    if (status != NV_OK)
        return status;

    if (length < NV_PCI_DVSEC_GPU_PDI_CAP_LEN)
        return NV_ERR_INVALID_DATA;

    if (offset > NV_PCI_EXT_CAP_END_OFFSET - NV_PCI_DVSEC_GPU_PDI_CAP_LEN)
        return NV_ERR_INVALID_OFFSET;

    if (os_pci_read_dword(nv->handle, offset + NV_PCI_DVSEC_GPU_PDI_LOW_OFFSET,
            &lo) != NV_OK ||
        os_pci_read_dword(nv->handle, offset + NV_PCI_DVSEC_GPU_PDI_HIGH_OFFSET,
            &hi) != NV_OK)
        return NV_ERR_INVALID_READ;

    value = ((NvU64)hi << 32) | lo;
    if (value == 0 || value == NV_U64_MAX)
        return NV_ERR_INVALID_DATA;

    *pdi = value;
    return NV_OK;
}

/*
 * Does the calling process hold an open GPU node for this GPU?  RM uses this
 * to confine some operations to processes that were granted the device.
 */
NvBool NV_API_CALL nv_is_gpu_accessible(nv_state_t *nv)
{
    nv_illumos_state_t *nvis = NV_GET_NVIS(nv);
    uf_info_t *fip = P_FINFO(curproc);
    int fd, nfiles;
    NvBool found = NV_FALSE;

    if (nv_ctl_dip == NULL || curproc == &p0)
        return NV_FALSE;

    mutex_enter(&fip->fi_lock);
    nfiles = fip->fi_nfiles;
    mutex_exit(&fip->fi_lock);

    for (fd = 0; fd < nfiles && !found; fd++)
    {
        file_t *fp = getf(fd);
        vnode_t *vp;

        if (fp == NULL)
            continue;

        vp = fp->f_vnode;
        if (vp != NULL && vp->v_type == VCHR &&
            getmajor(vp->v_rdev) == ddi_driver_major(nv_ctl_dip) &&
            NV_MINOR_CLONE(getminor(vp->v_rdev)) != 0 &&
            NV_MINOR_NODE(getminor(vp->v_rdev)) == nvis->minor_num)
        {
            nv_illumos_file_private_t *nvifp = nv_file_private_hold(vp->v_rdev);

            if (nvifp != NULL)
            {
                found = (nvifp->nvis == nvis);
                nv_file_private_rele(nvifp);
            }
        }

        releasef(fd);
    }

    return found;
}

/* os_info is the open instance returned by nv_get_file_private(). */
NvBool NV_API_CALL nv_match_gpu_os_info(nv_state_t *nv, void *os_info)
{
    nv_illumos_file_private_t *nvifp = os_info;

    return (nvifp != NULL && nvifp->kind == NV_NODE_GPU &&
            nvifp->nvis == NV_GET_NVIS(nv));
}

/*
 * Tegra SoC services (BPMP, DCE, host1x syncpoints, display clocks, IMP,
 * backlight).  They exist only on Tegra platforms; the results match the
 * Linux layer built without them.
 */
NV_STATUS NV_API_CALL nv_bpmp_send_mrq(nv_state_t *nv, NvU32 mrq,
    const void *request_data, NvU32 request_data_size, void *response_data,
    NvU32 response_data_size, NvS32 *response, NvS32 *api_ret)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_enable_clk(nv_state_t *nv, TEGRASOC_WHICH_CLK whichClkOS)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_disable_clk(nv_state_t *nv, TEGRASOC_WHICH_CLK whichClkOS)
{
}

NV_STATUS NV_API_CALL nv_get_max_freq(nv_state_t *nv,
    TEGRASOC_WHICH_CLK whichClkOS, NvU32 *pMaxFreqKHz)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_set_freq(nv_state_t *nv,
    TEGRASOC_WHICH_CLK whichClkOS, NvU32 reqFreqKHz)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_get_num_dpaux_instances(nv_state_t *nv,
    NvU32 *num_instances)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_get_disp_smmu_stream_ids(nv_state_t *nv,
    NvU32 *dispIsoStreamId, NvU32 *dispNisoStreamId)
{
    *dispIsoStreamId = nv->iommus.dispIsoStreamId;
    *dispNisoStreamId = nv->iommus.dispNisoStreamId;
}

NV_STATUS NV_API_CALL nv_get_syncpoint_aperture(NvU32 syncpointId,
    NvU64 *physAddr, NvU64 *limit, NvU32 *offset)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_get_tegra_brightness_level(nv_state_t *nv,
    NvU32 *brightness)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_set_tegra_brightness_level(nv_state_t *nv,
    NvU32 brightness)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_imp_get_import_data(TEGRA_IMP_IMPORT_DATA *tegra_imp_import_data)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_imp_get_uefi_data(nv_state_t *nv,
    NvU32 *iso_bw_kbps, NvU32 *floor_bw_kbps)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_imp_enable_disable_rfl(nv_state_t *nv, NvBool bEnable)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_imp_icc_set_bw(nv_state_t *nv, NvU32 avg_bw_kbps,
    NvU32 floor_bw_kbps)
{
    return NV_ERR_NOT_SUPPORTED;
}

NvU32 NV_API_CALL nv_tegra_get_rm_interface_type(NvU32 clientId)
{
    return 0;
}

NV_STATUS NV_API_CALL nv_tegra_dce_register_ipc_client(NvU32 clientType,
    void *handler_context, nvTegraDceClientIpcCallback handler,
    NvU32 *handle)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_tegra_dce_client_ipc_send_recv(NvU32 clientId,
    void *msg, NvU32 msgLength)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_tegra_dce_unregister_ipc_client(NvU32 clientId)
{
    return NV_ERR_NOT_SUPPORTED;
}

NvBool NV_API_CALL nv_pci_tegra_pm_init(nv_state_t *nv)
{
    return NV_FALSE;
}

void NV_API_CALL nv_pci_tegra_pm_deinit(nv_state_t *nv)
{
}

/*
 * GPU I2C buses are not exported to the illumos I2C framework, and the
 * transfer and bus-status hooks only serve SoC display I2C.
 */
void* NV_API_CALL nv_i2c_add_adapter(nv_state_t *nv, NvU32 port)
{
    return NULL;
}

void NV_API_CALL nv_i2c_del_adapter(nv_state_t *nv, void *data)
{
}

void NV_API_CALL nv_i2c_unregister_clients(nv_state_t *nv)
{
}

NV_STATUS NV_API_CALL nv_i2c_transfer(nv_state_t *nv, NvU32 port,
    NvU8 address, nv_i2c_msg_t *msgs, int num_msgs)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_i2c_bus_status(nv_state_t *nv, NvU32 port,
    NvS32 *scl, NvS32 *sda)
{
    return NV_ERR_NOT_SUPPORTED;
}
