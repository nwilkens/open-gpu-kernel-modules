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
 * Compiler helpers, arithmetic macros, printing, errno and time.
 */

#ifndef _UVM_KPI_BASE_H_
#define _UVM_KPI_BASE_H_

#define __init
#define __exit
#define __user
#define __iomem
#define __read_mostly
#define __force
#define __must_check            __attribute__((warn_unused_result))
#define __always_inline         inline __attribute__((always_inline))
#define noinline                __attribute__((noinline))
#define __cold                  __attribute__((cold))
#define ____cacheline_aligned_in_smp __attribute__((aligned(64)))
#define __same_type(a, b)       __builtin_types_compatible_p(typeof(a), typeof(b))

#ifndef likely
#define likely(x)               __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x)             __builtin_expect(!!(x), 0)
#endif

#define barrier()               __asm__ __volatile__("" ::: "memory")
#define READ_ONCE(x)            (*(const volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, v)        (*(volatile typeof(x) *)&(x) = (v))

#ifndef container_of
#define container_of(ptr, type, member)                                     \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#endif

#define BUILD_BUG_ON(cond)      _Static_assert(!(cond), "BUILD_BUG_ON(" #cond ")")
#define BUILD_BUG_ON_NOT_POWER_OF_2(n)                                      \
    BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))

#define BITS_PER_LONG           64
#define BITS_PER_BYTE           8
#define BIT(n)                  (1UL << (n))
#define BIT_ULL(n)              (1ULL << (n))
#define GENMASK(h, l)           (((~0UL) >> (BITS_PER_LONG - 1 - (h))) & (~0UL << (l)))
#define GENMASK_ULL(h, l)       (((~0ULL) >> (63 - (h))) & (~0ULL << (l)))
#define BITS_TO_LONGS(n)        (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)

#define U8_MAX                  ((u8)~0U)
#define U16_MAX                 ((u16)~0U)
#define U32_MAX                 ((u32)~0U)
#define U64_MAX                 ((u64)~0ULL)
#define S64_MAX                 ((s64)(U64_MAX >> 1))
#ifndef ULONG_MAX
#define ULONG_MAX               (~0UL)
#endif
#ifndef LONG_MAX
#define LONG_MAX                ((long)(~0UL >> 1))
#endif
#ifndef UINT_MAX
#define UINT_MAX                (~0U)
#endif
#ifndef INT_MAX
#define INT_MAX                 ((int)(~0U >> 1))
#endif

#undef min
#undef max
#define min(a, b)               ({ typeof(a) __a = (a); typeof(b) __b = (b); \
                                   __a < __b ? __a : __b; })
#define max(a, b)               ({ typeof(a) __a = (a); typeof(b) __b = (b); \
                                   __a > __b ? __a : __b; })
#define min_t(t, a, b)          min((t)(a), (t)(b))
#define max_t(t, a, b)          max((t)(a), (t)(b))
#define max3(a, b, c)           max(max(a, b), c)
#define min3(a, b, c)           min(min(a, b), c)
#define clamp(v, lo, hi)        min(max(v, lo), hi)

#define DIV_ROUND_UP(n, d)      (((n) + (d) - 1) / (d))
#define ALIGN(x, a)             (((x) + ((typeof(x))(a) - 1)) & ~((typeof(x))(a) - 1))
#define ALIGN_DOWN(x, a)        ((x) & ~((typeof(x))(a) - 1))
#define IS_ALIGNED(x, a)        (((x) & ((typeof(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a)         ((typeof(p))ALIGN((unsigned long)(p), (a)))
#undef roundup
#define roundup(x, y)           ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y)         ((x) - ((x) % (y)))

/* Linux errno names that illumos spells differently. */
#define ENOTSUPP                ENOTSUP
#define ERESTARTSYS             EINTR
#define EREMOTEIO               EIO

#define IS_ERR_VALUE(x)         unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error)
{
    return (void *)error;
}

static inline long PTR_ERR(const void *ptr)
{
    return (long)ptr;
}

static inline bool IS_ERR(const void *ptr)
{
    return IS_ERR_VALUE((unsigned long)ptr);
}

static inline bool IS_ERR_OR_NULL(const void *ptr)
{
    return !ptr || IS_ERR_VALUE((unsigned long)ptr);
}


#define printk                  linux_printk
#define vprintk                 linux_vprintk
#define pr_fmt(fmt)             fmt
#define pr_err(fmt, ...)        printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)       printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)       printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...)      no_printk(fmt, ##__VA_ARGS__)
#define pr_devel(fmt, ...)      no_printk(fmt, ##__VA_ARGS__)

static inline __attribute__((format(printf, 1, 2)))
int no_printk(const char *fmt, ...)
{
    return 0;
}

