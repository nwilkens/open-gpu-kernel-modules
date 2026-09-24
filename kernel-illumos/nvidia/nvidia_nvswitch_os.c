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
 * OS services for the NVSwitch library (nvswitch_os_*).  The library's
 * os_handle is an nvswitch_os_ctx_t, which carries the DMA state of one
 * switch.
 *
 * The NvSwitchRegDwords and NvSwitchBlacklist module parameters of the
 * Linux driver are nvidia.conf properties of the same name.
 */

#include "nvidia_nvswitch.h"
#include "ioctl_nvswitch.h"

#include <sys/sdt.h>
#include <sys/utsname.h>

/* 32-bit hex value, including the 0x prefix. */
#define NVSWITCH_REGKEY_VALUE_LEN       10

char *NvSwitchRegDwords;
char *NvSwitchBlacklist;

typedef struct nvswitch_dma_alloc
{
    list_node_t         link;
    caddr_t             kva;
    size_t              len;
    uint64_t            addr_hi;
    ddi_dma_handle_t    dma_hdl;
    ddi_acc_handle_t    acc_hdl;
} nvswitch_dma_alloc_t;

typedef struct nvswitch_dma_map
{
    list_node_t         link;
    caddr_t             cpu_addr;
    size_t              size;
    uint64_t            dma_addr;
    ddi_dma_handle_t    dma_hdl;
} nvswitch_dma_map_t;

typedef struct nvswitch_os_ctx
{
    dev_info_t         *dip;
    kmutex_t            dma_lock;
    uint64_t            dma_addr_hi;
    list_t              allocs;
    list_t              maps;
} nvswitch_os_ctx_t;

static const ddi_device_acc_attr_t nvswitch_dma_acc_attr = {
    DDI_DEVICE_ATTR_V0,
    DDI_NEVERSWAP_ACC,
    DDI_STRICTORDER_ACC
};

void *
nvswitch_os_ctx_create(dev_info_t *dip)
{
    nvswitch_os_ctx_t *ctx;

    ctx = kmem_zalloc(sizeof (*ctx), KM_SLEEP);
    ctx->dip = dip;
    /* The Linux default DMA mask, until the library sets one. */
    ctx->dma_addr_hi = UINT32_MAX;
    mutex_init(&ctx->dma_lock, NULL, MUTEX_DRIVER, NULL);
    list_create(&ctx->allocs, sizeof (nvswitch_dma_alloc_t),
        offsetof(nvswitch_dma_alloc_t, link));
    list_create(&ctx->maps, sizeof (nvswitch_dma_map_t),
        offsetof(nvswitch_dma_map_t, link));

    return (ctx);
}

static void
nvswitch_dma_map_free(nvswitch_dma_map_t *m)
{
    (void) ddi_dma_unbind_handle(m->dma_hdl);
    ddi_dma_free_handle(&m->dma_hdl);
    kmem_free(m, sizeof (*m));
}

static void
nvswitch_dma_alloc_free(nvswitch_dma_alloc_t *a)
{
    ddi_dma_mem_free(&a->acc_hdl);
    ddi_dma_free_handle(&a->dma_hdl);
    kmem_free(a, sizeof (*a));
}

/* Called after the library has unregistered the device. */
void
nvswitch_os_ctx_destroy(void *os_handle)
{
    nvswitch_os_ctx_t *ctx = os_handle;
    nvswitch_dma_map_t *m;
    nvswitch_dma_alloc_t *a;

    if (!list_is_empty(&ctx->maps) || !list_is_empty(&ctx->allocs))
        dev_err(ctx->dip, CE_WARN, "nvswitch DMA memory leaked by library");

    while ((m = list_remove_head(&ctx->maps)) != NULL)
        nvswitch_dma_map_free(m);
    while ((a = list_remove_head(&ctx->allocs)) != NULL)
        nvswitch_dma_alloc_free(a);

    list_destroy(&ctx->maps);
    list_destroy(&ctx->allocs);
    mutex_destroy(&ctx->dma_lock);
    kmem_free(ctx, sizeof (*ctx));
}

