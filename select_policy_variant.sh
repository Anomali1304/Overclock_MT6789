#!/system/bin/sh
# Select the module build before loading it so module_param visibility matches
# the device's cpufreq policy topology. Invoke instead of the old insmod line.
MODDIR=${0%/*}
POLICY_ROOT=/sys/devices/system/cpu/cpufreq
count=0
for policy in "$POLICY_ROOT"/policy[0-9]*; do
    [ -d "$policy" ] || continue
    count=$((count + 1))
done

case "$count" in
    2) module="$MODDIR/overclock_mt6789_2p.ko" ;;
    3) module="$MODDIR/overclock_mt6789_3p.ko" ;;
    *)
        echo "overclock_mt6789: expected 2 or 3 cpufreq policies, found $count" >&2
        exit 1
        ;;
esac

if [ ! -r "$module" ]; then
    echo "overclock_mt6789: missing selected module: $module" >&2
    exit 1
fi

# Do not silently load a second instance or ignore an existing module.
if [ -d /sys/module/overclock_mt6789 ]; then
    echo "overclock_mt6789: module already loaded" >&2
    exit 1
fi

exec /system/bin/insmod "$module"
