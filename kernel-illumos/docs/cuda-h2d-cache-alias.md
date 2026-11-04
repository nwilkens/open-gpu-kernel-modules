<!-- Copyright 2026 Edgecast Cloud LLC. -->

# Saruman CUDA upload corruption and the cache-alias experiment

## Status and scope

The illumos NVIDIA driver on Saruman's RTX 4060 Ti exposed an **observed
cache-type mismatch** on driver-owned CUDA upload staging RAM. The same
physical page had write-combined (WC) LX user and DDI kernel mappings, and a
default cacheable kernel direct-map mapping. That is a driver-level memory
policy problem. It is the leading explanation for intermittent stale
host-to-device (H2D) copies, but **no trace has shown an access through the
direct-map alias during a failed transfer**. The causal root of the corrupted
GPU bytes is therefore not proved.

Branch `cuda-cache-alias`, commit `59aff836e17063792444c80349cc89536a89c799`,
was an experimental driver-only correction. It chose a consistent cacheable
type for driver-owned RAM requested as WC. A later differential test found
that it broke the exact CUDA decode to HEVC NVENC control: the pre-policy
`nvidia` module passed with the other boot modules unchanged. Branch
`fix/sno-576-pre-policy` restores RM's requested cache type while retaining
later UVM fixes. This restores the pre-policy behavior in source; it does
not resolve the H2D corruption or pinned-host cleanup failures.

## Failure observed before the experiment

- Qwen2.5 7B Q4_K_M in llama.cpp b11827 sometimes generated repeated `G`
  characters after a fresh GPU process start. The same GGUF and long prompt
  answered correctly on the CPU.
- An independent CUDA reproducer found device-memory bytes matching the
  source **two transfers earlier**, including a synthetic source with no
  GGUF or file I/O. Pinned device-to-host readback and a tiny GPU kernel
  both observed stale device data. Sampling after synchronization found
  current bytes in the CPU staging slot, but that does not establish what
  the GPU read when its DMA operation began.
- The separate identity-DMA binding experiment at `7044902c` exercised its
  new bind path and still failed at transfer 3,076 of 5,000. An identity bind
  alone was not sufficient to correct this symptom.
- No corresponding NVIDIA Xid appeared in the inspected host log. Lack of
  an Xid is not proof that the transfer was correct.

## Driver path and measured memory types

1. RM requests `NV_MEMORY_WRITECOMBINED` from
   [`nv_alloc_pages()`](../nvidia/nvidia_mem.c) for the sampled 2 MiB upload
   staging allocation. Before this experiment, the request was stored
   unchanged in `at->cache_type`.
2. `nv_alloc_dma_chunks()` converted that type to
   `IOMEM_DATA_UC_WR_COMBINE` for `ddi_dma_mem_alloc()`. When the control
   device exported the allocation, [`nv_devmap_sysmem()`](../nvidia/nvidia_mmap.c)
   used `at->cache_type` for the user mapping too.
3. Live `mdb -k` resolved the staging user VA to the same physical page in
   the DDI and kernel direct mappings. It decoded the first two mappings as
   `wrcombine` and the direct mapping as the default type. These addresses
   and PFNs are observations, not stable interfaces.

| Mapping of the sampled prepatch PFN `0xa051` | PTE | Cache type |
| --- | --- | --- |
| LX user | `0x800000000a0510f7` | WC |
| DDI kernel | `0xa0512f3` | WC |
| Kernel direct map | `0xa051523` | Default/cacheable |