/*
 * DMA.  Contiguous memory comes from ddi_dma_mem_alloc(); each mapping is a
 * separate DMA handle bound to the caller's range, as dma_map_single() maps
 * any kernel buffer on Linux.
 */
static void
nvswitch_dma_attr_init(ddi_dma_attr_t *attr, uint64_t addr_hi, uint64_t align)
{
    bzero(attr, sizeof (*attr));
    attr->dma_attr_version = DMA_ATTR_V0;
    attr->dma_attr_addr_lo = 0;
    attr->dma_attr_addr_hi = addr_hi;
    attr->dma_attr_count_max = UINT64_MAX;
    attr->dma_attr_align = align;
    attr->dma_attr_burstsizes = 0xfff;
    attr->dma_attr_minxfer = 1;
    attr->dma_attr_maxxfer = UINT64_MAX;
    attr->dma_attr_seg = UINT64_MAX;
    attr->dma_attr_sgllen = 1;
    attr->dma_attr_granular = 1;
    attr->dma_attr_flags = 0;
}

static uint_t
nvswitch_dma_dir_flags(NvU32 direction)
{
    switch (direction)
    {
        case NVSWITCH_DMA_DIR_TO_SYSMEM:
            return (DDI_DMA_READ);
        case NVSWITCH_DMA_DIR_FROM_SYSMEM:
            return (DDI_DMA_WRITE);
        default:
            return (DDI_DMA_RDWR);
    }
}

NvlStatus
nvswitch_os_alloc_contig_memory(void *os_handle, void **virt_addr, NvU32 size,
    NvBool force_dma32)
{
    nvswitch_os_ctx_t *ctx = os_handle;
    nvswitch_dma_alloc_t *a;
    ddi_dma_attr_t attr;
    size_t real_len;

    if (ctx == NULL || virt_addr == NULL || size == 0 || !nv_may_sleep())
        return -NVL_BAD_ARGS;

    a = kmem_zalloc(sizeof (*a), KM_SLEEP);

    mutex_enter(&ctx->dma_lock);
    a->addr_hi = ctx->dma_addr_hi;
    mutex_exit(&ctx->dma_lock);
    if (force_dma32)
        a->addr_hi = MIN(a->addr_hi, UINT32_MAX);

    nvswitch_dma_attr_init(&attr, a->addr_hi, PAGESIZE);

    if (ddi_dma_alloc_handle(ctx->dip, &attr, DDI_DMA_SLEEP, NULL,
            &a->dma_hdl) != DDI_SUCCESS)
    {
        kmem_free(a, sizeof (*a));
        goto fail;
    }

    if (ddi_dma_mem_alloc(a->dma_hdl, size, &nvswitch_dma_acc_attr,
            DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL, &a->kva, &real_len,
            &a->acc_hdl) != DDI_SUCCESS)
    {
        ddi_dma_free_handle(&a->dma_hdl);
        kmem_free(a, sizeof (*a));
        goto fail;
    }

    a->len = real_len;
    bzero(a->kva, real_len);

    mutex_enter(&ctx->dma_lock);
    list_insert_tail(&ctx->allocs, a);
    mutex_exit(&ctx->dma_lock);

    *virt_addr = a->kva;
    return NVL_SUCCESS;

fail:
    dev_err(ctx->dip, CE_WARN, "nvidia-nvswitch: unable to allocate kernel "
        "memory");
    return -NVL_NO_MEM;
}

