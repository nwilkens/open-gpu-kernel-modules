/*
 * SPDX-FileCopyrightText: Copyright (c) 2000-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * Registry keys.  The NVreg_* module parameters of the Linux driver are
 * driver.conf properties of the same name in /kernel/drv/nvidia.conf.
 */

#include "nv-illumos.h"

#define NV_DEFINE_REGISTRY_KEY_TABLE
#include "nv-reg.h"
#include "nv-gpu-info.h"

#include <sys/firmload.h>

/* RM key that forces Confidential Computing on; illumos cannot host CC. */
#define NV_REG_RM_CONFIDENTIAL_COMPUTE  "RmConfidentialCompute"

typedef struct {
    const char  *name;
    char       **value;
} nv_string_parm_t;

static nv_string_parm_t nv_string_parms[] = {
    { "NVreg_" NV_REG_STRING(__NV_COHERENT_GPU_MEMORY_MODE),      &NVreg_CoherentGPUMemoryMode },
    { "NVreg_" NV_REG_STRING(__NV_REGISTRY_DWORDS),               &NVreg_RegistryDwords },
    { "NVreg_" NV_REG_STRING(__NV_REGISTRY_DWORDS_PER_DEVICE),    &NVreg_RegistryDwordsPerDevice },
    { "NVreg_" NV_REG_STRING(__NV_REGISTRY_BINARY_FILE_PER_DEVICE), &NVreg_RegistryBinaryFilePerDevice },
    { "NVreg_" NV_REG_STRING(__NV_RM_MSG),                        &NVreg_RmMsg },
    { "NVreg_" NV_REG_STRING(__NV_GPU_BLACKLIST),                 &NVreg_GpuBlacklist },
    { "NVreg_" NV_REG_STRING(__NV_TEMPORARY_FILE_PATH),           &NVreg_TemporaryFilePath },
    { "NVreg_" NV_REG_STRING(__NV_EXCLUDED_GPUS),                 &NVreg_ExcludedGpus },
    { "NVreg_" NV_REG_STRING(__NV_RM_NVLINK_BW),                  &NVreg_RmNvlinkBandwidth },
    { "NvSwitchRegDwords",                                        &NvSwitchRegDwords },
    { "NvSwitchBlacklist",                                        &NvSwitchBlacklist },
    { NULL, NULL }
};

static char *
nv_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *d = kmem_alloc(len, KM_SLEEP);

    bcopy(s, d, len);
    return d;
}

static void
nv_strfree(char *s)
{
    if (s != NULL)
        kmem_free(s, strlen(s) + 1);
}

/*
 * Load registry parameters from the driver.conf properties visible on dip.
 * Global driver.conf properties are attached to every node of the driver, so
 * any instance works.
 */
void
nv_registry_load(dev_info_t *dip)
{
    nv_parm_t *entry;
    nv_string_parm_t *sentry;
    char name[64];

    for (entry = nv_parms; entry->name != NULL; entry++)
    {
        (void) snprintf(name, sizeof (name), "NVreg_%s", entry->name);
        *entry->data = (NvU32)ddi_prop_get_int(DDI_DEV_T_ANY, dip,
            DDI_PROP_DONTPASS, name, (int)*entry->data);
    }

    for (sentry = nv_string_parms; sentry->name != NULL; sentry++)
    {
        char *value;

        if (ddi_prop_lookup_string(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
                (char *)sentry->name, &value) != DDI_PROP_SUCCESS)
            continue;

        *sentry->value = nv_strdup(value);
        ddi_prop_free(value);
    }
}

void
nv_registry_unload(void)
{
    nv_string_parm_t *sentry;

    for (sentry = nv_string_parms; sentry->name != NULL; sentry++)
    {
        nv_strfree(*sentry->value);
        *sentry->value = NULL;
    }
}

static NvBool
nv_registry_key_is_cc_override(const char *name)
{
    return (strcasecmp(name, NV_REG_RM_CONFIDENTIAL_COMPUTE) == 0);
}

/*
 * Copy a "key=value;key=value" option string without any
 * RmConfidentialCompute entries.  RM would otherwise force Confidential
 * Computing on, which needs CPU-side attestation and SPDM crypto that illumos
 * does not provide.
 */
