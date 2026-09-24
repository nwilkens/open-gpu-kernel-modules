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
 * OS primitives (os-interface.h) for the illumos kernel interface layer.
 */

#include "nv-illumos.h"

#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/thread.h>
#include <sys/cpuvar.h>
#include <sys/utsname.h>
#include <sys/policy.h>
#include <sys/zone.h>
#include <sys/random.h>
#include <sys/archsystm.h>
#include <sys/bootconf.h>
#include <sys/reboot.h>
#include <sys/debug.h>
#include <sys/stack.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/lgrp.h>
#include <vm/page.h>
#include <sys/gfx_private.h>
#include <sys/disp.h>

/* PAGESIZE is a variable for DDI drivers; nv_os_init() sets these. */
NvU64 os_page_size;
NvU64 os_max_page_size;
NvU64 os_page_mask;
NvU8  os_page_shift;

/* illumos is not a confidential-compute guest. */
NvBool os_cc_enabled = NV_FALSE;
NvBool os_cc_sev_snp_enabled = NV_FALSE;
NvBool os_cc_snp_vtom_enabled = NV_FALSE;
NvBool os_cc_tdx_enabled = NV_FALSE;
NvBool os_cc_sme_enabled = NV_FALSE;

/* There is no dma-buf framework on illumos. */
NvBool os_dma_buf_enabled = NV_FALSE;

NvBool os_imex_channel_is_supported = NV_TRUE;

/* The current debug display level (default to maximum debug level) */
NvU32 cur_debuglevel = 0xffffffff;

kmem_cache_t *nv_stack_cache;

void
nv_os_init(void)
{
    os_page_size = PAGESIZE;
    os_max_page_size = (NvU64)PAGESIZE << 9;        /* 2 MiB */
    os_page_mask = ~((NvU64)PAGEOFFSET);
    os_page_shift = PAGESHIFT;
}

NvBool
nv_may_sleep(void)
{
    return (!servicing_interrupt() && getpil() <= LOCK_LEVEL);
}

int
nv_stack_alloc(nvidia_stack_t **spp)
{
    nvidia_stack_t *sp;

    sp = kmem_cache_alloc(nv_stack_cache,
        nv_may_sleep() ? KM_SLEEP : KM_NOSLEEP);
    if (sp == NULL)
    {
        *spp = NULL;
        return (ENOMEM);
    }

    sp->size = sizeof (sp->stack);
    sp->top = sp->stack + sp->size;
    *spp = sp;
    return (0);
}

void
nv_stack_free(nvidia_stack_t *sp)
{
    if (sp != NULL)
        kmem_cache_free(nv_stack_cache, sp);
}

void NV_API_CALL os_disable_console_access(void)
{
}

void NV_API_CALL os_enable_console_access(void)
{
}

NvBool NV_API_CALL os_is_administrator(void)
{
    return (drv_priv(CRED()) == 0);
}

NvBool NV_API_CALL os_check_access(RsAccessRight accessRight)
{
    switch (accessRight)
    {
        case RS_ACCESS_PERFMON:
            return (secpolicy_cpc_cpu(CRED()) == 0 || os_is_administrator());
        case RS_ACCESS_NICE:
            return (secpolicy_setpriority(CRED()) == 0);
        default:
            return NV_FALSE;
    }
}

char* NV_API_CALL os_string_copy(char *dst, const char *src)
{
    return strcpy(dst, src);
}

NvU32 NV_API_CALL os_string_length(const char *str)
{
    return (NvU32)strlen(str);
}

NvU32 NV_API_CALL os_strtoul(const char *str, char **endp, NvU32 base)
{
    unsigned long result = 0;
    char *end = (char *)str;

    (void) ddi_strtoul(str, &end, (int)base, &result);
    if (endp != NULL)
        *endp = end;

    return (NvU32)result;
}

NvS32 NV_API_CALL os_string_compare(const char *str1, const char *str2)
{
    return strcmp(str1, str2);
}

void *NV_API_CALL os_mem_copy(void *dst, const void *src, NvU32 length)
{
    return memcpy(dst, src, length);
}

NV_STATUS NV_API_CALL os_memcpy_from_user(void *to, const void *from, NvU32 n)
{
    return (copyin(from, to, n) != 0) ? NV_ERR_INVALID_ADDRESS : NV_OK;
}