void
nvswitch_os_free_contig_memory(void *os_handle, void *virt_addr, NvU32 size)
{
    nvswitch_os_ctx_t *ctx = os_handle;
    nvswitch_dma_alloc_t *a;
    nvswitch_dma_map_t *m, *next;
    list_t stale;

    if (ctx == NULL || virt_addr == NULL)
        return;

    list_create(&stale, sizeof (nvswitch_dma_map_t),
        offsetof(nvswitch_dma_map_t, link));

    mutex_enter(&ctx->dma_lock);
    for (a = list_head(&ctx->allocs); a != NULL;
         a = list_next(&ctx->allocs, a))
    {
        if (a->kva == (caddr_t)virt_addr)
            break;
    }

    if (a != NULL)
    {
        list_remove(&ctx->allocs, a);

        for (m = list_head(&ctx->maps); m != NULL; m = next)
        {
            next = list_next(&ctx->maps, m);
            if (m->cpu_addr >= a->kva && m->cpu_addr < a->kva + a->len)
            {
                list_remove(&ctx->maps, m);
                list_insert_tail(&stale, m);
            }
        }
    }
    mutex_exit(&ctx->dma_lock);

    if (a == NULL)
    {
        dev_err(ctx->dip, CE_WARN, "nvidia-nvswitch: freeing unknown DMA "
            "memory %p", virt_addr);
        list_destroy(&stale);
        return;
    }

    if (!list_is_empty(&stale))
        dev_err(ctx->dip, CE_WARN, "nvidia-nvswitch: freeing mapped DMA memory");

    while ((m = list_remove_head(&stale)) != NULL)
        nvswitch_dma_map_free(m);
    list_destroy(&stale);

    nvswitch_dma_alloc_free(a);
}

NvlStatus
nvswitch_os_map_dma_region(void *os_handle, void *cpu_addr, NvU64 *dma_handle,
    NvU32 size, NvU32 direction)
{
    nvswitch_os_ctx_t *ctx = os_handle;
    nvswitch_dma_alloc_t *a;
    nvswitch_dma_map_t *m;
    ddi_dma_attr_t attr;
    ddi_dma_cookie_t cookie;
    uint_t ccount;
    uint64_t addr_hi;
    caddr_t va = cpu_addr;

    if (ctx == NULL || cpu_addr == NULL || dma_handle == NULL || size == 0 ||
        !nv_may_sleep())
        return -NVL_BAD_ARGS;

    /* A mapping of contiguous memory keeps that memory's address limit. */
    mutex_enter(&ctx->dma_lock);
    addr_hi = ctx->dma_addr_hi;
    for (a = list_head(&ctx->allocs); a != NULL;
         a = list_next(&ctx->allocs, a))
    {
        if (va >= a->kva && va < a->kva + a->len)
        {
            if (size > (size_t)(a->kva + a->len - va))
            {
                mutex_exit(&ctx->dma_lock);
                return -NVL_BAD_ARGS;
            }
            addr_hi = MIN(addr_hi, a->addr_hi);
            break;
        }
    }
    mutex_exit(&ctx->dma_lock);

    m = kmem_zalloc(sizeof (*m), KM_SLEEP);
    nvswitch_dma_attr_init(&attr, addr_hi, 1);

    if (ddi_dma_alloc_handle(ctx->dip, &attr, DDI_DMA_SLEEP, NULL,
            &m->dma_hdl) != DDI_SUCCESS)
    {
        kmem_free(m, sizeof (*m));
        goto fail;
    }

    if (ddi_dma_addr_bind_handle(m->dma_hdl, NULL, va, size,
            nvswitch_dma_dir_flags(direction) | DDI_DMA_CONSISTENT,
            DDI_DMA_SLEEP, NULL, &cookie, &ccount) != DDI_DMA_MAPPED)
    {
        ddi_dma_free_handle(&m->dma_hdl);
        kmem_free(m, sizeof (*m));
        goto fail;
    }

    if (ccount != 1 || cookie.dmac_size < size)
    {
        nvswitch_dma_map_free(m);
        goto fail;
    }

    m->cpu_addr = va;
    m->size = size;
    m->dma_addr = cookie.dmac_laddress;

    mutex_enter(&ctx->dma_lock);
    list_insert_tail(&ctx->maps, m);
    mutex_exit(&ctx->dma_lock);

    *dma_handle = m->dma_addr;
    return NVL_SUCCESS;

fail:
    dev_err(ctx->dip, CE_WARN, "nvidia-nvswitch: unable to create PCI DMA "
        "mapping");
    return -NVL_ERR_GENERIC;
}

