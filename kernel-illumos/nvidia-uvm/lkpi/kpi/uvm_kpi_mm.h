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
 * Memory: kernel allocators, pages, address spaces and DMA.  struct page is
 * illumos page_t; struct mm_struct pointers are struct as pointers.
 */

#ifndef _UVM_KPI_MM_H_
#define _UVM_KPI_MM_H_

#define offset_in_page(p)       ((unsigned long)(p) & ~PAGE_MASK)
#define MAX_PAGE_ORDER          10
#define MAX_ORDER               MAX_PAGE_ORDER

#define PAGE_ALIGN(a)           ALIGN((a), PAGE_SIZE)
#define PAGE_ALIGNED(a)         IS_ALIGNED((unsigned long)(a), PAGE_SIZE)
#define offset_in_page(p)       ((unsigned long)(p) & ~PAGE_MASK)

static inline int get_order(unsigned long size)
{
    return (size <= PAGE_SIZE) ? 0 : ilog2_u64((size - 1) >> PAGE_SHIFT) + 1;
}


#define kmalloc(size, gfp)      linux_kmalloc(size, gfp)
#define kzalloc(size, gfp)      linux_kmalloc(size, (gfp) | __GFP_ZERO)
#define kcalloc(n, size, gfp)   linux_kmalloc((n) * (size), (gfp) | __GFP_ZERO)
#define krealloc(p, size, gfp)  linux_krealloc(p, size, gfp)
#define kfree(p)                linux_kfree(p)
#define ksize(p)                linux_ksize(p)
#define vmalloc(size)           linux_vmalloc(size, B_FALSE)
#define vzalloc(size)           linux_vmalloc(size, B_TRUE)
#define vfree(p)                linux_vfree(p)
#define kvmalloc(size, gfp)     linux_vmalloc(size, ((gfp) & __GFP_ZERO) != 0)
#define kvfree(p)               linux_vfree(p)
#define is_vmalloc_addr(p)      linux_is_vmalloc_addr(p)

/* struct kmem_cache is illumos kmem_cache_t; only the allocation flags differ. */
#undef kmem_cache_alloc
#define kmem_cache_alloc(c, gfp)    linux_kmem_cache_alloc(c, gfp)
#define kmem_cache_zalloc(c, gfp)   linux_kmem_cache_alloc(c, (gfp) | __GFP_ZERO)

/* Pages. */
#define alloc_pages(gfp, order)             linux_alloc_pages(gfp, order)
#define alloc_pages_node(nid, gfp, order)   linux_alloc_pages(gfp, order)
#define alloc_page(gfp)                     linux_alloc_pages(gfp, 0)
#define __free_pages(pp, order)             linux_free_pages(pp, order)
#define __free_page(pp)                     linux_free_pages(pp, 0)
#define put_page(pp)                        linux_put_page(pp)
#define get_page(pp)                        ((void)(pp))
#define page_to_pfn(pp)                     linux_page_to_pfn(pp)
#define pfn_to_page(pfn)                    linux_pfn_to_page(pfn)
#define page_to_phys(pp)                    ((phys_addr_t)page_to_pfn(pp) << PAGE_SHIFT)
#define __phys_to_pfn(pa)                   ((unsigned long)((pa) >> PAGE_SHIFT))
#define page_address(pp)                    linux_page_address(pp)
#define kmap(pp)                            linux_page_address(pp)
#define kmap_atomic(pp)                     linux_page_address(pp)
#define kmap_local_page(pp)                 linux_page_address(pp)
#define kunmap(pp)                          ((void)(pp))
#define kunmap_atomic(va)                   ((void)(va))
#define kunmap_local(va)                    ((void)(va))
#define set_page_dirty(pp)                  linux_set_page_dirty(pp)
#define set_page_dirty_lock(pp)             linux_set_page_dirty(pp)
#define page_count(pp)                      ((void)(pp), 1)
#define page_ref_count(pp)                  ((void)(pp), 1)
#define set_page_count(pp, n)               ((void)(pp), (void)(n))
#define page_to_nid(pp)                     ((void)(pp), 0)
#define PageSwapCache(pp)                   ((void)(pp), 0)
#define lock_page(pp)                       ((void)(pp))
#define unlock_page(pp)                     ((void)(pp))
#define virt_addr_valid(va)                 ((void)(va), false)
#define virt_to_page(va)                    ((void)(va), (struct page *)NULL)
#define vmalloc_to_page(va)                 ((void)(va), (struct page *)NULL)
#define is_device_private_page(pp)          ((void)(pp), false)
#define is_device_coherent_page(pp)         ((void)(pp), false)
#define is_pci_p2pdma_page(pp)              ((void)(pp), false)