NV_STATUS NV_API_CALL os_memcpy_to_user(void *to, const void *from, NvU32 n)
{
    return (copyout(from, to, n) != 0) ? NV_ERR_INVALID_ADDRESS : NV_OK;
}

void* NV_API_CALL os_mem_set(void *dst, NvU8 c, NvU32 length)
{
    return memset(dst, c, length);
}

NvS32 NV_API_CALL os_mem_cmp(const NvU8 *buf0, const NvU8 *buf1, NvU32 length)
{
    return memcmp(buf0, buf1, length);
}

/*
 * RM allocates from interrupt context and frees without a size, so a header
 * records the size.  The header keeps 16-byte alignment for the payload.
 * Large requests do not wait for memory: a user-controlled size must fail
 * rather than block the thread indefinitely.
 */
#define NV_ALLOC_HDR            16
#define NV_KMEM_SLEEP_LIMIT     (128 * 1024)

NV_STATUS NV_API_CALL os_alloc_mem(void **address, NvU64 size)
{
    size_t total;
    int kmflag;
    uint64_t *hdr;

    if (address == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    *address = NULL;

    if (size > (NvU64)(SIZE_MAX - NV_ALLOC_HDR))
        return NV_ERR_INVALID_ARGUMENT;

    total = (size_t)size + NV_ALLOC_HDR;

    if (!nv_may_sleep())
        kmflag = KM_NOSLEEP;
    else if (total <= NV_KMEM_SLEEP_LIMIT)
        kmflag = KM_SLEEP;
    else
        kmflag = KM_NOSLEEP | KM_NORMALPRI;

    hdr = kmem_alloc(total, kmflag);
    if (hdr == NULL)
        return NV_ERR_NO_MEMORY;

    hdr[0] = total;
    *address = (uint8_t *)hdr + NV_ALLOC_HDR;

    return NV_OK;
}

void NV_API_CALL os_free_mem(void *address)
{
    uint64_t *hdr;

    if (address == NULL)
        return;

    hdr = (uint64_t *)((uint8_t *)address - NV_ALLOC_HDR);
    kmem_free(hdr, (size_t)hdr[0]);
}

NV_STATUS NV_API_CALL os_get_system_time(NvU32 *seconds, NvU32 *useconds)
{
    timestruc_t ts;

    gethrestime(&ts);
    *seconds = (NvU32)ts.tv_sec;
    *useconds = (NvU32)(ts.tv_nsec / (NANOSEC / MICROSEC));

    return NV_OK;
}

NvU64 NV_API_CALL os_get_monotonic_time_ns_hr(void)
{
    return (NvU64)gethrtime();
}

NvU64 NV_API_CALL os_get_monotonic_time_ns(void)
{
    return (NvU64)ddi_get_lbolt64() * (NvU64)nsec_per_tick;
}

NvU64 NV_API_CALL os_get_monotonic_tick_resolution_ns(void)
{
    return (NvU64)nsec_per_tick;
}

/*
 * Sleep when allowed; otherwise spin.  Waits shorter than a clock tick spin
 * even in thread context, matching the Linux usleep_range()/udelay() split.
 */
NV_STATUS NV_API_CALL os_delay_us(NvU32 MicroSeconds)
{
    clock_t ticks;

    if (!nv_may_sleep() || MicroSeconds < (NvU32)(nsec_per_tick / 1000))
    {
        drv_usecwait(MicroSeconds);
        return NV_OK;
    }

    ticks = drv_usectohz(MicroSeconds);
    delay(MAX(ticks, 1));
    return NV_OK;
}

NV_STATUS NV_API_CALL os_delay(NvU32 MilliSeconds)
{
    if (!nv_may_sleep())
    {
        while (MilliSeconds-- > 0)
            drv_usecwait(1000);
        return NV_OK;
    }

    delay(MAX(drv_usectohz((clock_t)MilliSeconds * 1000), 1));
    return NV_OK;
}

NvU64 NV_API_CALL os_get_cpu_frequency(void)
{
    return (NvU64)CPU->cpu_curr_clock;
}

NvU32 NV_API_CALL os_get_current_process(void)
{
    return (NvU32)curproc->p_pid;
}

void NV_API_CALL os_get_current_process_name(char *buf, NvU32 len)
{
    if (len == 0)
        return;

    (void) strlcpy(buf, PTOU(curproc)->u_comm, len);
}

/* illumos has no IOMMU shared virtual addressing interface. */
NV_STATUS NV_API_CALL os_iommu_sva_bind(void *arg, void **handle, NvU32 *pasid)
{
    nv_state_t *nv = arg;

    if (pasid != NULL)
        *pasid = 0;
    if (handle != NULL)
        *handle = NULL;

    NV_DEV_PRINTF(NV_DBG_ERRORS, nv, "IOMMU SVA bind failed\n");
    return NV_ERR_INVALID_STATE;
}

void NV_API_CALL os_iommu_sva_unbind(void *handle)
{
}

NV_STATUS NV_API_CALL os_get_current_thread(NvU64 *threadId)
{
    if (servicing_interrupt())
        *threadId = 0;
    else
        *threadId = (NvU64)curthread->t_did;

    return NV_OK;
}

/*
 * Debug and logging.  cmn_err(CE_CONT) continues the previous line, which is
 * what RM expects when a message is built from several nv_printf() calls.
 */
void NV_API_CALL out_string(const char *str)
{
    cmn_err(CE_CONT, "%s", str);
}

int NV_API_CALL nv_vprintf(NvU32 debuglevel, const char *printf_format, va_list arglist)
{
    if (debuglevel < ((cur_debuglevel >> 4) & 0x3))
        return 0;

    if (printf_format[0] == '\0')
        return 0;

    vcmn_err(CE_CONT, printf_format, arglist);
    return (int)strlen(printf_format);
}

int NV_API_CALL nv_printf(NvU32 debuglevel, const char *printf_format, ...)
{
    va_list arglist;
    int chars_written;

    va_start(arglist, printf_format);
    chars_written = nv_vprintf(debuglevel, printf_format, arglist);
    va_end(arglist);

    return chars_written;
}

int NV_API_CALL nv_dev_vprintf(struct nv_state_t *nv, NV_LOG_LEVEL level,
    const char *printf_format, va_list arglist)
{
    nv_illumos_state_t *nvis;
    char buf[256];
    int ce;

    if (nv == NULL || (nvis = NV_GET_NVIS(nv)) == NULL || nvis->dip == NULL)
        return 0;

    switch (level)
    {
        case NV_LOG_LEVEL_ALERT:
        case NV_LOG_LEVEL_CRIT:
        case NV_LOG_LEVEL_ERROR:
            ce = CE_WARN;
            break;
        case NV_LOG_LEVEL_WARNING:
        case NV_LOG_LEVEL_NOTICE:
            ce = CE_NOTE;
            break;
        default:
            ce = CE_CONT;
            break;
    }

    (void) vsnprintf(buf, sizeof (buf), printf_format, arglist);
    dev_err(nvis->dip, ce, "%s", buf);
    return (int)strlen(buf);
}

int NV_API_CALL nv_dev_printf(struct nv_state_t *nv, NV_LOG_LEVEL level,
    const char *printf_format, ...)
{
    va_list arglist;
    int chars_written;

    va_start(arglist, printf_format);
    chars_written = nv_dev_vprintf(nv, level, printf_format, arglist);
    va_end(arglist);

    return chars_written;
}

NvS32 NV_API_CALL os_snprintf(char *buf, NvU32 size, const char *fmt, ...)
{
    va_list arglist;
    size_t chars_written;

    va_start(arglist, fmt);
    chars_written = vsnprintf(buf, size, fmt, arglist);
    va_end(arglist);

    return (NvS32)chars_written;
}

NvS32 NV_API_CALL os_vsnprintf(char *buf, NvU32 size, const char *fmt, va_list arglist)
{
    return (NvS32)vsnprintf(buf, size, fmt, arglist);
}

void NV_API_CALL os_log_error(const char *fmt, va_list ap)
{
    vcmn_err(CE_WARN, fmt, ap);
}

void NV_API_CALL os_io_write_byte(NvU32 address, NvU8 value)
{
    outb((int)address, value);
}

void NV_API_CALL os_io_write_word(NvU32 address, NvU16 value)
{
    outw((int)address, value);
}

void NV_API_CALL os_io_write_dword(NvU32 address, NvU32 value)
{
    outl((int)address, value);
}

NvU8 NV_API_CALL os_io_read_byte(NvU32 address)
{
    return inb((int)address);
}

NvU16 NV_API_CALL os_io_read_word(NvU32 address)
{
    return inw((int)address);
}

NvU32 NV_API_CALL os_io_read_dword(NvU32 address)
{
    return inl((int)address);
}

/*
 * Map a physical range (typically a BAR) into kernel virtual space.
 */
void* NV_API_CALL os_map_kernel_space(NvU64 start, NvU64 size_bytes, NvU32 mode)
{
    uint32_t gfxp_mode;

    if (start == 0 && mode != NV_MEMORY_CACHED)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: os_map_kernel_space: won't map address 0x%0llx UC!\n", start);
        return NULL;
    }

    if (!nv_may_sleep())
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: os_map_kernel_space: can't map 0x%0llx, invalid context!\n",
            start);
        os_dbg_breakpoint();
        return NULL;
    }

    switch (mode)
    {
        case NV_MEMORY_CACHED:
            gfxp_mode = GFXP_MEMORY_CACHED;
            break;
        case NV_MEMORY_WRITECOMBINED:
            gfxp_mode = rm_disable_iomap_wc() ?
                GFXP_MEMORY_UNCACHED : GFXP_MEMORY_WRITECOMBINED;
            break;
        case NV_MEMORY_UNCACHED:
        case NV_MEMORY_DEFAULT:
            gfxp_mode = GFXP_MEMORY_UNCACHED;
            break;
        default:
            nv_printf(NV_DBG_ERRORS,
                "NVRM: os_map_kernel_space: unsupported mode!\n");
            return NULL;
    }

    return gfxp_map_kernel_space(start, (size_t)size_bytes, gfxp_mode);
}