This mismatch is a concrete difference from the Linux adapter's handling of
noncached system pages: [`nv-vm.c`](../../kernel-open/nvidia/nv-vm.c) calls
`nv_set_memory_type()` to change the kernel memory type and restores
write-back before releasing the pages. The [Linux x86 PAT guidance](https://docs.kernel.org/arch/x86/pat.html)
warns against conflicting attributes for aliases of the same physical RAM.
The [illumos DMA allocation manual](https://smartos.org/man/9F/ddi_dma_mem_alloc)
documents the distinct cached and WC allocation flags. These references
explain why the mismatch deserves correction; they do not prove it caused
the observed two-transfer lag.

## Historical experimental correction

Commit `59aff836` mapped a WC request to `NV_MEMORY_CACHED` only at the `nv_alloc_pages()`
driver-owned RAM allocation boundary. The effective type then flows to the
DDI allocation, kernel mapping, and LX user mapping. Other cache requests
retain their original value. `nv_alias_pages()`, BARs, and peer I/O do not
use this conversion. That experiment traded WC write behavior for matching
cache attributes on this host. Its test and policy helper were removed with
the rollback.

The exact-source `b01slow` native build against illumos
`860a3aca2c761b58119d369f18efb837b9331747` and kernel
`joyent_20261028T004857Z` linked all four modules and passed 7,394 crypto
checks. Only `nvidia` was staged in Saruman PI `20261028T004857Z`. After a
supervised reboot, a sampled staging PFN `0xa021` had no WC bits in the LX
user, DDI kernel, or direct-map PTEs:

| Mapping of the sampled patched PFN `0xa021` | PTE | Cache type |
| --- | --- | --- |
| LX user | `0xa021067` | Default/cacheable |
| DDI kernel | `0xa021263` | Default/cacheable |
| Kernel direct map | `0xa021523` | Default/cacheable |

This confirms the policy change took effect on the tested allocation. It
does not establish which alias the CPU or GPU accessed in a failing run.

## Live validation and remaining limits

The first test passed 80,000 independent CUDA copies and six fresh Qwen GPU
processes. A second, 5-minute-29-second restart soak passed 24 more fresh
GPU processes, 48 exact short/6,937-token answers, and 240,000 synthetic
and GGUF-backed CUDA copies. Each model log had a nonzero CUDA0 buffer.
Patzer's separate LX zone could still query the GPU. The operator reported
that Saruman's physical console and keyboard worked after the first test.
No matching Xid or NVIDIA failure appeared in inspected `dmesg` output.

At the operator's request, Open WebUI's Qwen service was switched to 29 GPU
layers. Its process allocated about 4.5 GiB on the card, exact-answer probes
passed, and the operator reported that browser responses looked correct.
These results support continued supervised use on this host. They do not
cover hours of service uptime, all CUDA paths, other GPUs, or the throughput
effect of replacing WC with cacheable staging.

To establish causality and readiness, compare the old and new mappings under
a controlled workload, measure upload and inference throughput, and review
the cache policy independently. Keep bead `sno-27t` open until the mechanism
and acceptance limits are resolved. The detailed run script and per-cycle
output are in `smartos-nvidia-open-drivers-install` at
`benchmarks/cuda-cache-alias-20261030/` (commit `3d0fa80`). The previous
`nvidia` bootfs file is backed up on Saruman at
`/slow/boot/nvidia-backups/20261028T004857Z/cache-alias-59aff83-20261030/nvidia.before`.

## Paired 4 KiB KPM candidate, 2026-11-02

The paired illumos PI `joyent_20261102T175509Z` and NVIDIA module
`cc622d3b` loaded after the PI archive was correctly injected. A 30-frame
CUDA decode to HEVC NVENC control passed. The fresh GGUF mmap async H2D
integrity run still failed at iteration 49 with 64 bytes from two transfers
earlier. `mdb -k` showed only one three-page WC remap and 11 large KPM skips.
The failed allocation was skipped, so this candidate does not fix CUDA H2D
or test whether KPM alias coherence is causal. See bead `sno-27t` and the
installer report at `benchmarks/sno-7vl-dtrace-20261101/` (commit `bba5c00`).

The next source draft adds `hat_kpm_prepare_cache()` to split only a large
boot-created KPM leaf covering a driver-owned WC PFN. It prepares a child
page table, copies the original leaf's attributes including `PT_GLOBAL`,
atomically substitutes the parent PTE, and invalidates the old translation.
It rejects later HAT-locked mappings, since those have a separate lock-count
lifecycle. The driver calls it before the existing 4 KiB cache transition.
This draft has passed local style and source-contract checks but has not
been built into a kernel or tested on Saruman. The build host was offline
when this note was updated; do not treat this as a verified fix.
