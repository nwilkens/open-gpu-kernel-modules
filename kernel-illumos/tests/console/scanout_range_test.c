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

#include <stdint.h>
#include <stdio.h>

#include "../../nvidia/nv-console-range.h"

int
main(void)
{
    static const struct {
        const char *name;
        uint64_t bar, bar_size, scanout, scanout_size, offset;
        int exact, expected;
    } cases[] = {
        { "fb base", 0x1000, 0x1000, 0x1000, 0x1000, 0, 0, 1 },
        { "fb subrange", 0x1000, 0x1000, 0x1800, 0x400, 0, 0, 1 },
        { "fb exact start rejects offset", 0x1000, 0x1000, 0x1800,
            0x400, 0, 1, 0 },
        { "fb too large", 0x1000, 0x1000, 0x1000, 0x1001, 0, 0, 0 },
        { "imem offset", 0x20000000, 0x2000000, 0x21000000,
            0x400000, 0x1000000, 1, 1 },
        { "zero bar", 0x1000, 0, 0x1000, 1, 0, 0, 0 },
        { "zero scanout", 0x1000, 0x1000, 0x1000, 0, 0, 0, 0 },
        { "small imem", 0x20000000, 0xffffff, 0x21000000,
            1, 0x1000000, 1, 0 },
        { "fb end", 0x1000, 0x1000, 0x1fff, 2, 0, 0, 0 },
        { "imem end", 0x20000000, 0x1100000, 0x21000000,
            0x100001, 0x1000000, 1, 0 },
        { "wrong imem start", 0x20000000, 0x2000000, 0x21001000,
            0x1000, 0x1000000, 1, 0 },
        { "bar overflow", UINT64_MAX - 1, 4, UINT64_MAX - 1,
            1, 0, 0, 0 },
        { "scanout overflow", UINT64_MAX - 0x100, 0x101,
            UINT64_MAX - 0x10, 0x20, 0, 0, 0 },
        { "below bar", 0x1000, 0x1000, 0xfff, 1, 0, 0, 0 },
    };
    unsigned int i;

    for (i = 0; i < sizeof (cases) / sizeof (cases[0]); i++) {
        int got = nv_console_range_fits(cases[i].bar, cases[i].bar_size,
            cases[i].scanout, cases[i].scanout_size, cases[i].offset,
            cases[i].exact);
        if (got != cases[i].expected) {
            fprintf(stderr, "%s: got %d, expected %d\n", cases[i].name,
                got, cases[i].expected);
            return (1);
        }
    }
    return (0);
}