NvlStatus
nvswitch_os_unmap_dma_region(void *os_handle, void *cpu_addr,
    NvU64 dma_handle, NvU32 size, NvU32 direction)
{
    nvswitch_os_ctx_t *ctx = os_handle;
    nvswitch_dma_map_t *m;

    if (ctx == NULL || cpu_addr == NULL)
        return -NVL_BAD_ARGS;

    mutex_enter(&ctx->dma_lock);
    for (m = list_head(&ctx->maps); m != NULL; m = list_next(&ctx->maps, m))
    {
        if (m->cpu_addr == (caddr_t)cpu_addr && m->dma_addr == dma_handle)
        {
            list_remove(&ctx->maps, m);
            break;
        }
    }
    mutex_exit(&ctx->dma_lock);

    if (m == NULL)
        return -NVL_BAD_ARGS;

    /* dma_unmap_single() leaves device writes visible to the CPU. */
    if (direction != NVSWITCH_DMA_DIR_FROM_SYSMEM)
        (void) ddi_dma_sync(m->dma_hdl, 0, m->size, DDI_DMA_SYNC_FORKERNEL);

    nvswitch_dma_map_free(m);
    return NVL_SUCCESS;
}

static NvlStatus
nvswitch_dma_sync(void *os_handle, NvU64 dma_handle, NvU32 size, uint_t flag)
{
    nvswitch_os_ctx_t *ctx = os_handle;
    nvswitch_dma_map_t *m;
    NvlStatus status = -NVL_BAD_ARGS;

    if (ctx == NULL)
        return -NVL_BAD_ARGS;

    if (size == 0)
        return NVL_SUCCESS;

    mutex_enter(&ctx->dma_lock);
    for (m = list_head(&ctx->maps); m != NULL; m = list_next(&ctx->maps, m))
    {
        if (dma_handle >= m->dma_addr && size <= m->size &&
            dma_handle - m->dma_addr <= m->size - size)
        {
            (void) ddi_dma_sync(m->dma_hdl, (off_t)(dma_handle - m->dma_addr),
                size, flag);
            status = NVL_SUCCESS;
            break;
        }
    }
    mutex_exit(&ctx->dma_lock);

    return status;
}

NvlStatus
nvswitch_os_sync_dma_region_for_cpu(void *os_handle, NvU64 dma_handle,
    NvU32 size, NvU32 direction)
{
    return nvswitch_dma_sync(os_handle, dma_handle, size,
        DDI_DMA_SYNC_FORKERNEL);
}

NvlStatus
nvswitch_os_sync_dma_region_for_device(void *os_handle, NvU64 dma_handle,
    NvU32 size, NvU32 direction)
{
    return nvswitch_dma_sync(os_handle, dma_handle, size, DDI_DMA_SYNC_FORDEV);
}

NvlStatus
nvswitch_os_set_dma_mask(void *os_handle, NvU32 dma_addr_width)
{
    nvswitch_os_ctx_t *ctx = os_handle;

    if (ctx == NULL || dma_addr_width == 0 || dma_addr_width > 64)
        return -NVL_BAD_ARGS;

    mutex_enter(&ctx->dma_lock);
    ctx->dma_addr_hi = (dma_addr_width == 64) ? UINT64_MAX :
        (1ULL << dma_addr_width) - 1;
    mutex_exit(&ctx->dma_lock);

    return NVL_SUCCESS;
}

/*
 * Registry.  Parsed like Linux: the first "name=value" occurrence of the
 * key, value in hex, up to ';' or the end of the string.
 */
static int
nvswitch_os_strtouint(const char *str, unsigned int *data)
{
    const char *p = str;
    unsigned long long val = 0;
    char c;

    *data = 0;

    while ((c = *p) != '\0')
    {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';

        if (c == 'x' && *str == '0' && p == str + 1)
            ;
        else if (c >= '0' && c <= '9')
            val = val * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f')
            val = val * 16 + (c - 'a' + 10);
        else
            return (EINVAL);
        p++;
    }

    if (val > 0xFFFFFFFFULL)
        return (EINVAL);

    *data = (unsigned int)val;
    return (0);
}

