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

The CPU path discovers all available cpufreq policies at module load and uses the first CPU in each policy's `related_cpus` mask as its representative. Policies are kept in ascending representative-CPU order. The first three are exposed as Little, Big, and Prime aliases; the dynamic path supports up to `MAX_CPU_POLICIES` policies.

For each domain the module updates:

- vendor `REG_FREQ_LUT_TABLE` entry 0;
- `policy->freq_table[0].frequency`;
- `policy->cpuinfo.max_freq`;
- `policy->max`;
- Energy Model top performance-state frequency.

Before modifying a live LUT row, the module temporarily caps the policy to the next frequency entry and waits for the hardware performance state to leave idx0. The timeout is 50 ms.

CPU targets are limited to the lower of:

```text
stock_idx0 + 100%
3000000 KHz
```

Targets above `2600000 KHz` are refused by default. Set `cpu_oc_allow_3ghz=1` to opt in to the experimental `2601000..3000000 KHz` range. This is deliberately disabled by default: repository testing records hangs at `2610000 KHz` and above on this hardware, so the 3GHz ceiling is a software limit, not a stability or safety claim. A target of `0` restores the saved stock idx0 frequency.

When `cpu_volt_follow=1` (default), the module derives a candidate LUT voltage from the policy's existing OPP slope and rejects missing/invalid slope data or requests beyond the configured voltage limits. This edits the cpufreq-hw LUT; it does not prove that the physical PMIC rail reached the requested voltage.

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
| Parameter | Mode | Meaning |
|---|---|---|
| `cpu_little_target_khz` | RW | Little policy idx0 target. `0` restores stock. |
| `cpu_big_target_khz` | RW | Big policy idx0 target. `0` restores stock. |
| `cpu_prime_target_khz` | RW | Third policy idx0 target; available only when a third policy is detected. |
| `cpu_policy_targets_khz` | RW | Comma-separated target KHz for every detected policy, ordered by representative CPU. Use one value per policy; `0` restores that policy. |
| `cpu_oc_apply` | RW | Write `1` to apply CPU targets. |
| `cpu_oc_result` | RO | Result of the last CPU apply operation. |
| `cpu_little_rep_cpu` | RO | Auto-detected representative CPU for the first policy. |
| `cpu_big_rep_cpu` | RO | Auto-detected representative CPU for the second policy. |
| `cpu_prime_rep_cpu` | RO | Auto-detected representative CPU for the third policy, or `UINT_MAX` if absent. |
| `cpu_prime_status` | RO | `active`, `inactive`, or `error`. |
| `cpu_oc_allow_3ghz` | RW | Set `1` to permit experimental targets above 2.6GHz up to 3GHz; disabled by default due to known hangs. |
| `cpu_volt_follow` | RW | `1` derives candidate idx0 LUT voltage from the existing OPP slope; default `1`. |
| `cpu_volt_max_delta_raw` | RW | Maximum voltage delta from stock, raw mV×100; default `20000`. |
| `cpu_volt_abs_max_raw` | RW | Absolute candidate LUT voltage ceiling; hard-clamped to `115000`. |
| `cpu_little_volt_raw` / `cpu_big_volt_raw` / `cpu_prime_volt_raw` | RW | Optional explicit candidate LUT voltage for each alias; `0` uses slope derivation. |
| `cpu_volt_reassert` | RW | Reassert the candidate LUT voltage if EEM/SVS overwrites it. |
| `cpu_little_min_khz` / `cpu_big_min_khz` / `cpu_prime_min_khz` | RW | Set the corresponding policy minimum frequency. |
| `cpu_lut_table` | RO | Compare RAM cpufreq table with the vendor MMIO LUT. |
| `cpu_stats_refresh` | RW | Write `1` for first policy or `2` for second policy to rebuild `time_in_state`. |

CPU target example:

```bash
P=/sys/module/overclock_mt6789/parameters

echo 2100000 > $P/cpu_little_target_khz
echo 2500000 > $P/cpu_big_target_khz
echo 1       > $P/cpu_oc_apply
cat $P/cpu_oc_result
cat $P/cpu_lut_table
```

Restore stock CPU top entries:

```bash
P=/sys/module/overclock_mt6789/parameters

echo 0 > $P/cpu_little_target_khz
echo 0 > $P/cpu_big_target_khz
echo 1 > $P/cpu_oc_apply
```

## CPU minimum frequency

`cpu_little_min_khz`, `cpu_big_min_khz`, and `cpu_prime_min_khz` write directly to the corresponding `struct cpufreq_policy::min` value. This is separate from the userspace `scaling_min_freq` file and is intended for kernels where that sysfs node is not writable.

The requested value is clamped to:

```text
cpuinfo.min_freq <= policy->min <= policy->max
```