static inline void memzero_page(struct page *pp, size_t off, size_t len)
{
    memset((char *)page_address(pp) + off, 0, len);
}

#define VM_MAP                  0x4
#define vmap(pages, n, flags, prot) ((void)(prot), linux_vmap(pages, n))
#define vunmap(va)              linux_vunmap(va)

/* Protections are illumos PROT_* bits. */
#define pgprot_val(p)           ((p).prot)
#define __pgprot(v)             ((pgprot_t) { (v) })
#define PAGE_KERNEL             __pgprot(PROT_READ | PROT_WRITE)
#define PAGE_KERNEL_NOENC       PAGE_KERNEL
#define PAGE_KERNEL_RO          __pgprot(PROT_READ)
#define pgprot_decrypted(p)     (p)
#define pgprot_writecombine(p)  (p)
#define pgprot_noncached(p)     (p)


static inline pgprot_t vm_get_page_prot(vm_flags_t flags)
{
    return __pgprot(PROT_READ | ((flags & VM_WRITE) ? PROT_WRITE : 0) |
        ((flags & VM_EXEC) ? PROT_EXEC : 0));
}


#define vma_is_anonymous(vma)   ((vma)->vm_ops == NULL)
#define vma_is_dax(vma)         ((void)(vma), false)
#define is_vm_hugetlb_page(vma) ((void)(vma), false)
#define vma_policy(vma)         ((struct mempolicy *)NULL)
#define userfaultfd_armed(vma)  ((void)(vma), false)
static inline vm_fault_t handle_mm_fault(struct vm_area_struct *vma,
    unsigned long addr, unsigned int flags)
{
    return VM_FAULT_SIGBUS;
}

#define vm_insert_page(vma, addr, pp)   linux_vm_insert_page(vma, addr, pp)
#define unmap_mapping_range(m, off, len, cows) \
    linux_unmap_mapping_range(m, off, len, cows)
#define address_space_init_once(m)  linux_address_space_init_once(m)
#define find_vma(mm, addr)      linux_find_vma(mm, addr)
#define find_vma_intersection(mm, s, e) linux_find_vma_intersection(mm, s, e)
#define vma_lookup(mm, addr)    linux_find_vma_intersection(mm, addr, (addr) + 1)

/* va_space_mm is disabled, so the mm reference helpers have no work. */
#define mmget_not_zero(mm)      ((void)(mm), false)
#define mmput(mm)               ((void)(mm))
#define mmgrab(mm)              ((void)(mm))
#define mmdrop(mm)              ((void)(mm))

/* memcg: an mm stands for its owner's project and zone (uvm_illumos_acct.c). */
struct mem_cgroup *linux_get_mem_cgroup_from_mm(struct mm_struct *);
struct mem_cgroup *linux_set_active_memcg(struct mem_cgroup *);
#define get_mem_cgroup_from_mm(mm)  linux_get_mem_cgroup_from_mm(mm)
#define set_active_memcg(memcg)     linux_set_active_memcg(memcg)
#define mem_cgroup_put(memcg)       ((void)(memcg))