void NV_API_CALL os_unmap_kernel_space(void *addr, NvU64 size_bytes)
{
    gfxp_unmap_kernel_space(addr, (size_t)size_bytes);
}

/* x86 caches are coherent with device DMA; there is nothing to flush. */
NV_STATUS NV_API_CALL os_flush_cpu_cache_all(void)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_flush_user_cache(void)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL os_flush_cpu_write_combine_buffer(void)
{
    __asm__ __volatile__("sfence" ::: "memory");
}

void NV_API_CALL os_dbg_init(void)
{
    NvU32 new_debuglevel;
    nvidia_stack_t *sp = NULL;

    if (nv_stack_alloc(&sp) != 0)
        return;

    if (rm_read_registry_dword(sp, NULL, "ResmanDebugLevel",
                               &new_debuglevel) == NV_OK)
    {
        if (new_debuglevel != (NvU32)~0)
            cur_debuglevel = new_debuglevel;
    }

    nv_stack_free(sp);
}

void NV_API_CALL os_dbg_set_level(NvU32 new_debuglevel)
{
    nv_printf(NV_DBG_SETUP, "NVRM: Changing debuglevel from 0x%x to 0x%x\n",
        cur_debuglevel, new_debuglevel);
    cur_debuglevel = new_debuglevel;
}