#define printk_ratelimited(fmt, ...)                                        \
    ({                                                                      \
        static struct ratelimit_state __rs;                                 \
        if (linux_ratelimit(&__rs))                                         \
            printk(fmt, ##__VA_ARGS__);                                     \
    })

#define DEFINE_RATELIMIT_STATE(name, interval, burst)                       \
    struct ratelimit_state name
#define __ratelimit(rs)         linux_ratelimit(rs)

#define dump_stack()            linux_dump_stack()

#define snprintf                linux_snprintf
#define vsnprintf               linux_vsnprintf
#define sprintf                 linux_sprintf

#define BUG()                   panic("BUG at %s:%d", __FILE__, __LINE__)
#define BUG_ON(cond)            do { if (unlikely(cond)) BUG(); } while (0)
#define WARN_ON(cond)                                                       \
    ({                                                                      \
        int __ret_warn = !!(cond);                                          \
        if (unlikely(__ret_warn))                                           \
            cmn_err(CE_WARN, "nvidia-uvm: WARN_ON(%s) at %s:%d", #cond,     \
                __FILE__, __LINE__);                                        \
        __ret_warn;                                                         \
    })
#define WARN_ON_ONCE(cond)      WARN_ON(cond)
#define WARN(cond, fmt, ...)    WARN_ON(cond)

static inline const char *kbasename(const char *path)
{
    const char *tail = strrchr(path, '/');

    return (tail != NULL) ? tail + 1 : path;
}

/* Time.  Linux jiffies are illumos clock ticks. */
#define HZ                      hz
#define NSEC_PER_USEC           1000L
#define NSEC_PER_MSEC           1000000L
#define USEC_PER_MSEC           1000L
#define NSEC_PER_SEC            1000000000L
#define USEC_PER_SEC            1000000L
#define MSEC_PER_SEC            1000L
#define MAX_JIFFY_OFFSET        ((LONG_MAX >> 1) - 1)

#define jiffies                 ((unsigned long)ddi_get_lbolt64())

static inline unsigned long usecs_to_jiffies(unsigned int us)
{
    return (unsigned long)drv_usectohz((clock_t)us);
}

static inline unsigned long msecs_to_jiffies(unsigned int ms)
{
    return (unsigned long)drv_usectohz((clock_t)ms * 1000);
}

static inline unsigned int jiffies_to_msecs(unsigned long j)
{
    return (unsigned int)(drv_hztousec((clock_t)j) / 1000);
}

static inline void ktime_get_raw_ts64(struct timespec64 *ts)
{
    hrtime_t now = gethrtime();

    ts->tv_sec = now / NANOSEC;
    ts->tv_nsec = now % NANOSEC;
}

static inline void ktime_get_ts64(struct timespec64 *ts)
{
    ktime_get_raw_ts64(ts);
}

static inline s64 timespec64_to_ns(const struct timespec64 *ts)
{
    return ((s64)ts->tv_sec * NSEC_PER_SEC) + ts->tv_nsec;
}

static inline ktime_t ktime_get(void)
{
    return gethrtime();
}

static inline s64 ktime_to_ns(ktime_t kt)
{
    return kt;
}

#define udelay(us)              drv_usecwait(us)
#define ndelay(ns)              drv_usecwait(((ns) + 999) / 1000)
#define mdelay(ms)              drv_usecwait((clock_t)(ms) * 1000)
#define usleep_range(lo, hi)    linux_usleep_range(lo, hi)
#define msleep(ms)              linux_msleep(ms)
#define ssleep(s)               linux_msleep((s) * 1000)
#define cpu_relax()             __asm__ __volatile__("pause" ::: "memory")
#define cond_resched()          ((void)0)
#define schedule()              linux_schedule()
#define yield()                 linux_schedule()

#define get_random_bytes(buf, len)  linux_get_random_bytes(buf, len)

#define sort(base, num, size, cmp, swap)                                    \
    linux_sort(base, num, size, cmp)

/* lookup3 final mix, as the Linux jhash. */
#define __jhash_rot(x, k)       (((x) << (k)) | ((x) >> (32 - (k))))
#define __jhash_final(a, b, c)                                              \
    {                                                                       \
        c ^= b; c -= __jhash_rot(b, 14);                                    \
        a ^= c; a -= __jhash_rot(c, 11);                                    \
        b ^= a; b -= __jhash_rot(a, 25);                                    \
        c ^= b; c -= __jhash_rot(b, 16);                                    \
        a ^= c; a -= __jhash_rot(c, 4);                                     \
        b ^= a; b -= __jhash_rot(a, 14);                                    \
        c ^= b; c -= __jhash_rot(b, 24);                                    \
    }
#define JHASH_INITVAL           0xdeadbeef

static inline u32 jhash_3words(u32 a, u32 b, u32 c, u32 initval)
{
    a += JHASH_INITVAL + initval + (3 << 2);
    b += JHASH_INITVAL + initval + (3 << 2);
    c += JHASH_INITVAL + initval + (3 << 2);
    __jhash_final(a, b, c);
    return c;
}

static inline u32 jhash_2words(u32 a, u32 b, u32 initval)
{
    return jhash_3words(a, b, 0, initval);
}

static inline u32 jhash_1word(u32 a, u32 initval)
{
    return jhash_3words(a, 0, 0, initval);
}

#endif /* _UVM_KPI_BASE_H_ */