NvlStatus
nvswitch_os_read_registry_dword(void *os_handle, const char *name, NvU32 *data)
{
    char regkey_val[NVSWITCH_REGKEY_VALUE_LEN + 1];
    const char *regkey, *start, *end;
    size_t len;

    *data = 0;

    if (NvSwitchRegDwords == NULL || name == NULL)
        return -NVL_ERR_GENERIC;

    if ((regkey = strstr(NvSwitchRegDwords, name)) == NULL ||
        (regkey = strchr(regkey, '=')) == NULL)
        return -NVL_ERR_GENERIC;

    start = regkey + 1;
    if ((end = strchr(regkey, ';')) == NULL)
        end = regkey + strlen(regkey);

    len = (size_t)(end - start);
    if (len == 0 || len > NVSWITCH_REGKEY_VALUE_LEN)
        return -NVL_ERR_GENERIC;

    bcopy(start, regkey_val, len);
    regkey_val[len] = '\0';

    if (nvswitch_os_strtouint(regkey_val, data) != 0)
        return -NVL_ERR_GENERIC;

    return NVL_SUCCESS;
}

NvBool
nvswitch_os_is_uuid_in_blacklist(NvUuid *uuid)
{
    char uuid_string[NVSWITCH_UUID_STRING_LENGTH];
    char *list, *ptr, *token;
    NvBool found = NV_FALSE;

    if (NvSwitchBlacklist == NULL)
        return NV_FALSE;

    if (nvswitch_uuid_to_string(uuid, uuid_string, sizeof (uuid_string)) == 0)
        return NV_FALSE;

    if ((list = rm_remove_spaces(NvSwitchBlacklist)) == NULL)
        return NV_FALSE;

    ptr = list;
    while ((token = strsep(&ptr, ",")) != NULL)
    {
        if (strcmp(token, uuid_string) == 0)
        {
            found = NV_TRUE;
            break;
        }
    }

    os_free_mem(list);
    return found;
}

/*
 * Logging.
 */
static void
nvswitch_log(int log_level, char *msg)
{
    size_t len = strlen(msg);
    int ce;

    switch (log_level)
    {
        case NVSWITCH_DBG_LEVEL_ERROR:
            ce = CE_WARN;
            break;
        case NVSWITCH_DBG_LEVEL_WARN:
            ce = CE_NOTE;
            break;
        default:
            ce = CE_CONT;
            break;
    }

    if (ce == CE_CONT)
    {
        cmn_err(CE_CONT, "!%s", msg);
        return;
    }

    if (len > 0 && msg[len - 1] == '\n')
        msg[len - 1] = '\0';
    cmn_err(ce, "%s", msg);
}

void
nvswitch_os_print(int log_level, const char *fmt, ...)
{
    char buf[NVSWITCH_LOG_BUFFER_SIZE];
    va_list arglist;

    va_start(arglist, fmt);
    (void) vsnprintf(buf, sizeof (buf), fmt, arglist);
    va_end(arglist);

    nvswitch_log(log_level, buf);
}

/* Linux emits the nvswitch_dev_sxid tracepoint; this is its SDT probe. */
void
nvswitch_os_report_error(void *os_handle, NvU32 error_code,
    const char *fmt, ...)
{
    nvswitch_os_ctx_t *ctx = os_handle;
    char buf[NVSWITCH_LOG_BUFFER_SIZE];
    va_list arglist;

    if (ctx == NULL)
        return;

    va_start(arglist, fmt);
    (void) vsnprintf(buf, sizeof (buf), fmt, arglist);
    va_end(arglist);

    DTRACE_PROBE3(nvswitch__dev__sxid, dev_info_t *, ctx->dip,
        uint32_t, error_code, char *, buf);
}

