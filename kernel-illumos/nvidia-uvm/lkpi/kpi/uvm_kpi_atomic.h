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
 * Atomics, barriers, bit operations and bitmaps.
 */

#ifndef _UVM_KPI_ATOMIC_H_
#define _UVM_KPI_ATOMIC_H_

/* x86 is TSO: acquire and release need only stop the compiler. */
#define mb()                    membar_enter()
#define rmb()                   membar_consumer()
#define wmb()                   membar_producer()
#define smp_mb()                membar_enter()
#define smp_rmb()               membar_consumer()
#define smp_wmb()               membar_producer()
#define smp_mb__before_atomic() barrier()
#define smp_mb__after_atomic()  barrier()
#define smp_load_acquire(p)     ({ typeof(*(p)) __v = READ_ONCE(*(p)); barrier(); __v; })
#define smp_store_release(p, v) do { barrier(); WRITE_ONCE(*(p), (v)); } while (0)

#define ATOMIC_INIT(i)          { (i) }
#define ATOMIC64_INIT(i)        { (i) }
#define ATOMIC_LONG_INIT(i)     { (i) }

#define atomic_read(v)          READ_ONCE((v)->counter)
#define atomic_set(v, i)        WRITE_ONCE((v)->counter, (i))
#define atomic64_read(v)        READ_ONCE((v)->counter)
#define atomic64_set(v, i)      WRITE_ONCE((v)->counter, (i))
#define atomic_long_read(v)     READ_ONCE((v)->counter)
#define atomic_long_set(v, i)   WRITE_ONCE((v)->counter, (i))

static inline int atomic_add_return(int i, atomic_t *v)
{
    return (int)atomic_add_32_nv((volatile uint32_t *)&v->counter, i);
}

#define atomic_sub_return(i, v) atomic_add_return(-(i), v)
#define atomic_inc_return(v)    atomic_add_return(1, v)
#define atomic_dec_return(v)    atomic_add_return(-1, v)
#define atomic_add(i, v)        ((void)atomic_add_return(i, v))
#define atomic_sub(i, v)        ((void)atomic_add_return(-(i), v))
#define atomic_inc(v)           ((void)atomic_add_return(1, v))
#define atomic_dec(v)           ((void)atomic_add_return(-1, v))
#define atomic_dec_and_test(v)  (atomic_add_return(-1, v) == 0)
#define atomic_inc_and_test(v)  (atomic_add_return(1, v) == 0)

static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
    return (int)atomic_cas_32((volatile uint32_t *)&v->counter,
        (uint32_t)old, (uint32_t)new);
}

static inline int atomic_xchg(atomic_t *v, int new)
{
    return (int)atomic_swap_32((volatile uint32_t *)&v->counter,
        (uint32_t)new);
}

static inline int atomic_dec_if_positive(atomic_t *v)
{
    int c, old;

    for (c = atomic_read(v); ; c = old) {
        if (c <= 0)
            return c - 1;
        old = atomic_cmpxchg(v, c, c - 1);
        if (old == c)
            return c - 1;
    }
}

static inline s64 atomic64_add_return(s64 i, atomic64_t *v)
{
    return (s64)atomic_add_64_nv((volatile uint64_t *)&v->counter, i);
}

#define atomic64_sub_return(i, v)   atomic64_add_return(-(i), v)
#define atomic64_inc_return(v)      atomic64_add_return(1, v)
#define atomic64_dec_return(v)      atomic64_add_return(-1, v)
#define atomic64_add(i, v)          ((void)atomic64_add_return(i, v))
#define atomic64_sub(i, v)          ((void)atomic64_add_return(-(i), v))
#define atomic64_inc(v)             ((void)atomic64_add_return(1, v))
#define atomic64_dec(v)             ((void)atomic64_add_return(-1, v))
#define atomic64_dec_and_test(v)    (atomic64_add_return(-1, v) == 0)

static inline s64 atomic64_cmpxchg(atomic64_t *v, s64 old, s64 new)
{
    return (s64)atomic_cas_64((volatile uint64_t *)&v->counter,
        (uint64_t)old, (uint64_t)new);
}