The parameter rejects `0`. Read the same parameter to obtain the current policy minimum.

Example:

```bash
P=/sys/module/overclock_mt6789/parameters

echo 2000000 > $P/cpu_little_min_khz
echo 2000000 > $P/cpu_big_min_khz
cat $P/cpu_little_min_khz
cat $P/cpu_big_min_khz
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

The CPU path validates targets against the stock-frequency-derived limit and the 3000 MHz absolute ceiling. Targets above 2600 MHz require the explicit `cpu_oc_allow_3ghz=1` opt-in. It verifies that the cluster leaves idx0 and checks MMIO readback before accepting a LUT update. The GPU PLL path uses the MediaTek frequency-hopping function for the PCW transition and never writes the PLL target synchronously from the apply parameter handler.

None of that makes a wrong target, an incompatible kernel tree, or a vendor-driver change safe to try blind. Test changes incrementally and keep a known-good kernel available.

## Runtime dependencies

The target kernel must provide the vendor symbols and structures this module expects. `CONFIG_KALLSYMS_ALL=y` is required for the kallsyms-resolved paths used by GED synchronization, PLL control, and CPU statistics refresh.

The module must be compiled with the same Clang toolchain as the kernel — especially important when `CONFIG_CFI_CLANG=y` is enabled.

## Known limitations

## Experimental CPU LUT voltage follow

The CPU voltage-follow path performs an **experimental candidate voltage-field write** to the CPU cpufreq-hw LUT. This is not a direct PMIC API and does not guarantee that the physical CPU rail follows the requested value.

Parameters are under `/sys/module/overclock_mt6789/parameters/`:

| Parameter | Meaning |
| --- | --- |
| `cpu_volt_follow` | Default `1`: derive a candidate idx0 LUT voltage when the target exceeds stock. Set `0` for frequency-only unless an explicit voltage is configured. |
| `cpu_little_volt_raw` | Optional explicit Little-policy idx0 voltage; `0` uses the slope derived from LUT rows 1 and 3. |
| `cpu_big_volt_raw` | Optional explicit Big-policy idx0 voltage; `0` uses the slope derived from LUT rows 1 and 3. |
| `cpu_prime_volt_raw` | Optional explicit third-policy idx0 voltage; available only when a third policy is detected. |
| `cpu_volt_max_delta_raw` | Maximum increase over the captured stock idx0 value; default `20000` (= 200 mV in the project's raw-unit convention). |
| `cpu_volt_abs_max_raw` | Final candidate ceiling; default `112000` (= 1.12 V), hard-clamped at `115000` (= 1.15 V). |
| `cpu_volt_reassert` | Default `1`: guardian checks the idx0 LUT voltage every 50 ms and rewrites only that field if it changes. |
| `cpu_lut_table` | Read-only dump now includes the LUT voltage field in raw units. |

Example (syntax only, **not a validated safe setting**):

```sh
P=/sys/module/overclock_mt6789/parameters
cat "$P/cpu_lut_table"
echo 20000 > "$P/cpu_volt_max_delta_raw"
echo 112000 > "$P/cpu_volt_abs_max_raw"
echo 0 > "$P/cpu_little_volt_raw"   # first policy: auto slope
echo 0 > "$P/cpu_big_volt_raw"      # second policy: auto slope
echo 2400000 > "$P/cpu_little_target_khz"
echo 2500000 > "$P/cpu_big_target_khz"
echo 1 > "$P/cpu_oc_apply"
cat "$P/cpu_oc_result"
cat "$P/cpu_lut_table"
```

Voltage raw values use the source project's `mV*100` convention, rounded upward to a 625-raw step. Explicit values cannot request less than the captured stock idx0 voltage. When voltage follow is enabled, the module rejects missing/invalid slope data and requests that exceed either voltage limit before writing the LUT. Setting `cpu_volt_follow=0` explicitly selects frequency-only behavior unless an explicit voltage override is supplied.

**Hardware caveats:** `LUT_VOLT = bits[28:12]` is inferred from MT6789 LUT/EEM correlation, not an official register contract. EEM/SVS may rewrite this field, which is why reassertion is optional. Reasserting a LUT field is not proof that the PMIC/MCUPM rail changed. The supplied reference project's on-device observations reported A55 rail response but no corresponding A76 rail response; treat A76 as frequency-only unless actual rail telemetry on this exact device proves otherwise. Do not treat the example frequencies or voltages as stable recommendations. Verify stock LUT values, live readback, EEM reports, rail telemetry and stress stability on-device; keep a known-good boot image.

- CPU voltage follow is experimental and writes a candidate voltage field in the cpufreq-hw LUT; it is not a direct PMIC command and may not change the physical rail. See the section above for limits and device-validation requirements.
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
