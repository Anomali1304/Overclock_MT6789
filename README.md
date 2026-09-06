<div align="center">

# overclock_mt6789

**Out-of-tree GPU/CPU overclock module for MediaTek MT6789 (Helio G99), built for the Xiaomi POCO M5 (`rock`).**

[![Kernel](https://img.shields.io/badge/kernel-5.10-informational?style=for-the-badge&logoColor=white&labelColor=222)](#runtime-dependencies)
[![Platform](https://img.shields.io/badge/platform-MT6789-informational?style=for-the-badge&logoColor=white&labelColor=222)](#scope)
[![License](https://img.shields.io/badge/license-GPL--2.0-informational?style=for-the-badge&logo=gnu&logoColor=white&labelColor=222)](LICENSE)

</div>

`overclock_mt6789` patches the stock GPU OPP table and the CPU cpufreq-hw top entry at runtime. It stays inert until an explicit apply parameter is written, and every write is validated against the limits below before it touches hardware.

## Why a kernel module instead of a userspace tool?

Neither table this module touches is exposed anywhere userspace can reach. The CPU OPP table is eFuse-burned into hardware registers at boot, not read from a software partition, and the OC path bypasses the cpufreq governor entirely through an MCUPM IPI call — `scaling_available_frequencies` and `time_in_state` never reflect it. The GPU OPP table lives as a 45-to-65-entry array inside the vendor `.ko`'s `.data` section. Reaching either one means resolving vendor symbols and writing kernel memory directly, which only a kernel module can do.

## Scope

### GPU

- Patches GPU OPP index 0 in `gpufreq`'s working table.
- Patches the signed table when its symbol is available.
- Resynchronizes GED's cached working table when `g_working_table` is resolvable.
- Updates the MFG PLL through `mtk_fh_set_rate()` and the required POSDIV ordering.
- Defers the PLL transition to a workqueue triggered by the `gpufreq_commit()` return probe.

GPU frequency is limited to `1750000 KHz`. GPU voltage and VSRAM are limited to `50000..100000` in mV×100 units. `gpu_target_vsram=0` uses the target GPU voltage.

### CPU

The CPU path targets the top cpufreq entry (`idx0`) for two domains:

- Little cluster: representative CPU `0`
- Big cluster: representative CPU `6`

For each domain the module updates:

- vendor `REG_FREQ_LUT_TABLE` entry 0;
- `policy->freq_table[0].frequency`;
- `policy->cpuinfo.max_freq`;
- `policy->max`;
- Energy Model top performance-state frequency.

Before modifying a live LUT row, the module temporarily caps the policy to the next frequency entry and waits for the hardware performance state to leave idx0. The timeout is 50 ms.

CPU targets are limited to the lower of:

```text
stock_idx0 + 60%
2600000 KHz
```

That ceiling isn't an arbitrary safety margin — `2610000 KHz` and above is confirmed to hang on this hardware. A target of `0` restores the saved stock idx0 frequency.

## Parameters

All parameters are exposed under:

```text
/sys/module/overclock_mt6789/parameters/
```

### GPU

| Parameter | Mode | Meaning |
|---|---|---|
| `gpu_target_khz` | RW | Target frequency in KHz. `0` disables the requested OC target. Maximum `1750000`. |
| `gpu_target_volt` | RW | Target voltage in mV×100. Maximum `100000` (1000 mV). |
| `gpu_target_vsram` | RW | Target VSRAM in mV×100. `0` uses `gpu_target_volt`. Maximum `100000` (1000 mV). |
| `gpu_oc_apply` | RW | Write `1` to apply the GPU target values. |
| `gpu_oc_result` | RO | Result of the last GPU apply operation. |
| `gpu_opp_table` | RO | Live GPU OPP table, recomputed on every read. |

Example:

```bash
P=/sys/module/overclock_mt6789/parameters

echo 1000000 > $P/gpu_target_khz
echo 90000   > $P/gpu_target_volt
echo 0       > $P/gpu_target_vsram
echo 1       > $P/gpu_oc_apply
cat $P/gpu_oc_result
cat $P/gpu_opp_table
```

Restore the original GPU OPP entry:

```bash
P=/sys/module/overclock_mt6789/parameters

echo 0 > $P/gpu_target_khz
echo 1 > $P/gpu_oc_apply
```

### CPU

| Parameter | Mode | Meaning |
|---|---|---|
| `cpu_ll_target_khz` | RW | Little-cluster idx0 target. `0` restores stock. |
| `cpu_b_target_khz` | RW | Big-cluster idx0 target. `0` restores stock. |
| `cpu_oc_apply` | RW | Write `1` to apply both CPU targets. |
| `cpu_oc_result` | RO | Result of the last CPU apply operation. |
| `cpu_ll_rep_cpu` | RO | Little-cluster representative CPU. Default `0`. |
| `cpu_b_rep_cpu` | RO | Big-cluster representative CPU. Default `6`. |
| `cpu_ll_min_khz` | RW | Sets `policy->min` directly for the little cluster. |
| `cpu_b_min_khz` | RW | Sets `policy->min` directly for the big cluster. |
| `cpu_lut_table` | RO | Compares the RAM cpufreq table with the vendor MMIO LUT. |
| `cpu_stats_refresh` | RW | Write `1` for little or `2` for big to rebuild `time_in_state`. |

CPU target example:

```bash
P=/sys/module/overclock_mt6789/parameters

echo 2100000 > $P/cpu_ll_target_khz
echo 2500000 > $P/cpu_b_target_khz
echo 1       > $P/cpu_oc_apply
cat $P/cpu_oc_result
cat $P/cpu_lut_table
```

Restore stock CPU top entries:

```bash
P=/sys/module/overclock_mt6789/parameters

echo 0 > $P/cpu_ll_target_khz
echo 0 > $P/cpu_b_target_khz
echo 1 > $P/cpu_oc_apply
```

## CPU minimum frequency

`cpu_ll_min_khz` and `cpu_b_min_khz` write directly to the corresponding `struct cpufreq_policy::min` value. This is separate from the userspace `scaling_min_freq` file and is intended for kernels where that sysfs node is not writable.

The requested value is clamped to:

```text
cpuinfo.min_freq <= policy->min <= policy->max
```

The parameter rejects `0`. Read the same parameter to obtain the current policy minimum.

Example:

```bash
P=/sys/module/overclock_mt6789/parameters

echo 2000000 > $P/cpu_ll_min_khz
echo 2000000 > $P/cpu_b_min_khz
cat $P/cpu_ll_min_khz
cat $P/cpu_b_min_khz
```

When a CPU OC target is applied, the module also updates `policy->min` when the previous minimum is above the new top frequency or was tracking the previous `policy->max`.

## `time_in_state`

The CPU OC path changes the frequency table after `cpufreq_stats` may already have created its bucket table. `cpu_stats_refresh` explicitly rebuilds that table.

```bash
P=/sys/module/overclock_mt6789/parameters

echo 2 > $P/cpu_stats_refresh
cat $P/cpu_stats_refresh
cat /sys/devices/system/cpu/cpu6/cpufreq/stats/time_in_state

echo 1 > $P/cpu_stats_refresh
cat /sys/module/overclock_mt6789/parameters/cpu_stats_refresh
```

Run one domain at a time. The big cluster is the lower-risk first check because the little-cluster representative is CPU 0.

## Warning ⚠️

The module writes live kernel data structures and hardware registers directly, and refuses apply operations while the device is entering suspend or hibernation.

### CFI on this module

This kernel enforces `CONFIG_CFI_CLANG`. Every function reached through a `kallsyms_lookup_name()`-resolved pointer needs `__nocfi`, and the module must build with `-fsanitize=kcfi` in `EXTRA_CFLAGS` using the exact same Clang as the target kernel. Get either wrong and the device reboots instantly with no panic log — there's nothing to read afterward to tell you why.

### What the module already checks

The CPU path validates the target against the stock-frequency-derived limit and the absolute 2600 MHz ceiling, and verifies that the cluster leaves idx0 before modifying the live LUT row. The GPU PLL path uses the MediaTek frequency-hopping function for the PCW transition and never writes the PLL target synchronously from the apply parameter handler.

None of that makes a wrong target, an incompatible kernel tree, or a vendor-driver change safe to try blind. Test changes incrementally and keep a known-good kernel available.

## Runtime dependencies

The target kernel must provide the vendor symbols and structures this module expects. `CONFIG_KALLSYMS_ALL=y` is required for the kallsyms-resolved paths used by GED synchronization, PLL control, and CPU statistics refresh.

The module must be compiled with the same Clang toolchain as the kernel — especially important when `CONFIG_CFI_CLANG=y` is enabled.

## Known limitations

- CPU voltage is not controlled by this module. Above-stock CPU frequency depends on the platform's existing voltage-management behavior.
- GED synchronization is best-effort. If `g_working_table` cannot be resolved, the hardware OPP change can still be applied while GED's cached frequency may remain unchanged.
- Signed GPU table patching is optional and depends on `gpufreq_get_signed_table` being available.
- The vendor `cpufreq_mtk_mirror` layout is platform-specific. A vendor kernel change can invalidate the offsets used by this module.
- `cpu_stats_refresh` is manual and should be used only after confirming the normal OC path is stable.

## Build

```bash
make KDIR=~/OSS/common WORKSPACE=~/OSS
make check-clang
```

`KDIR` must point at a completed kernel build tree for the target device — the module needs the vendor symbols and headers from that exact build, not a generic MT6789 tree.

Install/remove for development:

```bash
make install
make uninstall
```

## License

GPL-2.0. See [`LICENSE`](LICENSE).
