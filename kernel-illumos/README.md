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
(HMM and ATS), tools event queues, the builtin tests, and module parameters,
which keep their default values. Without an IOMMU, a registered GPU can reach
all of physical memory, as on Linux. Only 64-bit processes can map
`/dev/nvidia-uvm`.
