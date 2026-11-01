<div align="center">

# sysfs

**GPU-only sysfs interface for MediaTek MT6789 (Helio G99).**

[![Kernel](https://img.shields.io/badge/kernel-5.10-informational?style=for-the-badge&labelColor=222)](#build)
[![Platform](https://img.shields.io/badge/platform-MT6789-informational?style=for-the-badge&labelColor=222)](#gpu-interface)
[![License](https://img.shields.io/badge/license-GPL--2.0-informational?style=for-the-badge&logo=gnu&labelColor=222)](LICENSE)

</div>

`sysfs` is an out-of-tree kernel module that exposes a GPU control and telemetry interface at `/sys/kernel/gpu/`. It is derived from the GPU SKI interface in the `gpu-ski-mt6789-clean` branch.

This branch contains GPU sysfs functionality only. It does not contain CPU overclocking, GPU OPP-table patching, voltage modification, or frequency overclocking code.

## GPU interface

After loading the module, the following files are created:

| File | Access | Description |
|---|---|---|
| `gpu_model` | Read | Reports the GPU model string. |
| `gpu_clock` | Read | Current GPU frequency in KHz. |
| `gpu_busy` | Read | GPU loading percentage. |
| `gpu_freq_table` | Read | Frequencies from the active GPU working OPP table, in KHz. |
| `gpu_governor` | Read/write | Reads or changes the active Mali power policy. |
| `gpu_available_governor` | Read | Lists Mali power policies supported by the driver. |
| `gpu_min_clock` | Read/write | Reads or sets the GPU minimum frequency limit, in KHz. |
| `gpu_max_clock` | Read/write | Reads or sets the GPU maximum frequency limit, in KHz. |

The module creates this path:

```text
/sys/kernel/gpu/
├── gpu_available_governor
├── gpu_busy
├── gpu_clock
├── gpu_freq_table
├── gpu_governor
├── gpu_max_clock
├── gpu_min_clock
└── gpu_model
```

## Usage

Load the module:

```sh
su
insmod /path/to/sysfs.ko
```

Read GPU information:

```sh
cat /sys/kernel/gpu/gpu_model
cat /sys/kernel/gpu/gpu_clock
cat /sys/kernel/gpu/gpu_busy
cat /sys/kernel/gpu/gpu_freq_table
cat /sys/kernel/gpu/gpu_governor
cat /sys/kernel/gpu/gpu_available_governor
```

Set the GPU frequency limits using exact frequencies listed in `gpu_freq_table`:

```sh
echo 500000 > /sys/kernel/gpu/gpu_min_clock
echo 955000 > /sys/kernel/gpu/gpu_max_clock
```

Change the Mali power policy to a name listed by `gpu_available_governor`:

```sh
cat /sys/kernel/gpu/gpu_available_governor
echo coarse_demand > /sys/kernel/gpu/gpu_governor
```

Remove the module:

```sh
rmmod sysfs
```

## Implementation

The module resolves the MediaTek GPU helper functions through kprobes and uses the vendor GPU driver interfaces for:

- Reading the active GPU working OPP table and current frequency.
- Reading GPU loading.
- Applying the driver's GPU minimum and maximum frequency limits.
- Reading the available Mali power policies and changing the active policy through the Mali policy API.

The module does not modify OPP entries, GPU voltage, PLL registers, CPU frequency tables, or CPU voltage.

## Runtime requirements

The target kernel and vendor modules must provide the expected MediaTek GPU and Mali symbols. The Mali platform device is expected at:

```text
13000000.mali
```

The module targets the MT6789 vendor GPU driver interface. Symbol availability and structure compatibility depend on the exact kernel and vendor-driver build.

## Build

Build against the configured target kernel build tree:

```sh
make KDIR=~/OSS/common WORKSPACE=~/OSS
```

Check the detected Clang toolchain and kernel CFI configuration:

```sh
make check-clang KDIR=~/OSS/common WORKSPACE=~/OSS
```

The output module is:

```text
sysfs.ko
```

Use the same kernel build tree and compatible Clang toolchain as the target device.

## License

GPL-2.0. See [LICENSE](LICENSE).