static inline s64 atomic64_xchg(atomic64_t *v, s64 new)
{
    return (s64)atomic_swap_64((volatile uint64_t *)&v->counter,
        (uint64_t)new);
}

static inline long atomic_long_add_return(long i, atomic_long_t *v)
{
    return (long)atomic_add_long_nv((volatile ulong_t *)&v->counter, i);
}

#define atomic_long_add(i, v)       ((void)atomic_long_add_return(i, v))
#define atomic_long_sub(i, v)       ((void)atomic_long_add_return(-(i), v))
#define atomic_long_inc(v)          ((void)atomic_long_add_return(1, v))
#define atomic_long_dec(v)          ((void)atomic_long_add_return(-1, v))
#define atomic_long_inc_return(v)   atomic_long_add_return(1, v)
#define atomic_long_dec_return(v)   atomic_long_add_return(-1, v)

static inline long atomic_long_cmpxchg(atomic_long_t *v, long old, long new)
{
    return (long)atomic_cas_ulong((volatile ulong_t *)&v->counter,
        (ulong_t)old, (ulong_t)new);
}

static inline long atomic_long_xchg(atomic_long_t *v, long new)
{
    return (long)atomic_swap_ulong((volatile ulong_t *)&v->counter,
        (ulong_t)new);
}

#define atomic_long_read_acquire(v)     smp_load_acquire(&(v)->counter)
#define atomic_long_set_release(v, i)   smp_store_release(&(v)->counter, (i))

#define cmpxchg(p, o, n)                                                    \
    ({                                                                      \
        BUILD_BUG_ON(sizeof(*(p)) != 4 && sizeof(*(p)) != 8);               \
        (typeof(*(p)))((sizeof(*(p)) == 8) ?                                \
            atomic_cas_64((volatile uint64_t *)(p), (uint64_t)(o),          \
                (uint64_t)(n)) :                                            \
            atomic_cas_32((volatile uint32_t *)(p), (uint32_t)(o),          \
                (uint32_t)(n)));                                            \
    })

/* Bit operations on arrays of unsigned long. */
#define BIT_WORD(nr)            ((nr) / BITS_PER_LONG)
#define BIT_MASK(nr)            (1UL << ((nr) % BITS_PER_LONG))

static inline void set_bit(long nr, volatile unsigned long *addr)
{
    atomic_or_ulong((volatile ulong_t *)&addr[BIT_WORD(nr)], BIT_MASK(nr));
}

static inline void clear_bit(long nr, volatile unsigned long *addr)
{
    atomic_and_ulong((volatile ulong_t *)&addr[BIT_WORD(nr)], ~BIT_MASK(nr));
}

static inline void change_bit(long nr, volatile unsigned long *addr)
{
    unsigned long old;
    volatile unsigned long *p = &addr[BIT_WORD(nr)];

    do {
        old = *p;
    } while (atomic_cas_ulong((volatile ulong_t *)p, old,
        old ^ BIT_MASK(nr)) != old);
}

static inline int test_and_set_bit(long nr, volatile unsigned long *addr)
{
    return atomic_set_long_excl((volatile ulong_t *)&addr[BIT_WORD(nr)],
        (uint_t)(nr % BITS_PER_LONG)) != 0;
}

static inline int test_and_clear_bit(long nr, volatile unsigned long *addr)
{
    return atomic_clear_long_excl((volatile ulong_t *)&addr[BIT_WORD(nr)],
        (uint_t)(nr % BITS_PER_LONG)) == 0;
}

static inline void __set_bit(long nr, volatile unsigned long *addr)
{
    addr[BIT_WORD(nr)] |= BIT_MASK(nr);
}

static inline void __clear_bit(long nr, volatile unsigned long *addr)
{
    addr[BIT_WORD(nr)] &= ~BIT_MASK(nr);
}

static inline int __test_and_set_bit(long nr, volatile unsigned long *addr)
{
    unsigned long old = addr[BIT_WORD(nr)];

    addr[BIT_WORD(nr)] = old | BIT_MASK(nr);
    return (old & BIT_MASK(nr)) != 0;
}