static char *
nv_registry_filter_cc(const char *option_string)
{
    char *copy, *ptr, *token, *out;
    size_t len;

    len = strlen(option_string) + 1;
    copy = kmem_alloc(len, KM_SLEEP);
    out = kmem_zalloc(len, KM_SLEEP);
    bcopy(option_string, copy, len);

    ptr = copy;
    while ((token = strsep(&ptr, ";")) != NULL)
    {
        char *eq = strchr(token, '=');
        char key[64];
        size_t klen;

        if (*token == '\0')
            continue;

        klen = (eq != NULL) ? (size_t)(eq - token) : strlen(token);
        while (klen > 0 && token[klen - 1] == ' ')
            klen--;
        while (klen > 0 && *token == ' ')
        {
            token++;
            klen--;
        }

        if (klen < sizeof (key))
        {
            bcopy(token, key, klen);
            key[klen] = '\0';
            if (nv_registry_key_is_cc_override(key))
            {
                nv_printf(NV_DBG_ERRORS,
                    "NVRM: ignoring %s: Confidential Computing is not "
                    "supported on illumos\n", NV_REG_RM_CONFIDENTIAL_COMPUTE);
                continue;
            }
        }

        if (out[0] != '\0')
            (void) strlcat(out, ";", len);
        (void) strlcat(out, token, len);
    }

    kmem_free(copy, len);
    return out;
}

/*
 * Parses a PCI BDF of the form "bus:slot", "domain:bus:slot" or
 * "domain:bus:slot.func".
 */
static NV_STATUS
pci_str_to_bdf(char *pci_dev_str, NvU32 *pci_domain, NvU32 *pci_bus,
    NvU32 *pci_slot, NvU32 *pci_func)
{
    char *option_string;
    char *token, *string;
    NvU32 domain, bus, slot;
    NV_STATUS status = NV_OK;

    if ((option_string = rm_remove_spaces(pci_dev_str)) == NULL)
        return NV_ERR_GENERIC;

    string = option_string;

    if (strlen(string) == 0)
    {
        status = NV_ERR_INVALID_ARGUMENT;
        goto done;
    }

    if ((token = strsep(&string, ".")) == NULL)
    {
        status = NV_ERR_INVALID_ARGUMENT;
        goto done;
    }

    if (string != NULL &&
        (!(*string >= '0' && *string <= '7') || strlen(string) > 1))
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: Invalid PCI function in token %s\n",
                  pci_dev_str);
        status = NV_ERR_INVALID_ARGUMENT;
        goto done;
    }
    *pci_func = (string == NULL) ? 0 : (NvU32)(*string - '0');

    domain = os_strtoul(token, &string, 16);
    if (string == NULL || *string != ':' || *(string + 1) == '\0')
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: Invalid PCI domain/bus in token %s\n",
                  pci_dev_str);
        status = NV_ERR_INVALID_ARGUMENT;
        goto done;
    }

    token = string;
    bus = os_strtoul(token + 1, &string, 16);

    if (*string != '\0')
    {
        if (*string != ':' || *(string + 1) == '\0')
        {
            nv_printf(NV_DBG_ERRORS, "NVRM: Invalid PCI slot in token %s\n",
                      pci_dev_str);
            status = NV_ERR_INVALID_ARGUMENT;
            goto done;
        }

        token = string;
        slot = os_strtoul(token + 1, &string, 16);
        if (slot == 0 && (token + 1) == string)
        {
            nv_printf(NV_DBG_ERRORS, "NVRM: Invalid PCI slot in token %s\n",
                      pci_dev_str);
            status = NV_ERR_INVALID_ARGUMENT;
            goto done;
        }
        *pci_domain = domain;
        *pci_bus = bus;
        *pci_slot = slot;
    }
    else
    {
        *pci_slot = bus;
        *pci_bus = domain;
        *pci_domain = 0;
    }

done:
    os_free_mem(option_string);
    return status;
}

static NvBool
nv_bdf_matches(nv_state_t *nv, NvU32 domain, NvU32 bus, NvU32 slot, NvU32 func)
{
    return (nv->pci_info.domain == domain && nv->pci_info.bus == bus &&
            nv->pci_info.slot == slot && nv->pci_info.function == func);
}

