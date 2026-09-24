# illumos port: open work

Everything here builds and links, but nothing has run on a GPU yet.

## Needs a GPU

- Load all four modules, open every device node, and run a CUDA program.
- Run the UVM builtin tests (`uvm_enable_builtin_tests=1`).
- Measure the real kernel stack high-water mark on the UVM fault and ioctl
  paths. The static estimates are 15 to 19 KB of the 20 KB default stack;
  deep paths switch to a 64 KB stack through `thread_splitstack`.
- Exercise suspend and resume, GPU detach and re-attach, and NVSwitch
  attach.
- Check the `nvidia_i2c` controllers with `i2cadm` on a kernel that has
  `drv/i2cnex`.

## Design decisions still open

- RM-pinned user memory (`cudaHostRegister`): a munmap of the range without
  unregistering it hangs uninterruptibly, because an illumos pagelock cannot
  outlive its mapping and RM cannot revoke GPU access. The fix is
  RM-coordinated revocation from the umem cleanup callback, as hermon does.
  The alternative is to refuse these registrations.
- CUDA user libraries exist only for Linux. Running them in LX zones needs
  nvidia ioctl and mmap passthrough in the lx brand.

## Known gaps

- `thread_splitstack` is a SmartOS interface. On illumos-gate the driver
  warns and runs deep UVM paths on the caller's stack.
- Suspend with `NVreg_PreserveVideoMemoryAllocations` is refused, as it is
  on Linux outside the procfs suspend path.
- An `nvidia_i2c` controller stops working after its GPU re-attaches, until
  `nvidia_i2c` itself is re-attached.
- UVM module parameters are read once at start; changing one needs a reload.
- UVM test-file mappings do not survive fork.

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