/*
 * 64-bit illumos processes also map above the amd64 VA hole (the stack lives
 * just below USERLIMIT), but only the low canonical half is addressable by the
 * GPU with CPU-identical virtual addresses.
 */
NvU64 NV_API_CALL os_get_max_user_va(void)
{
    return 0x0000800000000000ULL;
}

NV_STATUS NV_API_CALL os_schedule(void)
{
    if (!nv_may_sleep())
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: os_schedule: Attempted to yield"
                                 " the CPU while in atomic or interrupt"
                                 " context\n");
        return NV_ERR_ILLEGAL_ACTION;
    }

    delay(1);
    return NV_OK;
}

void NV_API_CALL os_dbg_breakpoint(void)
{
    if (NVreg_EnableDbgBreakpoint == 0)
        return;

    /* Only drop into kmdb when it is loaded; debug_enter() would reboot. */
    if (boothowto & RB_DEBUG)
        debug_enter("NVRM: breakpoint");
}

NvU32 NV_API_CALL os_get_cpu_number(void)
{
    return (NvU32)CPU->cpu_id;
}

NvU32 NV_API_CALL os_get_cpu_count(void)
{
    return (NvU32)max_ncpus;
}

NvBool NV_API_CALL os_is_efi_enabled(void)
{
    return ddi_prop_exists(DDI_DEV_T_ANY, ddi_root_node(),
        DDI_PROP_DONTPASS, "efi-systab") ? NV_TRUE : NV_FALSE;
}