/* Mapping of user pages. */
#define NV_PIN_USER_PAGES(start, n, flags, pages)                           \
    linux_pin_user_pages(start, n, flags, pages)
#define NV_UNPIN_USER_PAGE(pp)  linux_unpin_user_page(pp)

/* DMA. */
enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE = 1,
    DMA_FROM_DEVICE = 2,
    DMA_NONE = 3,
};

#define DMA_BIT_MASK(n)         (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define dma_map_page(dev, pp, off, size, dir)                               \
    linux_dma_map_page(dev, pp, off, size)
#define dma_unmap_page(dev, addr, size, dir)                                \
    linux_dma_unmap_page(dev, addr, size)
#define dma_mapping_error(dev, addr)    ((addr) == DMA_MAPPING_ERROR)
#define dma_alloc_coherent(dev, size, handle, gfp)                          \
    linux_dma_alloc_coherent(dev, size, handle, gfp)
#define dma_free_coherent(dev, size, va, handle)                            \
    linux_dma_free_coherent(dev, size, va, handle)
#define dma_to_phys(dev, addr)  ((phys_addr_t)(addr))
#define dev_to_node(dev)        ((void)(dev), NUMA_NO_NODE)
#define dev_get_platdata(dev)   ((void *)NULL)
#define iommu_get_domain_for_dev(dev)   ((struct iommu_domain *)NULL)

/* Scatterlists, used only by code the illumos build disables. */
#define sg_dma_len(sg)          ((sg)->dma_length)
#define sg_dma_address(sg)      ((sg)->dma_address)
#define sg_next(sg)             ((struct scatterlist *)NULL)
#define sg_page_iter_dma_address(it)    ((dma_addr_t)0)

/* A single NUMA node. */
#define NUMA_NO_NODE            (-1)
#define MAX_NUMNODES            1
#define nr_node_ids             1
#define NODE_DATA(nid)          NULL
#define numa_mem_id()           0
#define numa_node_id()          0
#define num_possible_nodes()    1
#define num_online_nodes()      1
#define node_distance(a, b)     ((a) == (b) ? 10 : 20)
#define node_start_pfn(nid)     0UL
#define node_end_pfn(nid)       ((unsigned long)physmax + 1)
#define node_state(nid, state)  ((nid) == 0)
#define N_MEMORY                0
#define N_CPU                   1
#define N_ONLINE                2
#define N_POSSIBLE              3

extern nodemask_t linux_node_online_map;
#define node_online_map         linux_node_online_map
#define node_possible_map       linux_node_online_map

#define node_isset(n, mask)     ((n) >= 0 && (n) < MAX_NUMNODES && test_bit(n, (mask).bits))
#define node_set(n, mask)       __set_bit(n, (mask).bits)
#define node_clear(n, mask)     __clear_bit(n, (mask).bits)
#define node_test_and_set(n, mask)  __test_and_set_bit(n, (mask).bits)
#define nodes_clear(mask)       bitmap_zero((mask).bits, MAX_NUMNODES)
#define nodes_empty(mask)       bitmap_empty((mask).bits, MAX_NUMNODES)
#define __nodes_weight(m, n)    bitmap_weight((m)->bits, n)
#define nodes_weight(mask)      bitmap_weight((mask).bits, MAX_NUMNODES)
#define first_node(mask)        ((int)find_first_bit((mask).bits, MAX_NUMNODES))
#define next_node(n, mask)      ((int)find_next_bit((mask).bits, MAX_NUMNODES, (n) + 1))
#define for_each_node_mask(node, mask)                                      \
    for ((node) = first_node(mask); (node) < MAX_NUMNODES;                  \
         (node) = next_node(node, mask))
#define for_each_node(node)     for_each_node_mask(node, node_possible_map)
#define for_each_online_node(node)  for_each_node_mask(node, node_online_map)
#define for_each_node_state(node, state) for_each_node_mask(node, node_online_map)

#endif /* _UVM_KPI_MM_H_ */
