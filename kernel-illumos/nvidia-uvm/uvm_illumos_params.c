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
 * UVM module parameters from nvidia_uvm driver properties, which are read
 * once, just before UVM starts.  An integer parameter takes an integer
 * property or a string that Linux would accept for it, so unsigned values
 * above INT32_MAX are written as strings.  A value that does not parse or
 * is out of range is reported and the parameter keeps its default.
 */

#include "uvm_illumos.h"

#include <sys/ctype.h>

#define UVM_PARAM_EXTERN(name, type, lo, hi)                                \
    extern const struct linux_param_var nv_uvm_param_##name;
UVM_PARAMS(UVM_PARAM_EXTERN)

typedef struct uvm_param {
    const char                     *up_name;
    linux_param_type_t              up_type;
    int64_t                         up_min;
    uint64_t                        up_max;
    const struct linux_param_var   *up_var;
    uint64_t                        up_default;
    char                           *up_default_str;
    char                           *up_alloc;       /* charp value we own */
    size_t                          up_alloc_size;
} uvm_param_t;

#define UVM_PARAM_ENTRY(name, type, lo, hi)                                 \
    { #name, LINUX_PARAM_TYPE_##type, (lo), (hi), &nv_uvm_param_##name },

static uvm_param_t uvm_params[] = {
    UVM_PARAMS(UVM_PARAM_ENTRY)
};

static uint64_t
uvm_param_get(const uvm_param_t *p)
{
    void *addr = p->up_var->lpv_addr;

    switch (p->up_type) {
    case LINUX_PARAM_TYPE_int:
        return ((uint64_t)(int64_t)*(int32_t *)addr);
    case LINUX_PARAM_TYPE_uint:
        return (*(uint32_t *)addr);
    case LINUX_PARAM_TYPE_ulong:
        return (*(uint64_t *)addr);
    case LINUX_PARAM_TYPE_bool:
        return (*(bool *)addr);
    default:
        return (0);
    }
}

static void
uvm_param_set(uvm_param_t *p, uint64_t v)
{
    void *addr = p->up_var->lpv_addr;

    switch (p->up_type) {
    case LINUX_PARAM_TYPE_int:
        *(int32_t *)addr = (int32_t)(int64_t)v;
        break;
    case LINUX_PARAM_TYPE_uint:
        *(uint32_t *)addr = (uint32_t)v;
        break;
    case LINUX_PARAM_TYPE_ulong:
        *(uint64_t *)addr = v;
        break;
    case LINUX_PARAM_TYPE_bool:
        *(bool *)addr = (v != 0);
        break;
    default:
        break;
    }
}

static boolean_t
uvm_param_in_range(const uvm_param_t *p, uint64_t v)
{
    if (p->up_type == LINUX_PARAM_TYPE_int)
        return ((int64_t)v >= p->up_min && (int64_t)v <= (int64_t)p->up_max);

    return (v >= (uint64_t)p->up_min && v <= p->up_max);
}

static boolean_t
uvm_param_word(const char *s, const char *const *words)
{
    for (; *words != NULL; words++) {
        if (strcasecmp(s, *words) == 0)
            return (B_TRUE);
    }

    return (B_FALSE);
}

/* The spellings of Linux kstrtobool(). */
static boolean_t
uvm_param_parse_bool(const char *s, uint64_t *vp)
{
    static const char *const yes[] = { "1", "y", "yes", "t", "true", "on",
        NULL };
    static const char *const no[] = { "0", "n", "no", "f", "false", "off",
        NULL };

    if (uvm_param_word(s, yes))
        *vp = 1;
    else if (uvm_param_word(s, no))
        *vp = 0;
    else
        return (B_FALSE);

    return (B_TRUE);
}

/* Like Linux kstrtoint() and friends: base prefix allowed, nothing after. */
static boolean_t
uvm_param_parse_num(const uvm_param_t *p, const char *s, uint64_t *vp)
{
    char *end = NULL;

    if (*s == '\0' || isspace(*s))
        return (B_FALSE);

    if (p->up_type == LINUX_PARAM_TYPE_int) {
        longlong_t v;

        if (ddi_strtoll(s, &end, 0, &v) != 0)
            return (B_FALSE);
        *vp = (uint64_t)(int64_t)v;
    } else {
        u_longlong_t v;

        if (*s == '-' || ddi_strtoull(s, &end, 0, &v) != 0)
            return (B_FALSE);
        *vp = v;
    }

    return (end != NULL && end != s && *end == '\0');
}

static void
uvm_param_reject(const uvm_param_t *p, const char *why)
{
    cmn_err(CE_WARN, "%s: ignoring property %s: %s", UVM_ILLUMOS_NAME,
        p->up_name, why);
}

static void
uvm_param_free_str(uvm_param_t *p)
{
    if (p->up_alloc != NULL) {
        kmem_free(p->up_alloc, p->up_alloc_size);
        p->up_alloc = NULL;
        p->up_alloc_size = 0;
    }
}

static void
uvm_param_load_str(uvm_param_t *p, const char *s)
{
    size_t len = strnlen(s, (size_t)p->up_max + 1);

    if (len > p->up_max) {
        uvm_param_reject(p, "string too long");
        return;
    }

    p->up_alloc_size = len + 1;
    p->up_alloc = kmem_alloc(p->up_alloc_size, KM_SLEEP);
    bcopy(s, p->up_alloc, len + 1);
    *(char **)p->up_var->lpv_addr = p->up_alloc;

    cmn_err(CE_CONT, "!%s: %s=\"%s\"\n", UVM_ILLUMOS_NAME, p->up_name,
        p->up_alloc);
}

static void
uvm_param_load(uvm_param_t *p, dev_info_t *dip)
{
    const uint_t flags = DDI_PROP_DONTPASS | DDI_PROP_NOTPROM;
    int *ints;
    uint_t nints;
    char *str;
    uint64_t v;
    boolean_t ok;

    if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, flags,
        (char *)p->up_name, &ints, &nints) == DDI_PROP_SUCCESS) {
        if (p->up_type == LINUX_PARAM_TYPE_charp) {
            uvm_param_reject(p, "a string is required");
        } else if (nints != 1) {
            uvm_param_reject(p, "a single value is required");
        } else if (p->up_type != LINUX_PARAM_TYPE_int && ints[0] < 0) {
            uvm_param_reject(p, "value out of range");
        } else {
            v = (uint64_t)(int64_t)ints[0];
            if (!uvm_param_in_range(p, v)) {
                uvm_param_reject(p, "value out of range");
            } else {
                uvm_param_set(p, v);
                cmn_err(CE_CONT, "!%s: %s=%d\n", UVM_ILLUMOS_NAME,
                    p->up_name, ints[0]);
            }
        }
        ddi_prop_free(ints);
        return;
    }

    if (ddi_prop_lookup_string(DDI_DEV_T_ANY, dip, flags,
        (char *)p->up_name, &str) != DDI_PROP_SUCCESS) {
        if (ddi_prop_exists(DDI_DEV_T_ANY, dip, flags, (char *)p->up_name))
            uvm_param_reject(p, "not an integer or a string");
        return;
    }

    if (p->up_type == LINUX_PARAM_TYPE_charp) {
        uvm_param_load_str(p, str);
    } else {
        if (p->up_type == LINUX_PARAM_TYPE_bool)
            ok = uvm_param_parse_bool(str, &v);
        else
            ok = uvm_param_parse_num(p, str, &v);

        if (!ok) {
            uvm_param_reject(p, "malformed value");
        } else if (!uvm_param_in_range(p, v)) {
            uvm_param_reject(p, "value out of range");
        } else {
            uvm_param_set(p, v);
            cmn_err(CE_CONT, "!%s: %s=%s\n", UVM_ILLUMOS_NAME, p->up_name,
                str);
        }
    }
    ddi_prop_free(str);
}