static inline int __test_and_clear_bit(long nr, volatile unsigned long *addr)
{
    unsigned long old = addr[BIT_WORD(nr)];

    addr[BIT_WORD(nr)] = old & ~BIT_MASK(nr);
    return (old & BIT_MASK(nr)) != 0;
}

static inline int test_bit(long nr, const volatile unsigned long *addr)
{
    return (addr[BIT_WORD(nr)] & BIT_MASK(nr)) != 0;
}

#define clear_bit_unlock(nr, addr)                                          \
    do { smp_mb__before_atomic(); clear_bit(nr, addr); } while (0)
#define test_and_set_bit_lock(nr, addr)     test_and_set_bit(nr, addr)

static inline unsigned long __ffs(unsigned long word)
{
    return (unsigned long)__builtin_ctzl(word);
}

static inline unsigned long __fls(unsigned long word)
{
    return (unsigned long)(BITS_PER_LONG - 1 - __builtin_clzl(word));
}

static inline int fls(unsigned int x)
{
    return (x == 0) ? 0 : 32 - __builtin_clz(x);
}

static inline int fls64(u64 x)
{
    return (x == 0) ? 0 : 64 - __builtin_clzll(x);
}

static inline unsigned long ffz(unsigned long word)
{
    return __ffs(~word);
}