/*
 * Parses NVreg_RegistryDwordsPerDevice: "pci=DDDD:BB:DD.F;key=value;..."
 * applies the keys that follow a "pci=" entry matching this GPU.
 */
NV_STATUS
nv_parse_per_device_option_string(nvidia_stack_t *sp, nv_state_t *current_nv)
{
    char *option_string, *ptr, *token, *name, *value;
    NvU32 data, domain, bus, slot, func;
    nv_state_t *nv = NULL;

    if (current_nv == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (NVreg_RegistryDwordsPerDevice == NULL)
        return NV_OK;

    if ((option_string = rm_remove_spaces(NVreg_RegistryDwordsPerDevice)) == NULL)
        return NV_ERR_GENERIC;

    ptr = option_string;

    while ((token = strsep(&ptr, ";")) != NULL)
    {
        if ((name = strsep(&token, "=")) == NULL || strlen(name) == 0)
            continue;

        if ((value = strsep(&token, "=")) == NULL || strlen(value) == 0)
            continue;

        if (strsep(&token, "=") != NULL)
            continue;

        if (strcmp(name, NV_REG_PCI_DEVICE_BDF) == 0)
        {
            if (pci_str_to_bdf(value, &domain, &bus, &slot, &func) == NV_OK &&
                nv_bdf_matches(current_nv, domain, bus, slot, func))
                nv = current_nv;
            else
                nv = NULL;
            continue;
        }

        if (nv == NULL)
            continue;

        if (nv_registry_key_is_cc_override(name))
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: ignoring %s: Confidential Computing is not "
                "supported on illumos\n", NV_REG_RM_CONFIDENTIAL_COMPUTE);
            continue;
        }

        data = os_strtoul(value, NULL, 0);
        rm_write_registry_dword(sp, nv, name, data);
    }

    os_free_mem(option_string);
    return NV_OK;
}

/*
 * Loads a binary registry value from /kernel/firmware/nvidia/<path>.
 */
static NV_STATUS
nv_write_binary_registry_file(nvidia_stack_t *sp, nv_state_t *nv,
    const char *key_name, const char *relative_path)
{
    firmware_handle_t fh;
    off_t size;
    void *buf;
    NV_STATUS status;
    int err;

    if (strstr(relative_path, "..") != NULL || relative_path[0] == '/')
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: NVreg_RegistryBinaryFilePerDevice key \"%s\": invalid path \"%s\"\n",
            key_name, relative_path);
        return NV_ERR_INVALID_ARGUMENT;
    }

    err = firmware_open(NV_ILLUMOS_DRIVER_NAME, relative_path, &fh);
    if (err != 0)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: NVreg_RegistryBinaryFilePerDevice key \"%s\" failed to load "
            "firmware file \"%s\" (rc=%d)\n", key_name, relative_path, err);
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    size = firmware_get_size(fh);
    if (size <= 0 || size > NV_U32_MAX)
    {
        (void) firmware_close(fh);
        return NV_ERR_INVALID_DATA;
    }

    buf = kmem_alloc((size_t)size, KM_SLEEP);
    err = firmware_read(fh, 0, buf, (size_t)size);
    (void) firmware_close(fh);

    if (err != 0)
    {
        kmem_free(buf, (size_t)size);
        return NV_ERR_INVALID_DATA;
    }

    status = rm_write_registry_binary(sp, nv, key_name, buf, (NvU32)size);
    if (status != NV_OK)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: NVreg_RegistryBinaryFilePerDevice key \"%s\" failed to "
            "write registry binary (status=0x%x)\n", key_name, status);
    }

    kmem_free(buf, (size_t)size);
    return status;
}

/*
 * Parses NVreg_RegistryBinaryFilePerDevice: "pci=DDDD:BB:DD.F;key=path;..."
 */