static void
uvm_params_restore(void)
{
    uvm_param_t *p;

    for (p = uvm_params; p < uvm_params + ARRAY_SIZE(uvm_params); p++) {
        if (p->up_type == LINUX_PARAM_TYPE_charp) {
            *(char **)p->up_var->lpv_addr = p->up_default_str;
            uvm_param_free_str(p);
        } else {
            uvm_param_set(p, p->up_default);
        }
    }
}

/* Records the compiled-in defaults. */
void
uvm_params_init(void)
{
    uvm_param_t *p;

    for (p = uvm_params; p < uvm_params + ARRAY_SIZE(uvm_params); p++) {
        if (p->up_type == LINUX_PARAM_TYPE_charp)
            p->up_default_str = *(char **)p->up_var->lpv_addr;
        else
            p->up_default = uvm_param_get(p);
    }
}

/*
 * Sets every parameter from the properties of dip, starting over from the
 * defaults, since a failed start of UVM may have changed some.  Must not
 * run once UVM has started.
 */
void
uvm_params_apply(dev_info_t *dip)
{
    uvm_param_t *p;

    uvm_params_restore();

    for (p = uvm_params; p < uvm_params + ARRAY_SIZE(uvm_params); p++)
        uvm_param_load(p, dip);
}

void
uvm_params_fini(void)
{
    uvm_params_restore();
}