void NV_API_CALL os_dump_stack(void)
{
    traceback((caddr_t)getfp());
}

NV_STATUS NV_API_CALL os_get_version_info(os_version_info *pOsVersionInfo)
{
    unsigned long major = 0, minor = 0;
    char *end;

    (void) ddi_strtoul(utsname.release, &end, 10, &major);
    if (*end == '.')
        (void) ddi_strtoul(end + 1, NULL, 10, &minor);

    pOsVersionInfo->os_major_version = (NvU32)major;
    pOsVersionInfo->os_minor_version = (NvU32)minor;
    pOsVersionInfo->os_build_number = 0;
    pOsVersionInfo->os_build_version_str = utsname.release;
    pOsVersionInfo->os_build_date_plus_str = utsname.version;

    return NV_OK;
}

NV_STATUS NV_API_CALL os_get_is_openrm(NvBool *bIsOpenRm)
{
    *bIsOpenRm = NV_TRUE;
    return NV_OK;
}

NvBool NV_API_CALL os_is_bif_reset_supported(void *pOsGpuInfo)
{
    return NV_TRUE;
}

NvBool NV_API_CALL os_is_vgx_hyper(void)
{
    return NV_FALSE;
}

NV_STATUS NV_API_CALL os_inject_vgx_msi(NvU16 guestID, NvU64 msiAddr, NvU32 msiData)
{
    return NV_ERR_NOT_SUPPORTED;
}

NvBool NV_API_CALL os_is_grid_supported(void)
{
    return NV_FALSE;
}

NvU32 NV_API_CALL os_get_grid_csp_support(void)
{
    return 0;
}

void NV_API_CALL os_bug_check(NvU32 bugCode, const char *bugCodeStr)
{
    panic("NVRM: %s (0x%x)", bugCodeStr, bugCode);
}

NV_STATUS NV_API_CALL os_get_euid(NvU32 *pSecToken)
{
    *pSecToken = (NvU32)crgetuid(CRED());
    return NV_OK;
}

void NV_API_CALL os_add_record_for_crashLog(void *pbuffer, NvU32 size)
{
}

void NV_API_CALL os_delete_record_for_crashLog(void *pbuffer)
{
}

NV_STATUS NV_API_CALL os_call_vgpu_vfio(void *pvgpu_vfio_info, NvU32 cmd_type)
{
    return NV_ERR_NOT_SUPPORTED;
}

/*
 * Node-local page allocation is only used for GPU memory onlined as a NUMA
 * node (coherent Grace platforms), which illumos does not support; see
 * os_numa_add_gpu_memory().
 */
NV_STATUS NV_API_CALL os_alloc_pages_node(NvS32 nid, NvU32 size, NvU32 flags,
    NvU64 *pAddress)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_get_page(NvU64 address)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_put_page(NvU64 address)
{
    return NV_ERR_NOT_SUPPORTED;
}

NvU32 NV_API_CALL os_get_page_refcount(NvU64 address)
{
    return 0;
}

NvU32 NV_API_CALL os_count_tail_pages(NvU64 address)
{
    return 1;
}

void NV_API_CALL os_free_pages_phys(NvU64 address, NvU32 size)
{
}

NV_STATUS NV_API_CALL os_numa_memblock_size(NvU64 *memblock_size)
{
    if (nv_ctl_device.numa_memblock_size == 0)
        return NV_ERR_INVALID_STATE;

    *memblock_size = nv_ctl_device.numa_memblock_size;
    return NV_OK;
}

NV_STATUS NV_API_CALL os_get_random_bytes(NvU8 *bytes, NvU16 numBytes)
{
    if (random_get_bytes(bytes, numBytes) != 0)
        return NV_ERR_NOT_READY;

    return NV_OK;
}

NvU32 NV_API_CALL os_get_current_process_flags(void)
{
    NvU32 flags = OS_CURRENT_PROCESS_FLAG_NONE;

    if (curproc->p_flag & SEXITING)
        flags |= OS_CURRENT_PROCESS_FLAG_EXITING;

    if (curproc == &p0 || ttolwp(curthread) == NULL)
        flags |= OS_CURRENT_PROCESS_FLAG_KERNEL_THREAD;

    return flags;
}