/* Written out because -mno-popcnt makes GCC call __popcountdi2. */
static inline unsigned int hweight64(u64 w)
{
    w = w - ((w >> 1) & 0x5555555555555555ULL);
    w = (w & 0x3333333333333333ULL) + ((w >> 2) & 0x3333333333333333ULL);
    w = (w + (w >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (unsigned int)((w * 0x0101010101010101ULL) >> 56);
}

#define hweight32(w)            hweight64((u32)(w))
#define hweight16(w)            hweight64((u16)(w))
#define hweight8(w)             hweight64((u8)(w))
#define hweight_long(w)         ((unsigned long)hweight64((unsigned long)(w)))

static inline int ilog2_u64(u64 n)
{
    return 63 - __builtin_clzll(n);
}

/* Constant expressions when n is constant, for bit-field widths. */
#define __const_ilog2(n) ( \
    ((n) & (1ULL << 63)) ? 63 : \
    ((n) & (1ULL << 62)) ? 62 : \
    ((n) & (1ULL << 61)) ? 61 : \
    ((n) & (1ULL << 60)) ? 60 : \
    ((n) & (1ULL << 59)) ? 59 : \
    ((n) & (1ULL << 58)) ? 58 : \
    ((n) & (1ULL << 57)) ? 57 : \
    ((n) & (1ULL << 56)) ? 56 : \
    ((n) & (1ULL << 55)) ? 55 : \
    ((n) & (1ULL << 54)) ? 54 : \
    ((n) & (1ULL << 53)) ? 53 : \
    ((n) & (1ULL << 52)) ? 52 : \
    ((n) & (1ULL << 51)) ? 51 : \
    ((n) & (1ULL << 50)) ? 50 : \
    ((n) & (1ULL << 49)) ? 49 : \
    ((n) & (1ULL << 48)) ? 48 : \
    ((n) & (1ULL << 47)) ? 47 : \
    ((n) & (1ULL << 46)) ? 46 : \
    ((n) & (1ULL << 45)) ? 45 : \
    ((n) & (1ULL << 44)) ? 44 : \
    ((n) & (1ULL << 43)) ? 43 : \
    ((n) & (1ULL << 42)) ? 42 : \
    ((n) & (1ULL << 41)) ? 41 : \
    ((n) & (1ULL << 40)) ? 40 : \
    ((n) & (1ULL << 39)) ? 39 : \
    ((n) & (1ULL << 38)) ? 38 : \
    ((n) & (1ULL << 37)) ? 37 : \
    ((n) & (1ULL << 36)) ? 36 : \
    ((n) & (1ULL << 35)) ? 35 : \
    ((n) & (1ULL << 34)) ? 34 : \
    ((n) & (1ULL << 33)) ? 33 : \
    ((n) & (1ULL << 32)) ? 32 : \
    ((n) & (1ULL << 31)) ? 31 : \
    ((n) & (1ULL << 30)) ? 30 : \
    ((n) & (1ULL << 29)) ? 29 : \
    ((n) & (1ULL << 28)) ? 28 : \
    ((n) & (1ULL << 27)) ? 27 : \
    ((n) & (1ULL << 26)) ? 26 : \
    ((n) & (1ULL << 25)) ? 25 : \
    ((n) & (1ULL << 24)) ? 24 : \
    ((n) & (1ULL << 23)) ? 23 : \
    ((n) & (1ULL << 22)) ? 22 : \
    ((n) & (1ULL << 21)) ? 21 : \
    ((n) & (1ULL << 20)) ? 20 : \
    ((n) & (1ULL << 19)) ? 19 : \
    ((n) & (1ULL << 18)) ? 18 : \
    ((n) & (1ULL << 17)) ? 17 : \
    ((n) & (1ULL << 16)) ? 16 : \
    ((n) & (1ULL << 15)) ? 15 : \
    ((n) & (1ULL << 14)) ? 14 : \
    ((n) & (1ULL << 13)) ? 13 : \
    ((n) & (1ULL << 12)) ? 12 : \
    ((n) & (1ULL << 11)) ? 11 : \
    ((n) & (1ULL << 10)) ? 10 : \
    ((n) & (1ULL << 9)) ? 9 : \
    ((n) & (1ULL << 8)) ? 8 : \
    ((n) & (1ULL << 7)) ? 7 : \
    ((n) & (1ULL << 6)) ? 6 : \
    ((n) & (1ULL << 5)) ? 5 : \
    ((n) & (1ULL << 4)) ? 4 : \
    ((n) & (1ULL << 3)) ? 3 : \
    ((n) & (1ULL << 2)) ? 2 : \
    ((n) & (1ULL << 1)) ? 1 : \
    0)

#define ilog2(n)                                                            \
    (__builtin_constant_p(n) ? __const_ilog2((u64)(n)) : ilog2_u64((u64)(n)))
#define is_power_of_2(n)        ((n) != 0 && (((n) & ((n) - 1)) == 0))
#define roundup_pow_of_two(n)   ((n) <= 1 ? 1 : (1ULL << (ilog2((n) - 1) + 1)))
#define rounddown_pow_of_two(n) (1ULL << ilog2(n))
#define order_base_2(n)         ((n) > 1 ? ilog2((n) - 1) + 1 : 0)

/* Bitmaps. */
#define DECLARE_BITMAP(name, bits)  unsigned long name[BITS_TO_LONGS(bits)]
#define BITMAP_FIRST_WORD_MASK(start)   (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits)    (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

unsigned long find_next_bit(const unsigned long *, unsigned long, unsigned long);
unsigned long find_next_zero_bit(const unsigned long *, unsigned long,
    unsigned long);
unsigned long find_last_bit(const unsigned long *, unsigned long);
void    bitmap_set(unsigned long *, unsigned int, unsigned int);
void    bitmap_clear(unsigned long *, unsigned int, unsigned int);
void    bitmap_shift_right(unsigned long *, const unsigned long *,
            unsigned int, unsigned int);
void    bitmap_shift_left(unsigned long *, const unsigned long *,
            unsigned int, unsigned int);
unsigned int bitmap_weight(const unsigned long *, unsigned int);

#define find_first_bit(addr, size)      find_next_bit(addr, size, 0)
#define find_first_zero_bit(addr, size) find_next_zero_bit(addr, size, 0)

#define for_each_set_bit(bit, addr, size)                                   \
    for ((bit) = find_first_bit((addr), (size));                            \
         (bit) < (size);                                                    \
         (bit) = find_next_bit((addr), (size), (bit) + 1))

#define for_each_set_bit_from(bit, addr, size)                              \
    for ((bit) = find_next_bit((addr), (size), (bit));                      \
         (bit) < (size);                                                    \
         (bit) = find_next_bit((addr), (size), (bit) + 1))

#define for_each_clear_bit(bit, addr, size)                                 \
    for ((bit) = find_first_zero_bit((addr), (size));                       \
         (bit) < (size);                                                    \
         (bit) = find_next_zero_bit((addr), (size), (bit) + 1))

#define for_each_clear_bit_from(bit, addr, size)                            \
    for ((bit) = find_next_zero_bit((addr), (size), (bit));                 \
         (bit) < (size);                                                    \
         (bit) = find_next_zero_bit((addr), (size), (bit) + 1))

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
    memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}

