# NVIDIA kernel modules for illumos

This directory holds the illumos kernel interface layer. It is linked with the
OS-agnostic `nv-kernel.o` and `nv-modeset-kernel.o` objects built from `src/`.
It builds two drivers:

| Module | Device nodes |
| --- | --- |
| `nvidia` | `/dev/nvidiactl`, `/dev/nvidiaN`, `/dev/nvidia-caps/*`, `/dev/nvidia-caps-imex-channels/*`, `/dev/nvidia-nvlink`, `/dev/nvidia-nvswitchctl`, `/dev/nvidia-nvswitchN` |
| `nvidia_modeset` | `/dev/nvidia-modeset` |

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
