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
 * Firmware tables, temporary files and NVSwitch discovery for RM.
 */

#include "nv-illumos.h"

#include <sys/bootconf.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/sunndi.h>
#include <sys/gfx_private.h>

static NvBool os_verify_checksum(const NvU8 *pMappedAddr, NvU32 length)
{
    NvU8 sum = 0;
    NvU32 iter;

    for (iter = 0; iter < length; iter++)
        sum += pMappedAddr[iter];

    return (sum == 0);
}

static NvBool os_verify_smbios_entry(const NvU8 *p)
{
    if (memcmp(p, "_SM3_", 5) == 0)
        return (p[6] > 0 && p[6] < 32 && os_verify_checksum(p, p[6]));

    if (memcmp(p, "_SM_", 4) == 0)
        return (p[5] > 0 && p[5] < 32 && os_verify_checksum(p, p[5]) &&
                memcmp(p + 16, "_DMI_", 5) == 0 &&
                os_verify_checksum(p + 16, 15));

    return NV_FALSE;
}

#define SMBIOS_LEGACY_BASE 0xF0000
#define SMBIOS_LEGACY_SIZE 0x10000

/*
 * On UEFI systems the loader records the entry point in the "smbios-address"
 * boot property; legacy BIOS systems keep it in the F-segment.
 */
NV_STATUS NV_API_CALL os_get_smbios_header(NvU64 *pSmbsAddr)
{
    NV_STATUS status = NV_ERR_OPERATING_SYSTEM;
    int64_t addr;
    NvU8 *map;

    addr = ddi_prop_get_int64(DDI_DEV_T_ANY, ddi_root_node(),
        DDI_PROP_DONTPASS, "smbios-address", 0);
    if (addr != 0)
    {
        map = os_map_kernel_space((NvU64)addr, 32, NV_MEMORY_CACHED);
        if (map == NULL)
            return NV_ERR_INSUFFICIENT_RESOURCES;

        if (os_verify_smbios_entry(map))
        {
            *pSmbsAddr = (NvU64)addr;
            status = NV_OK;
        }
        os_unmap_kernel_space(map, 32);
        return status;
    }

    map = os_map_kernel_space(SMBIOS_LEGACY_BASE, SMBIOS_LEGACY_SIZE,
        NV_MEMORY_CACHED);
    if (map == NULL)
        return NV_ERR_INSUFFICIENT_RESOURCES;

    for (NvU32 off = 0; off + 32 <= SMBIOS_LEGACY_SIZE; off += 16)
    {
        if (os_verify_smbios_entry(map + off))
        {
            *pSmbsAddr = SMBIOS_LEGACY_BASE + off;
            status = NV_OK;
            break;
        }
    }

    os_unmap_kernel_space(map, SMBIOS_LEGACY_SIZE);
    return status;
}

NV_STATUS NV_API_CALL os_get_acpi_rsdp_from_uefi(NvU32 *pRsdpAddr)
{
    int64_t addr;

    if (pRsdpAddr == NULL)
        return NV_ERR_INVALID_STATE;

    *pRsdpAddr = 0;

    if (!os_is_efi_enabled())
        return NV_ERR_NOT_SUPPORTED;

    addr = ddi_prop_get_int64(DDI_DEV_T_ANY, ddi_root_node(),
        DDI_PROP_DONTPASS, "acpi-root-tab", 0);
    if (addr == 0)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: RSDP Not found!\n");
        return NV_ERR_OPERATING_SYSTEM;
    }

    *pRsdpAddr = (NvU32)addr;
    return NV_OK;
}

/*
 * Temporary files back video memory preservation across suspend.  The file is
 * created exclusively under NVreg_TemporaryFilePath (default /tmp, which is
 * swap-backed tmpfs like Linux's shmem) and unlinked immediately, so it
 * disappears with its last reference.
 */