static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
    unsigned int n = BITS_TO_LONGS(nbits);

    memset(dst, 0xff, n * sizeof(unsigned long));
    if (nbits % BITS_PER_LONG)
        dst[n - 1] &= BITMAP_LAST_WORD_MASK(nbits);
}

static inline void bitmap_copy(unsigned long *dst, const unsigned long *src,
    unsigned int nbits)
{
    memcpy(dst, src, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}

#define __BITMAP_OP2(name, expr)                                            \
static inline int name(unsigned long *dst, const unsigned long *a,          \
    const unsigned long *b, unsigned int nbits)                             \
{                                                                           \
    unsigned int k, n = BITS_TO_LONGS(nbits);                               \
    unsigned long res = 0;                                                  \
                                                                            \
    for (k = 0; k < n; k++) {                                               \
        dst[k] = (expr);                                                    \
        if (k == n - 1 && (nbits % BITS_PER_LONG))                          \
            dst[k] &= BITMAP_LAST_WORD_MASK(nbits);                         \
        res |= dst[k];                                                      \
    }                                                                       \
    return res != 0;                                                        \
}

__BITMAP_OP2(bitmap_and, a[k] & b[k])
__BITMAP_OP2(bitmap_andnot, a[k] & ~b[k])
__BITMAP_OP2(__bitmap_or, a[k] | b[k])
__BITMAP_OP2(__bitmap_xor, a[k] ^ b[k])

#define bitmap_or(d, a, b, n)       ((void)__bitmap_or(d, a, b, n))
#define bitmap_xor(d, a, b, n)      ((void)__bitmap_xor(d, a, b, n))

static inline void bitmap_complement(unsigned long *dst,
    const unsigned long *src, unsigned int nbits)
{
    unsigned int k, n = BITS_TO_LONGS(nbits);

    for (k = 0; k < n; k++)
        dst[k] = ~src[k];
    if (nbits % BITS_PER_LONG)
        dst[n - 1] &= BITMAP_LAST_WORD_MASK(nbits);
}

static inline bool bitmap_empty(const unsigned long *src, unsigned int nbits)
{
    return find_first_bit(src, nbits) >= nbits;
}

static inline bool bitmap_full(const unsigned long *src, unsigned int nbits)
{
    return find_first_zero_bit(src, nbits) >= nbits;
}

static inline bool bitmap_equal(const unsigned long *a, const unsigned long *b,
    unsigned int nbits)
{
    unsigned int k, lim = nbits / BITS_PER_LONG;

    for (k = 0; k < lim; k++)
        if (a[k] != b[k])
            return false;
    if (nbits % BITS_PER_LONG)
        return ((a[k] ^ b[k]) & BITMAP_LAST_WORD_MASK(nbits)) == 0;
    return true;
}

static inline bool bitmap_intersects(const unsigned long *a,
    const unsigned long *b, unsigned int nbits)
{
    unsigned int k, lim = nbits / BITS_PER_LONG;

    for (k = 0; k < lim; k++)
        if (a[k] & b[k])
            return true;
    if (nbits % BITS_PER_LONG)
        return ((a[k] & b[k]) & BITMAP_LAST_WORD_MASK(nbits)) != 0;
    return false;
}

static inline bool bitmap_subset(const unsigned long *a,
    const unsigned long *b, unsigned int nbits)
{
    unsigned int k, lim = nbits / BITS_PER_LONG;

    for (k = 0; k < lim; k++)
        if (a[k] & ~b[k])
            return false;
    if (nbits % BITS_PER_LONG)
        return ((a[k] & ~b[k]) & BITMAP_LAST_WORD_MASK(nbits)) == 0;
    return true;
}

#endif /* _UVM_KPI_ATOMIC_H_ */
