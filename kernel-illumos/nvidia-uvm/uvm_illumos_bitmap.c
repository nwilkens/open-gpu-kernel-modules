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
 * Out-of-line bitmap helpers with the Linux semantics.
 */

#include "uvm_illumos_kpi.h"

/* Find the first bit at or after offset whose value, xor invert, is 1. */
static unsigned long
find_next_bit_common(const unsigned long *addr, unsigned long size,
    unsigned long offset, unsigned long invert)
{
    unsigned long word;

    if (offset >= size)
        return (size);

    word = (addr[BIT_WORD(offset)] ^ invert) & BITMAP_FIRST_WORD_MASK(offset);
    offset = offset & ~(BITS_PER_LONG - 1);

    while (word == 0) {
        offset += BITS_PER_LONG;
        if (offset >= size)
            return (size);
        word = addr[BIT_WORD(offset)] ^ invert;
    }

    return (MIN(offset + __ffs(word), size));
}

unsigned long
find_next_bit(const unsigned long *addr, unsigned long size,
    unsigned long offset)
{
    return (find_next_bit_common(addr, size, offset, 0UL));
}

unsigned long
find_next_zero_bit(const unsigned long *addr, unsigned long size,
    unsigned long offset)
{
    return (find_next_bit_common(addr, size, offset, ~0UL));
}

unsigned long
find_last_bit(const unsigned long *addr, unsigned long size)
{
    unsigned long idx, word;

    if (size == 0)
        return (size);

    idx = (size - 1) / BITS_PER_LONG;
    word = addr[idx] & BITMAP_LAST_WORD_MASK(size);

    for (;;) {
        if (word != 0)
            return (idx * BITS_PER_LONG + __fls(word));
        if (idx-- == 0)
            return (size);
        word = addr[idx];
    }
}

void
bitmap_set(unsigned long *map, unsigned int start, unsigned int len)
{
    unsigned int i;

    for (i = start; i < start + len; i++)
        __set_bit(i, map);
}

void
bitmap_clear(unsigned long *map, unsigned int start, unsigned int len)
{
    unsigned int i;

    for (i = start; i < start + len; i++)
        __clear_bit(i, map);
}

unsigned int
bitmap_weight(const unsigned long *src, unsigned int nbits)
{
    unsigned int k, w = 0, lim = nbits / BITS_PER_LONG;

    for (k = 0; k < lim; k++)
        w += hweight64(src[k]);
    if (nbits % BITS_PER_LONG)
        w += hweight64(src[k] & BITMAP_LAST_WORD_MASK(nbits));
    return (w);
}

/* dst may alias src in both shifts, as on Linux. */
void
bitmap_shift_right(unsigned long *dst, const unsigned long *src,
    unsigned int shift, unsigned int nbits)
{
    unsigned int k, lim = BITS_TO_LONGS(nbits);
    unsigned int off = shift / BITS_PER_LONG, rem = shift % BITS_PER_LONG;
    unsigned long mask = BITMAP_LAST_WORD_MASK(nbits);

    if (off >= lim) {
        memset(dst, 0, lim * sizeof (unsigned long));
        return;
    }

    for (k = 0; off + k < lim; k++) {
        unsigned long upper, lower;

        if (rem == 0 || off + k + 1 >= lim) {
            upper = 0;
        } else {
            upper = src[off + k + 1];
            if (off + k + 1 == lim - 1)
                upper &= mask;
            upper <<= (BITS_PER_LONG - rem);
        }
        lower = src[off + k];
        if (off + k == lim - 1)
            lower &= mask;
        lower >>= rem;
        dst[k] = lower | upper;
    }
    if (off != 0)
        memset(&dst[lim - off], 0, off * sizeof (unsigned long));
}

void
bitmap_shift_left(unsigned long *dst, const unsigned long *src,
    unsigned int shift, unsigned int nbits)
{
    int k;
    unsigned int lim = BITS_TO_LONGS(nbits);
    unsigned int off = shift / BITS_PER_LONG, rem = shift % BITS_PER_LONG;

    if (off >= lim) {
        memset(dst, 0, lim * sizeof (unsigned long));
        return;
    }

    for (k = (int)lim - (int)off - 1; k >= 0; k--) {
        unsigned long upper, lower;

        if (rem != 0 && k > 0)
            lower = src[k - 1] >> (BITS_PER_LONG - rem);
        else
            lower = 0;
        upper = src[k] << rem;
        dst[k + off] = lower | upper;
    }
    if (off != 0)
        memset(dst, 0, off * sizeof (unsigned long));
    if (nbits % BITS_PER_LONG)
        dst[lim - 1] &= BITMAP_LAST_WORD_MASK(nbits);
}