/* Tegra SoC services do not exist on illumos x86 platforms. */
NV_STATUS NV_API_CALL os_get_tegra_platform(NvU32 *mode)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_tegra_igpu_perf_boost(void *handle, NvBool enable,
    NvU32 duration, int boost_type)
{
    return NV_ERR_NOT_SUPPORTED;
}

/* The driver keeps no page pools, so nothing is held back for reclaim. */
NvU64 NV_API_CALL os_get_reclaimable_memory_usage(void)
{
    return 0;
}

/*
 * NUMA node ids are the platform's memory lgroup handles.  RM only asks about
 * nodes it created with os_numa_add_gpu_memory(), which illumos does not
 * support, so an unknown node is reported as invalid.
 */
NV_STATUS NV_API_CALL os_get_numa_node_memory_usage(NvS32 node_id,
    NvU64 *free_memory_bytes, NvU64 *total_memory_bytes)
{
    lgrp_mem_size_t total, avail;

    if (node_id < 0)
        return NV_ERR_INVALID_ARGUMENT;

    total = lgrp_mem_size((lgrp_id_t)node_id, LGRP_MEM_SIZE_INSTALL);
    if (total == 0)
    {
        nv_printf(NV_DBG_ERRORS, "Invalid NUMA node ID\n");
        return NV_ERR_INVALID_ARGUMENT;
    }
    avail = lgrp_mem_size((lgrp_id_t)node_id, LGRP_MEM_SIZE_FREE);

    *total_memory_bytes = (NvU64)total;
    *free_memory_bytes = (NvU64)avail;
    return NV_OK;
}

/* illumos has no interface for onlining driver-managed memory. */
NV_STATUS NV_API_CALL os_numa_add_gpu_memory(void *handle, NvU64 offset,
    NvU64 size, NvU32 *nodeId)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_numa_remove_gpu_memory(void *handle, NvU64 offset,
    NvU64 size, NvU32 nodeId)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_offline_page_at_address(NvU64 address)
{
    int err;

    nv_printf(NV_DBG_INFO, "NVRM: offlining page at address: 0x%llx\n",
        address);

    err = page_retire((uint64_t)address, PR_UE);
    if (err != 0 && err != PR_RETIRED)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: page_retire() failed (%d) for address: 0x%llx\n",
            err, address);
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NV_OK;
}

/*
 * Zones share the global pid space; a pid is visible from its own zone and
 * from the global zone, which is how Linux pid namespaces are used by RM.
 */
typedef struct {
    pid_t       pid;
    zoneid_t    zoneid;
} nv_pid_info_t;

void* NV_API_CALL os_get_pid_info(void)
{
    nv_pid_info_t *info;

    info = kmem_alloc(sizeof (*info), KM_NOSLEEP);
    if (info == NULL)
        return NULL;

    info->pid = curproc->p_pid;
    info->zoneid = getzoneid();
    return info;
}

void NV_API_CALL os_put_pid_info(void *pid_info)
{
    if (pid_info != NULL)
        kmem_free(pid_info, sizeof (nv_pid_info_t));
}

NV_STATUS NV_API_CALL os_find_ns_pid(void *pid_info, NvU32 *ns_pid)
{
    nv_pid_info_t *info = pid_info;
    zoneid_t zid;

    if (info == NULL || ns_pid == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    zid = getzoneid();
    if (zid != GLOBAL_ZONEID && zid != info->zoneid)
    {
        *ns_pid = 0;
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    *ns_pid = (NvU32)info->pid;
    return NV_OK;
}

NvBool NV_API_CALL os_is_init_ns(void)
{
    return (getzoneid() == GLOBAL_ZONEID);
}

NV_STATUS NV_API_CALL os_device_vm_present(void)
{
    return NV_ERR_NOT_SUPPORTED;
}

/*
 * System suspend reaches the driver as DDI_SUSPEND from the kernel itself, so
 * video memory can be preserved without userspace hooks.
 */
NvBool NV_API_CALL os_supports_kernel_suspend_notifiers(void)
{
    return NV_TRUE;
}

/* illumos has no cgroups; RM only calls this when a cgroup backend exists. */
void* NV_API_CALL os_cgroup_for_pid(int pid, void *pidInfo, int impl)
{
    return NULL;
}
