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
 * Files, character devices, polling, user copies and module boilerplate.
 */

#ifndef _UVM_KPI_FILE_H_
#define _UVM_KPI_FILE_H_

/* illumos <sys/file.h> owns the file tag. */
#define file                    linux_file

#define fget(fd)                linux_fget(fd)
#define fput(f)                 linux_fput(f)
#define file_inode(f)           ((f)->f_inode)

#define MINORBITS               20
#define MINORMASK               ((1U << MINORBITS) - 1)
#define MAJOR(dev)              ((unsigned int)getmajor(dev))
#define MINOR(dev)              ((unsigned int)getminor(dev))
#define MKDEV(ma, mi)           makedevice((major_t)(ma), (minor_t)(mi))

/* The nvidia_uvm driver owns its major; the region is its base minors. */
static inline int alloc_chrdev_region(dev_t *dev, unsigned int baseminor,
    unsigned int count, const char *name)
{
    *dev = MKDEV(0, baseminor);
    return 0;
}

#define unregister_chrdev_region(dev, count)    ((void)(dev))

static inline void cdev_init(struct cdev *cdev,
    const struct file_operations *fops)
{
    memset(cdev, 0, sizeof(*cdev));
    cdev->ops = fops;
}

#define cdev_add(cdev, dev, count)  linux_cdev_add(cdev, dev, count)
#define cdev_del(cdev)              linux_cdev_del(cdev)

#define poll_wait(filp, wq, pt)     linux_poll_wait(filp, wq, pt)

/* UVM ioctls have no in-kernel callers, so user copies are always copyin. */
static inline unsigned long copy_from_user(void *to, const void *from,
    unsigned long n)
{
    return (copyin(from, to, n) == 0) ? 0 : n;
}

static inline unsigned long copy_to_user(void *to, const void *from,
    unsigned long n)
{
    return (copyout(from, to, n) == 0) ? 0 : n;
}

#define get_user(x, ptr)                                                    \
    (copy_from_user(&(x), (ptr), sizeof(*(ptr))) ? -EFAULT : 0)
#define put_user(x, ptr)                                                    \
    ({ typeof(*(ptr)) __pu_v = (x);                                         \
       copy_to_user((ptr), &__pu_v, sizeof(*(ptr))) ? -EFAULT : 0; })

#define S_IRUGO                 0444
#define S_IWUSR                 0200

/* Tunables keep their defaults. */
#define module_param(name, type, perm)                                      \
    static const void *__module_param_##name __attribute__((unused)) = &(name)
#define module_param_named(name, var, type, perm)                           \
    module_param(var, type, perm)
#define module_param_string(name, str, len, perm)                           \
    module_param(str, charp, perm)
#define MODULE_PARM_DESC(name, desc)
#define MODULE_LICENSE(s)
#define MODULE_INFO(tag, s)
#define MODULE_VERSION(s)
#define MODULE_DESCRIPTION(s)
#define MODULE_AUTHOR(s)
#define MODULE_IMPORT_NS(ns)
#define MODULE_SOFTDEP(s)
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define THIS_MODULE             ((struct module *)NULL)

/* module_init/module_exit name the entry points for the illumos driver. */
#define module_init(fn)                                                     \
    int nv_uvm_module_init(void) { return fn(); }
#define module_exit(fn)                                                     \
    void nv_uvm_module_exit(void) { fn(); }

/* procfs is not available. */
#define seq_printf(sf, fmt, ...)    ((void)(sf), no_printk(fmt, ##__VA_ARGS__))
#define seq_puts(sf, str)           ((void)(sf), (void)(str))
#define proc_remove(e)              ((void)(e))
#define proc_symlink(n, p, d)       ((void)(p), (struct proc_dir_entry *)NULL)

/* MMIO mappings of device memory, used only by disabled device P2P code. */
#define ioremap(pa, size)           ((void __iomem *)NULL)
#define ioremap_cache(pa, size)     ((void __iomem *)NULL)
#define iounmap(va)                 ((void)(va))

static inline u32 ioread32(const volatile void *addr)
{
    return *(const volatile u32 *)addr;
}

static inline void iowrite32(u32 v, volatile void *addr)
{
    *(volatile u32 *)addr = v;
}

static inline void iowrite64(u64 v, volatile void *addr)
{
    *(volatile u64 *)addr = v;
}

#define resource_size(r)            ((r)->end - (r)->start + 1)
#define pci_resource_start(pdev, bar)   ((resource_size_t)0)
#define pci_resource_len(pdev, bar)     ((resource_size_t)0)
#define pci_dev_put(pdev)               ((void)(pdev))
#define pci_device_is_present(pdev)     ((void)(pdev), true)

#endif /* _UVM_KPI_FILE_H_ */