void
nvswitch_os_assert_log(const char *fmt, ...)
{
    char buf[NVSWITCH_LOG_BUFFER_SIZE];
    va_list arglist;

    if (!nvlink_log_ratelimit())
        return;

    va_start(arglist, fmt);
    (void) vsnprintf(buf, sizeof (buf), fmt, arglist);
    va_end(arglist);

    nvswitch_log(NVSWITCH_DBG_LEVEL_ERROR, buf);
}

/*
 * Miscellaneous services.
 */
void
nvswitch_os_override_platform(void *os_handle, NvBool *rtlsim)
{
    /* Never run on RTL */
    *rtlsim = NV_FALSE;
}

NvU64
nvswitch_os_get_platform_time(void)
{
    return (NvU64)gethrtime();
}

NvU64
nvswitch_os_get_platform_time_epoch(void)
{
    timestruc_t ts;

    gethrestime(&ts);
    return (NvU64)ts.tv_sec * NANOSEC + (NvU64)ts.tv_nsec;
}

void
nvswitch_os_sleep(unsigned int ms)
{
    if (!nv_may_sleep() && ms > NV_MAX_ISR_DELAY_MS)
    {
        if (nvlink_log_ratelimit())
        {
            nvswitch_os_print(NVSWITCH_DBG_LEVEL_ERROR, "NVSwitch: requested "
                "sleep duration %u msec exceeded %u msec\n", ms,
                NV_MAX_ISR_DELAY_MS);
        }
        return;
    }

    (void) os_delay(ms);
}

int
nvswitch_os_is_admin(void)
{
    return (os_is_administrator() ? 1 : 0);
}

/* The utsname release, "5.11" on illumos. */
NvlStatus
nvswitch_os_get_os_version(NvU32 *pMajorVer, NvU32 *pMinorVer,
    NvU32 *pBuildNum)
{
    NvU32 ver[3] = { 0, 0, 0 };
    const char *p = utsname.release;
    int i;

    for (i = 0; i < 3 && *p != '\0'; i++)
    {
        while (*p >= '0' && *p <= '9')
            ver[i] = ver[i] * 10 + (NvU32)(*p++ - '0');
        if (*p != '.')
            break;
        p++;
    }

    if (pMajorVer != NULL)
        *pMajorVer = ver[0];
    if (pMinorVer != NULL)
        *pMinorVer = ver[1];
    if (pBuildNum != NULL)
        *pBuildNum = ver[2];

    return NVL_SUCCESS;
}

NvlStatus
nvswitch_os_get_pid(NvU32 *pPid)
{
    if (pPid != NULL)
        *pPid = (NvU32)curproc->p_pid;

    return NVL_SUCCESS;
}

void *
nvswitch_os_malloc_trace(NvLength size, const char *file, NvU32 line)
{
    void *ptr;

    return (os_alloc_mem(&ptr, size) == NV_OK) ? ptr : NULL;
}

void
nvswitch_os_free(void *ptr)
{
    os_free_mem(ptr);
}

NvLength
nvswitch_os_strlen(const char *str)
{
    return strlen(str);
}

int
nvswitch_os_strncmp(const char *s1, const char *s2, NvLength length)
{
    return strncmp(s1, s2, length);
}

void *
nvswitch_os_memset(void *dest, int value, NvLength size)
{
    return memset(dest, value, size);
}

void *
nvswitch_os_memcpy(void *dest, const void *src, NvLength size)
{
    return memcpy(dest, src, size);
}

NvU32
nvswitch_os_mem_read32(const volatile void *address)
{
    return *(const volatile NvU32 *)address;
}

void
nvswitch_os_mem_write32(volatile void *address, NvU32 data)
{
    *(volatile NvU32 *)address = data;
}

int
nvswitch_os_snprintf(char *dest, NvLength size, const char *fmt, ...)
{
    va_list arglist;
    size_t chars_written;

    va_start(arglist, fmt);
    chars_written = vsnprintf(dest, size, fmt, arglist);
    va_end(arglist);

    return (int)chars_written;
}

int
nvswitch_os_vsnprintf(char *buf, NvLength size, const char *fmt,
    va_list arglist)
{
    return (int)vsnprintf(buf, size, fmt, arglist);
}