NV_STATUS NV_API_CALL os_allocate_temporary_file(void **ppFile, NvU64 size)
{
    const char *dir = (NVreg_TemporaryFilePath != NULL) ?
        NVreg_TemporaryFilePath : "/tmp";
    static volatile uint32_t seq;
    char path[MAXPATHLEN];
    vnode_t *vp = NULL;
    int err;

    if (curproc == &p0)
        return NV_ERR_OPERATING_SYSTEM;

    (void) snprintf(path, sizeof (path), "%s/.nvidia-tmp-%d-%u", dir,
        (int)curproc->p_pid, atomic_inc_32_nv(&seq));

    err = vn_open(path, UIO_SYSSPACE, FCREAT | FEXCL | FREAD | FWRITE |
        FOFFMAX, 0600, &vp, CRCREAT, 0);
    if (err != 0)
    {
        if (NVreg_TemporaryFilePath != NULL)
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: The temporary file path specified via the NVreg_TemporaryFilePath\n"
                "NVRM: module parameter could not be opened (error %d).\n", err);
        }
        return NV_ERR_OPERATING_SYSTEM;
    }

    (void) vn_remove(path, UIO_SYSSPACE, RMFILE);

    if (size > 0)
    {
        struct flock64 fl;

        bzero(&fl, sizeof (fl));
        fl.l_whence = 0;
        fl.l_start = 0;
        fl.l_len = (off64_t)size;

        err = VOP_SPACE(vp, F_ALLOCSP, &fl, FWRITE | FOFFMAX, 0, kcred, NULL);
        if (err != 0 && err != EINVAL && err != ENOTSUP && err != ENOSYS)
        {
            (void) VOP_CLOSE(vp, FREAD | FWRITE, 1, 0, kcred, NULL);
            VN_RELE(vp);
            return NV_ERR_INSUFFICIENT_RESOURCES;
        }
    }

    *ppFile = vp;
    return NV_OK;
}

void NV_API_CALL os_close_file(void *pFile)
{
    vnode_t *vp = pFile;

    (void) VOP_CLOSE(vp, FREAD | FWRITE, 1, 0, kcred, NULL);
    VN_RELE(vp);
}

static NV_STATUS os_file_rdwr(enum uio_rw rw, void *pFile, NvU8 *pBuffer,
    NvU64 size, NvU64 offset)
{
    vnode_t *vp = pFile;

    while (size > 0)
    {
        ssize_t chunk = (ssize_t)MIN(size, (NvU64)(1ULL << 30));
        ssize_t resid = 0;
        int err;

        err = vn_rdwr(rw, vp, (caddr_t)pBuffer, chunk, (offset_t)offset,
            UIO_SYSSPACE, FOFFMAX, RLIM64_INFINITY, kcred, &resid);
        if (err != 0 || resid == chunk)
            return NV_ERR_OPERATING_SYSTEM;

        pBuffer += chunk - resid;
        offset += chunk - resid;
        size -= chunk - resid;
    }

    return NV_OK;
}

NV_STATUS NV_API_CALL os_write_file(void *pFile, NvU8 *pBuffer, NvU64 size,
    NvU64 offset)
{
    return os_file_rdwr(UIO_WRITE, pFile, pBuffer, size, offset);
}

NV_STATUS NV_API_CALL os_read_file(void *pFile, NvU8 *pBuffer, NvU64 size,
    NvU64 offset)
{
    return os_file_rdwr(UIO_READ, pFile, pBuffer, size, offset);
}

static int
os_nvswitch_walk(dev_info_t *dip, void *arg)
{
    NvBool *found = arg;
    int vendor, class;

    vendor = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
        "vendor-id", -1);
    class = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
        "class-code", -1);

    if (vendor == 0x10de && class != -1 &&
        ((class >> 8) & 0xffff) == ((PCI_CLASS_BRIDGE << 8) | PCI_BRIDGE_OTHER))
    {
        *found = NV_TRUE;
        return DDI_WALK_TERMINATE;
    }

    return DDI_WALK_CONTINUE;
}

NvBool NV_API_CALL os_is_nvswitch_present(void)
{
    NvBool found = NV_FALSE;

    ndi_devi_enter(ddi_root_node());
    ddi_walk_devs(ddi_get_child(ddi_root_node()), os_nvswitch_walk, &found);
    ndi_devi_exit(ddi_root_node());

    return found;
}
