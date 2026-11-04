<!-- Copyright 2026 Edgecast Cloud LLC. -->

# Guarded KPM cache-attribute experiment for CUDA upload pages

## Question

Does the write-back KPM alias of a driver-owned write-combined (WC) page cause
Saruman's intermittent stale CUDA host-to-device data? The current driver
passes NVDEC/NVENC and pinned-host allocation lifecycle tests, but a fresh
pageable CUDA copy process observed 1,728 old bytes at transfer 3,950 and a
pinned Qwen model produced incorrect output. The exact live evidence is in
`smartos-nvidia-open-drivers-install/benchmarks/sno-7vl-dtrace-20261101/`.

## Design

Keep RM's requested WC policy and the existing DDI and user mappings. For
WC system RAM allocated through `nv_alloc_pages()`, inspect every physical
page's KPM mapping before changing any of them. Require an existing 4 KiB
mapping of the same PFN. If any page fails that check, leave the entire
allocation unchanged and count the skip. This diagnostic build must never
attempt to overwrite a large KPM mapping: illumos `htable_create()` panics
if it links a 4 KiB table over a present large-page entry.

The first driver-only implementation used `hat_devload()` for the transition.
That implementation panicked before CUDA testing; see the result below. The
revised implementation calls illumos `hat_kpm_set_cache()` to change only
`PT_WRITETHRU`, `PT_NOCACHE`, and `PT_PAT_4K` in the existing PTE. The HAT
checks the PFN and expected cache mode, atomically updates the PTE, and
shoots down the TLB. The driver flushes CPU caches on all CPUs before and
after a transition, and rolls back a partial WC conversion. If rollback or
WB restoration fails, the driver retains the DMA allocation and its mappings
for mdb diagnosis rather than releasing pages with uncertain cache state. Keep
the allocation marked as remapped so its last-reference destruction flushes
again and restores KPM write-back before `ddi_dma_mem_free()` releases pages.
Do not touch BAR, peer, user-pinned, or alias allocations.

This is a guarded diagnostic, not a production cache policy. It uses a full
CPU cache flush per transition and does not handle large KPM mappings. A
measured skip on the CUDA staging allocation means this test is inconclusive
and a small illumos HAT operation with safe large-page demotion is needed.
The HAT rechecks the 4 KiB mapping and cache bits during each transition.
It rejects a missing mapping, changed PFN, unexpected cache policy, or a
large KPM mapping. The driver still requires an exact matching illumos
kernel; the new HAT symbol is absent from the currently running PI. Verify
the modified kernel and driver together with mdb before accepting any CUDA
result. A large-page skip is inconclusive for the alias hypothesis.

## Acceptance and rollback

The existing `cuda_copy_integrity` reproducer failed on the pre-policy
candidate before this patch. On a separately approved boot, inspect the
live KPM, DDI, and user PTEs for the same staging PFN; verify the diagnostic
counter confirms an actual remap. Then run repeated fresh-process pageable
H2D copies, pinned lifecycle and pinned Qwen correctness, the exact 30-frame
NVDEC/NVENC control at idle and with Qwen active, and check for Xid or
teardown errors. A passing run would support the alias hypothesis but still
needs independent review and broader soak before production acceptance.

Only `nvidia.kmod` changes; the other three integrated modules stay paired.
Before another reboot, back up the current module, take a `slow/boot` ZFS
snapshot, and stage the candidate with exact hashes. Do not reboot without
the operator's explicit approval. Restore the backed-up module if this
experiment fails; that restoration also requires an approved reboot to load.

## Result: do not deploy this driver-only experiment

The approved boot loaded this diagnostic, but the first idle 30-frame
NVDEC/NVENC control panicked Saruman at 2026-11-02 16:53:52 UTC. The saved
panic is `remap bits changed: old_pte=0xa071523,
pte=0x800000000a0714f3`. The only changed bit outside native
`PT_REMAP_BITS` is `PT_GLOBAL` (bit 8): it was present in the KPM entry and
absent in the entry rebuilt by `hat_devload()`. Native `PT_IGNORE` is zero,
and the loaded kernel's `mmu.pt_global` is zero under KPTI. The exact HAT
implementation calls `x86pte_set()` before checking the remap bits, so this
path writes the unsuitable PTE and then panics. The pageable H2D and pinned
Qwen tests were not reached. This experiment establishes a driver-only
remapping failure; it does not establish whether the original WB KPM alias
causes stale CUDA uploads.

The prior module and Qwen configuration are restored on disk. The failed
module remains loaded until a separately approved recovery reboot. The
compressed dump is `/slow/crash/vmdump.5`; `savecore -f` reported an
incomplete dump and left an unreadable partial core. See the
`sno-7vl-dtrace-20261101` installer report for raw logs.

The follow-up patch adds that direct-map cache-attribute operation for
existing 4 KiB mappings in illumos HAT and switches the driver to it. The
HAT rejects large mappings; safe large-page demotion remains separate work.
This follow-up has not booted or run CUDA/NVENC validation. The failed
driver-only experiment must not be reused.

The matching illumos source is branch `fix/sno-27t-kpm-memattr`, commit
`8dcf80d15d`, based on the October PI's `860a3aca2c76`. A new illumos
`unix`/PI build is required before loading this driver: the currently
running kernel does not export `hat_kpm_set_cache`. The matching illumos
commit is also packaged as
[`kernel-illumos/patches/illumos-joyent-kpm-cache.patch`](../patches/illumos-joyent-kpm-cache.patch)
for a builder that has the exact October source.
