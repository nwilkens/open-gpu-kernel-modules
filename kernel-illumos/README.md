# NVIDIA kernel modules for illumos

This directory holds the illumos kernel interface layer. It is linked with the
OS-agnostic `nv-kernel.o` and `nv-modeset-kernel.o` objects built from `src/`,
and, for `nvidia_uvm`, with the `kernel-open/nvidia-uvm` sources. It builds
these drivers:

| Module | Device nodes |
| --- | --- |
| `nvidia` | `/dev/nvidiactl`, `/dev/nvidiaN`, `/dev/nvidia-caps/*`, `/dev/nvidia-caps-imex-channels/*`, `/dev/nvidia-nvlink`, `/dev/nvidia-nvswitchctl`, `/dev/nvidia-nvswitchN` |
| `nvidia_modeset` | `/dev/nvidia-modeset` |
| `nvidia_i2c` | none; GPU I2C controllers in the illumos I2C framework |
| `nvidia_uvm` | `/dev/nvidia-uvm`, `/dev/nvidia-uvm-tools` |

nvidia-drm and nvidia-peermem are not built. illumos has no Linux DRM and no
InfiniBand peer-memory interface.

## Building

Building needs GCC and GNU binutils (`gld`, `gobjcopy`). It also needs an
illumos source tree that matches the running platform, for kernel headers.
From the top of the repository:

    gmake modules ILLUMOS_SRC=/path/to/illumos-gate

`ILLUMOS_SRC` defaults to `/opt/illumos-joyent-hdrs`. The modules are written
to `kernel-illumos/_out/SunOS_x86_64/`.

## Installing

    gmake modules_install DESTDIR=/
    update_drv -a -i "$(tr '\n' ' ' < kernel-illumos/_out/SunOS_x86_64/nvidia.aliases)" nvidia \
        || add_drv -m '* 0666 root sys' \
           -i "$(tr '\n' ' ' < kernel-illumos/_out/SunOS_x86_64/nvidia.aliases)" nvidia
    add_drv -m '* 0666 root sys' nvidia_modeset
    add_drv nvidia_i2c
    add_drv -m '* 0666 root sys' nvidia_uvm

`nvidia.aliases` is generated from the supported-GPU table in the top-level
`README.md`, plus the NVSwitch device IDs.

Append `devlink.tab` to `/etc/devlink.tab` so that devfsadm creates the `/dev`
names listed above.

GSP firmware is not part of this repository. Copy `gsp_*.bin` from the
`firmware/` directory of the matching NVIDIA Linux driver package into
`/kernel/firmware/nvidia/<version>/`.

## Configuration

The Linux `NVreg_*` module parameters are driver properties of the same name
in `/kernel/drv/nvidia.conf`, for example:

    NVreg_EnableMSI=1;
    NVreg_RegistryDwords="RMUseSwI2c=0x01";

NVSwitch settings use the Linux parameter names `NvSwitchRegDwords` and
`NvSwitchBlacklist`.

Confidential Computing is not supported, and `RmConfidentialCompute` settings
are ignored.

## GPU I2C buses

`nvidia_i2c` registers one controller per GPU with the illumos I2C framework,
so `i2cadm` and libi2c can reach the GPU's DDC and other I2C buses. It needs an
illumos kernel that ships the framework (`drv/i2cnex`).

Each controller is named `nvgpuBBDDF` after the GPU's PCI bus, device and
function, in hex. Its ports 0 to 15 are RM's I2C port numbers. A port carries
I/O only while the GPU is initialized and RM exports that port.

Plain reads and writes of up to 256 bytes work on every exported port. Ports
that are not routed over DP AUX also take a one-byte write followed by a read
with a repeated start, and SMBus quick, byte, word and block write, and I2C
block read and write of up to 32 bytes. Other write-then-read requests are
refused.

A controller belongs to one attach of its GPU. If the GPU is detached and
attached again, the controller fails all I/O until `nvidia_i2c` is detached
and attached again.

## Unified Memory

`nvidia_uvm` builds the unmodified `kernel-open/nvidia-uvm` sources against a
Linux compatibility layer in `nvidia-uvm/lkpi`. The files in `nvidia-uvm`
supply the device, the segment driver that backs its mappings, and CPU page
allocation. To pick up a new driver release, update `kernel-open/nvidia-uvm`
and rebuild; source files added upstream are taken from
`nvidia-uvm-sources.Kbuild`.

UVM starts on the first open of `/dev/nvidia-uvm` after an `nvidia` instance
has attached. Until then, opens fail with `ENXIO`.

These are not supported yet: GPUs behind an IOMMU, access to pageable memory
(HMM and ATS), and tools event queues. Without an IOMMU, a registered GPU can
reach all of physical memory, as on Linux. Only 64-bit processes can map
`/dev/nvidia-uvm`.

CPU faults on UVM mappings, UVM ioctls and the last close of a UVM file can
call into RM, which needs more than the 20 KB stack of an LWP. They run on a
64 KB stack from `thread_splitstack()`. On a kernel without it, `nvidia_uvm`
logs a warning at attach and runs them on the caller's stack.

### UVM module parameters

The Linux `nvidia-uvm` module parameters are properties of the same name in
`/kernel/drv/nvidia_uvm.conf`. They are read once, when the first open starts
UVM. To change them, edit the file, then unload and reload `nvidia_uvm`.
Parameters that Linux lets you change at run time through sysfs are fixed
once UVM has started.

    uvm_perf_prefetch_threshold=75;
    uvm_perf_fault_coalesce=0;
    uvm_channel_gpfifo_loc="sys";
    uvm_disable_hmm="y";

- `int`, `uint` and `ulong` parameters take an integer, or a string in the
  form Linux accepts (decimal, `0x` hex or `0` octal). Use the string form
  for unsigned values above 2147483647.
- `bool` parameters take 0 or 1, or one of the strings `y`, `yes`, `t`,
  `true`, `on`, `n`, `no`, `f`, `false`, `off`.
- `charp` parameters take a string of at most 255 bytes.

A value with the wrong type, a malformed value or an out-of-range value is
logged as a warning, and the parameter keeps its default. UVM validates most
parameters itself and falls back to the default for values it rejects.
`nvidia_uvm` also enforces these limits, where UVM uses a value without
checking it:

| Parameter | Accepted |
| --- | --- |
| `uvm_perf_pma_batch_nonpinned_order` | 0 to 10 |
| `uvm_perf_thrashing_lapse_usec` | 0 to 500000 |
| `uvm_perf_access_counter_migration_enable` | -1 to 1 |
| `uvm_leak_checker` | 0 to 2 |

Every parameter is listed with its type in
`nvidia-uvm/lkpi/uvm_kpi_params.h`. A new `module_param()` in an updated
`kernel-open/nvidia-uvm` does not compile until it is added there.

### Builtin tests

The UVM builtin tests are built in, as on Linux. Their ioctls fail unless
`uvm_enable_builtin_tests=1;` is set in `nvidia_uvm.conf`. While it is set,
every ioctl on `/dev/nvidia-uvm` and `/dev/nvidia-uvm-tools`, and every mmap
of `/dev/nvidia-uvm`, also requires the `sys_config` privilege, which only the global zone has, because the test
ioctls and the test flags of regular ioctls can expose kernel addresses and
change driver state. Do not enable the tests on production systems.
