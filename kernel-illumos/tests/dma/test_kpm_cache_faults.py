#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Edgecast Cloud LLC.
# SPDX-License-Identifier: MIT
"""Fault inject the real KPM transition and DMA teardown functions."""

from pathlib import Path
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[2] / "nvidia/nvidia_mem.c"


def function(source, name):
    start = source.index(f"\n{name}(")
    start = source.rfind("static NvBool", 0, start)
    opening = source.index("{", start)
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start:offset + 1]
    raise AssertionError(f"unterminated {name}")


source = SOURCE.read_text()
code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef unsigned int NvU32;
typedef unsigned int NvBool;
typedef unsigned int uint_t;
typedef unsigned long pfn_t;
typedef unsigned long ulong_t;
typedef char *caddr_t;
#define NV_TRUE 1
#define NV_FALSE 0
#define HAT_STORECACHING_OK 0x400
#define HAT_MERGING_OK 0x200
#define NV_DBG_ERRORS 1
#define mmu_btop(x) ((x) / 4096)

typedef struct { unsigned long phys_addr; } nvidia_pte_t;
typedef struct { void *acc_handle, *dma_handle; } nv_dma_chunk_t;
typedef struct {
    nvidia_pte_t *page_table;
    NvU32 num_pages, num_chunks;
    caddr_t kva;
    nv_dma_chunk_t *chunks;
    struct { unsigned kpm_wc:1, kpm_quarantine:1; } flags;
} nv_illumos_alloc_t;

static volatile uint64_t nv_kpm_wc_restore_allocs;
static int script[8], calls, flushes, unmaps, frees;
static int hat_kpm_set_cache(pfn_t pfn, uint_t before, uint_t after)
{ (void)pfn; (void)before; (void)after; return script[calls++]; }
static void nv_kpm_wc_flush_all(void) { flushes++; }
static void nv_printf(int level, const char *fmt, ...)
{ (void)level; (void)fmt; }
static void nv_kunmap_phys(caddr_t va, NvU32 pages)
{ (void)va; (void)pages; unmaps++; }
static void ddi_dma_mem_free(void **handle)
{ (void)handle; frees++; }
static void ddi_dma_free_handle(void **handle)
{ (void)handle; frees++; }
static void kmem_free(void *ptr, size_t size)
{ (void)ptr; (void)size; frees++; }
static void atomic_inc_64(volatile uint64_t *value) { ++*value; }
''' + function(source, "nv_kpm_wc_set") + "\n" + function(source, "nv_free_dma_chunks") + r'''

int main(void)
{
    nvidia_pte_t pages[2] = {{4096}, {8192}};
    nv_dma_chunk_t chunk = {(void *)1, (void *)2};
    nv_illumos_alloc_t alloc = {0};

    alloc.page_table = pages;
    alloc.num_pages = 2;
    alloc.num_chunks = 1;
    alloc.chunks = &chunk;
    alloc.kva = (caddr_t)3;
    script[0] = 0; script[1] = 5; script[2] = 5;
    assert(nv_kpm_wc_set(&alloc, NV_TRUE) == NV_FALSE);
    assert(alloc.flags.kpm_quarantine == 1 && calls == 3);
    assert(nv_free_dma_chunks(&alloc) == NV_FALSE);
    assert(frees == 0 && unmaps == 0 && alloc.kva == (caddr_t)3);

    alloc.flags.kpm_quarantine = 0;
    alloc.flags.kpm_wc = 1;
    alloc.num_pages = 1;
    calls = 0;
    script[0] = 5;
    assert(nv_free_dma_chunks(&alloc) == NV_FALSE);
    assert(alloc.flags.kpm_quarantine == 1);
    assert(frees == 0 && unmaps == 0 && alloc.kva == (caddr_t)3);

    alloc.flags.kpm_quarantine = 0;
    calls = 0;
    script[0] = 0;
    assert(nv_free_dma_chunks(&alloc) == NV_TRUE);
    assert(frees == 3 && unmaps == 0 && alloc.kva == NULL);
    assert(alloc.flags.kpm_wc == 0);
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    cfile = Path(directory) / "faults.c"
    exe = Path(directory) / "faults"
    cfile.write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