NV_STATUS
nv_parse_per_device_binary_option_string(nvidia_stack_t *sp,
    nv_state_t *current_nv)
{
    NV_STATUS status = NV_OK;
    char *option_string, *ptr, *token, *name, *value;
    NvU32 domain, bus, slot, func;
    nv_state_t *nv = NULL;

    if (current_nv == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (NVreg_RegistryBinaryFilePerDevice == NULL)
        return NV_OK;

    if ((option_string = rm_remove_spaces(NVreg_RegistryBinaryFilePerDevice)) == NULL)
        return NV_ERR_GENERIC;

    ptr = option_string;

    while ((token = strsep(&ptr, ";")) != NULL)
    {
        if (strlen(token) == 0)
            continue;

        if ((name = strsep(&token, "=")) == NULL || strlen(name) == 0 ||
            (value = strsep(&token, "=")) == NULL || strlen(value) == 0 ||
            strsep(&token, "=") != NULL)
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: NVreg_RegistryBinaryFilePerDevice contains a malformed token\n");
            status = NV_ERR_INVALID_ARGUMENT;
            break;
        }

        if (strcmp(name, NV_REG_PCI_DEVICE_BDF) == 0)
        {
            if (pci_str_to_bdf(value, &domain, &bus, &slot, &func) == NV_OK &&
                nv_bdf_matches(current_nv, domain, bus, slot, func))
                nv = current_nv;
            else
                nv = NULL;
            continue;
        }

        if (nv == NULL)
            continue;

        status = nv_write_binary_registry_file(sp, nv, name, value);
        if (status != NV_OK)
            break;
    }

    os_free_mem(option_string);
    return status;
}

/*
 * Compare a GPU UUID with NVreg_ExcludedGpus (or the older NVreg_GpuBlacklist).
 */
NvBool
nv_is_uuid_in_gpu_exclusion_list(const char *uuid)
{
    const char *input;
    char *list, *ptr, *token;

    if (NVreg_ExcludedGpus != NULL)
        input = NVreg_ExcludedGpus;
    else if (NVreg_GpuBlacklist != NULL)
        input = NVreg_GpuBlacklist;
    else
        return NV_FALSE;

    if ((list = rm_remove_spaces(input)) == NULL)
        return NV_FALSE;

    ptr = list;
    while ((token = strsep(&ptr, ",")) != NULL)
    {
        if (strcmp(token, uuid) == 0)
        {
            os_free_mem(list);
            return NV_TRUE;
        }
    }

    os_free_mem(list);
    return NV_FALSE;
}

NV_STATUS NV_API_CALL os_registry_init(void)
{
    nv_parm_t *entry;
    nvidia_stack_t *sp = NULL;

    if (nv_stack_alloc(&sp) != 0)
        return NV_ERR_NO_MEMORY;

    if (NVreg_RmNvlinkBandwidth != NULL)
    {
        rm_write_registry_string(sp, NULL, "RmNvlinkBandwidth",
            NVreg_RmNvlinkBandwidth, strlen(NVreg_RmNvlinkBandwidth));
    }

    if (NVreg_RmMsg != NULL)
    {
        rm_write_registry_string(sp, NULL, "RmMsg", NVreg_RmMsg,
            strlen(NVreg_RmMsg));
    }

    /* CoherentGPUMemoryMode=driver implies EnableUserNUMAManagement=0. */
    if (NVreg_CoherentGPUMemoryMode != NULL)
    {
        NVreg_EnableUserNUMAManagement =
            (strcmp(NVreg_CoherentGPUMemoryMode, "driver") == 0) ? 0 : 1;
    }

    if (NVreg_RegistryDwords != NULL)
    {
        char *filtered = nv_registry_filter_cc(NVreg_RegistryDwords);

        rm_parse_option_string(sp, filtered);
        kmem_free(filtered, strlen(NVreg_RegistryDwords) + 1);
    }

    for (entry = nv_parms; entry->name != NULL; entry++)
        rm_write_registry_dword(sp, NULL, entry->name, *entry->data);

    nv_stack_free(sp);

    return NV_OK;
}

NvU32
nv_reg_device_file_uid(void)
{
    return NVreg_DeviceFileUID;
}

NvU32
nv_reg_device_file_gid(void)
{
    return NVreg_DeviceFileGID;
}

NvU32
nv_reg_enable_msi(void)
{
    return NVreg_EnableMSI;
}
