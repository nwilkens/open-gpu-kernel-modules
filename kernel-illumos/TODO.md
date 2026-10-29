# illumos port: open work

The four modules have loaded on SmartOS with an RTX 4060 Ti. CUDA decode,
NVENC encode, and an interrupted LX-zone stop passed on one host and kernel.
Those tests do not cover every RM failure path or untrusted-zone operation.

## Needs wider GPU validation

- Run the UVM builtin tests (`uvm_enable_builtin_tests=1`).
- Measure the real kernel stack high-water mark on the UVM fault and ioctl
  paths. The static estimates are 15 to 19 KB of the 20 KB default stack;
  deep paths switch to a 64 KB stack through `thread_splitstack`.
- Exercise suspend and resume, GPU detach and re-attach, and NVSwitch
  attach.
- Check the `nvidia_i2c` controllers with `i2cadm` on a kernel that has
  `drv/i2cnex`.

## Design decisions still open

- RM-pinned user memory (`cudaHostRegister`) has checked UVM import teardown
  for the observed interrupted NVENC zone stop. The callback retains the pin
  if GPU or IOMMU removal fails. Ordinary RM duplicates and virtual-memory
  DMA mappings of a tracked pin are refused before they can acquire an
  untracked reference; UVM's own tracked duplicate and mapping client are
  admitted. Other RM mapping paths still need an ownership audit.
- A raw RM channel can share a UVM VA space without appearing in UVM's
  registered-channel list. On illumos, RM now refuses GPFIFO scheduling or
  BIND of an unbound user channel in an externally owned VA space, including
  CE channels. BIND needed its own gate because it can schedule without a
  separate GPFIFO schedule control.
  UVM-owned kernel channels remain schedulable. The callback certifies only
  raw channels that were never bound and have no failed-revoke bit; any other
  unrevoked channel retains the pin. A direct BIND attempt on an eligible
  channel in a disposable LX zone returned `NV_ERR_INVALID_STATE` before UVM
  registration on one GPU. The follow-up wrapper preflight that avoids an
  assertion-style log for that expected denial has passed focused tests and
  a SmartOS module build, but has not yet booted. An adversarial untrusted-zone
  test and an audit of alternate submission paths are still needed before
  claiming complete isolation.
- CUDA user libraries exist only for Linux. Companion illumos-joyent PR #564
  supplies the LX ioctl, device identity, and procfs behavior needed by those
  libraries; CUDA decode and NVENC passed with it on one SmartOS host. The LX
  change is not yet merged, and device assignment does not establish safe
  sharing with untrusted zones.

## Known gaps

- `thread_splitstack` is a SmartOS interface. On illumos-gate the driver
  warns and runs deep UVM paths on the caller's stack.
- Suspend with `NVreg_PreserveVideoMemoryAllocations` is refused, as it is
  on Linux outside the procfs suspend path.
- An `nvidia_i2c` controller stops working after its GPU re-attaches, until
  `nvidia_i2c` itself is re-attached.
- UVM module parameters are read once at start; changing one needs a reload.
- UVM test-file mappings do not survive fork.
- UVM memory is charged to the process that opened `/dev/nvidia-uvm`, even
  if the descriptor is passed to another process. Charges taken on threads
  outside that project skip rctl syslog and signal actions (deny still
  applies).
- Memory kept after a failed DMA unbind is charged to the global zone's
  project 0 and never freed.

## To report upstream

- NVIDIA: `libspdm_encode_base64_with_newline` in
  `src/nvidia/src/libraries/libspdm/nvidia/nvspdm_cert.c` writes one byte past
  its buffer.
- NVIDIA: the Linux libspdm crypto backend in `kernel-open` steps HKDF expand
  by the wrong length, indexes base64 decode wrongly, and writes through a
  NULL output size in AEAD.
- illumos: `i2c_ctrl_register()` returns with `root->ir_mutex` held when it
  rejects a duplicate name.
- illumos: `i2c_ctrl_port_name_portno()` uses `sizeof` of a pointer as the
  buffer size.
