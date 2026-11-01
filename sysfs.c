// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/platform_device.h>
#include <linux/device.h>

#define GPU_OPP_MAX 72

enum gpufreq_posdiv {
	POSDIV_POWER_1 = 0,
	POSDIV_POWER_2 = 1,
	POSDIV_POWER_4 = 2,
	POSDIV_POWER_8 = 3,
	POSDIV_POWER_16 = 4,
};

enum gpufreq_target {
	TARGET_STACK = 0,
	TARGET_GPU = 1,
	TARGET_DEFAULT = 2,
};

struct gpufreq_opp_info {
	unsigned int freq;
	unsigned int volt;
	unsigned int vsram;
	enum gpufreq_posdiv posdiv;
	unsigned int vaging;
	unsigned int power;
};

typedef const struct gpufreq_opp_info *(*fn_get_wt_t)(enum gpufreq_target);
typedef int (*fn_get_opp_num_t)(enum gpufreq_target);

static fn_get_wt_t sym_gpufreq_get_working_table;
static fn_get_opp_num_t sym_gpufreq_get_opp_num;

static __nocfi int sysfs_kprobe_nop(struct kprobe *kp, struct pt_regs *regs)
{
	return 0;
}

static unsigned long resolve_ksym(const char *name)
{
	struct kprobe kp = {
		.symbol_name = name,
		.pre_handler = sysfs_kprobe_nop,
	};
	unsigned long addr;
	int ret = register_kprobe(&kp);

	if (ret < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

/* -------------------------------------------------------------------------
 * GPU SKI-compatible sysfs interface.
 *
 * GPU telemetry and frequency limits use MTK gpufreq/GED helper symbols.
 * Governor policy remains controlled by Mali's native power_policy interface.
 * ------------------------------------------------------------------------- */

typedef unsigned int (*fn_ski_cur_t)(enum gpufreq_target);
typedef bool (*fn_ski_loading_t)(unsigned int *);
typedef bool (*fn_ski_limit_t)(unsigned int);
typedef bool (*fn_ski_bottom_t)(unsigned long *);
typedef bool (*fn_ski_ceiling_t)(int *);

static fn_ski_cur_t ski_cur_freq;
static fn_ski_loading_t ski_get_loading;
static fn_ski_limit_t ski_custom_boost;
static fn_ski_limit_t ski_custom_upbound;
static fn_ski_bottom_t ski_get_bottom;
static fn_ski_ceiling_t ski_get_ceiling;
static struct kobject *ski_kobj;
static unsigned int ski_custom_max_khz;
static unsigned int ski_custom_min_khz;

static __nocfi bool ski_resolve_helpers(void)
{
	unsigned long addr;

	if (!sym_gpufreq_get_working_table) {
		addr = resolve_ksym("gpufreq_get_working_table");
		if (addr)
			sym_gpufreq_get_working_table = (fn_get_wt_t)addr;
	}
	if (!sym_gpufreq_get_opp_num) {
		addr = resolve_ksym("gpufreq_get_opp_num");
		if (addr)
			sym_gpufreq_get_opp_num = (fn_get_opp_num_t)addr;
	}
	if (!ski_cur_freq) {
		addr = resolve_ksym("gpufreq_get_cur_freq");
		if (addr)
			ski_cur_freq = (fn_ski_cur_t)addr;
	}
	if (!ski_get_loading) {
		addr = resolve_ksym("mtk_get_gpu_loading");
		if (addr)
			ski_get_loading = (fn_ski_loading_t)addr;
	}
	if (!ski_custom_boost) {
		addr = resolve_ksym("mtk_custom_boost_gpu_freq");
		if (addr)
			ski_custom_boost = (fn_ski_limit_t)addr;
	}
	if (!ski_custom_upbound) {
		addr = resolve_ksym("mtk_custom_upbound_gpu_freq");
		if (addr)
			ski_custom_upbound = (fn_ski_limit_t)addr;
	}
	if (!ski_get_bottom) {
		addr = resolve_ksym("mtk_get_gpu_bottom_freq");
		if (addr)
			ski_get_bottom = (fn_ski_bottom_t)addr;
	}
	if (!ski_get_ceiling) {
		addr = resolve_ksym("mtk_get_gpu_ceiling_index");
		if (addr)
			ski_get_ceiling = (fn_ski_ceiling_t)addr;
	}

	return sym_gpufreq_get_working_table && sym_gpufreq_get_opp_num &&
	       ski_cur_freq && ski_get_loading &&
	       ski_custom_boost && ski_custom_upbound && ski_get_bottom &&
	       ski_get_ceiling;
}

static __nocfi const struct gpufreq_opp_info *ski_gpu_table(int *count)
{
	const struct gpufreq_opp_info *table;

	if (!ski_resolve_helpers())
		return NULL;
	*count = sym_gpufreq_get_opp_num(TARGET_GPU);
	if (*count <= 0 || *count > GPU_OPP_MAX)
		return NULL;
	table = sym_gpufreq_get_working_table(TARGET_GPU);
	return table;
}

static __nocfi int ski_freq_to_idx(unsigned int khz)
{
	const struct gpufreq_opp_info *table;
	int count = 0, i;

	table = ski_gpu_table(&count);
	if (!table)
		return -EAGAIN;
	for (i = 0; i < count; i++)
		if (table[i].freq == khz)
			return i;
	return -EINVAL;
}

/*
 * Match the Mali R32P1 policy API in android_kernel_xiaomi_rock:
 * kbase_pm_get_policy() returns a policy whose first field is its name;
 * kbase_pm_list_policies() returns the policies accepted by power_policy.
 * dev_get_drvdata() on the Mali platform device returns struct kbase_device.
 */
struct ski_mali_pm_policy {
	char *name;
};

typedef const struct ski_mali_pm_policy *(*fn_ski_mali_get_policy_t)(void *);
typedef int (*fn_ski_mali_list_policies_t)(
	void *, const struct ski_mali_pm_policy * const **);
typedef void (*fn_ski_mali_set_policy_t)(
	void *, const struct ski_mali_pm_policy *);

static fn_ski_mali_get_policy_t ski_mali_get_policy;
static fn_ski_mali_list_policies_t ski_mali_list_policies;
static fn_ski_mali_set_policy_t ski_mali_set_policy;

static __nocfi struct device *ski_mali_get_device(void)
{
	return bus_find_device_by_name(&platform_bus_type, NULL, "13000000.mali");
}

static __nocfi int ski_mali_resolve_policy_api(void)
{
	unsigned long addr;

	if (!ski_mali_get_policy) {
		addr = resolve_ksym("kbase_pm_get_policy");
		if (addr)
			ski_mali_get_policy = (fn_ski_mali_get_policy_t)addr;
	}
	if (!ski_mali_list_policies) {
		addr = resolve_ksym("kbase_pm_list_policies");
		if (addr)
			ski_mali_list_policies = (fn_ski_mali_list_policies_t)addr;
	}
	if (!ski_mali_set_policy) {
		addr = resolve_ksym("kbase_pm_set_policy");
		if (addr)
			ski_mali_set_policy = (fn_ski_mali_set_policy_t)addr;
	}
	return (ski_mali_get_policy && ski_mali_list_policies &&
		ski_mali_set_policy) ? 0 : -ENODEV;
}

static __nocfi int ski_mali_active_policy(char *name, size_t name_size)
{
	struct device *dev;
	const struct ski_mali_pm_policy *policy;
	void *kbdev;

	if (ski_mali_resolve_policy_api())
		return -ENODEV;

	dev = ski_mali_get_device();
	if (!dev)
		return -ENODEV;

	kbdev = dev_get_drvdata(dev);
	if (!kbdev) {
		put_device(dev);
		return -ENODEV;
	}

	policy = ski_mali_get_policy(kbdev);
	if (!policy || !policy->name) {
		put_device(dev);
		return -ENODATA;
	}

	strscpy(name, policy->name, name_size);
	put_device(dev);
	return 0;
}

static __nocfi ssize_t gpu_available_governor_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct device *dev;
	const struct ski_mali_pm_policy * const *policies = NULL;
	void *kbdev;
	int count, i;
	ssize_t len = 0;

	if (ski_mali_resolve_policy_api())
		return -EAGAIN;

	dev = ski_mali_get_device();
	if (!dev)
		return -ENODEV;
	kbdev = dev_get_drvdata(dev);
	if (!kbdev) {
		put_device(dev);
		return -ENODEV;
	}

	count = ski_mali_list_policies(kbdev, &policies);
	if (count <= 0 || !policies) {
		put_device(dev);
		return -ENODATA;
	}

	for (i = 0; i < count && len < PAGE_SIZE - 2; i++) {
		if (!policies[i] || !policies[i]->name)
			continue;
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				 i ? " " : "", policies[i]->name);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	put_device(dev);
	return len;
}

static __nocfi ssize_t gpu_governor_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	char name[64];
	int ret;

	ret = ski_mali_active_policy(name, sizeof(name));
	if (ret)
		return -EAGAIN;
	return scnprintf(buf, PAGE_SIZE, "%s\n", name);
}

static __nocfi ssize_t gpu_governor_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	const struct ski_mali_pm_policy * const *policies = NULL;
	struct device *dev;
	void *kbdev;
	int n, i;
	ssize_t ret = -EINVAL;

	if (ski_mali_resolve_policy_api())
		return -EAGAIN;

	dev = ski_mali_get_device();
	if (!dev)
		return -ENODEV;
	kbdev = dev_get_drvdata(dev);
	if (!kbdev) {
		put_device(dev);
		return -ENODEV;
	}

	n = ski_mali_list_policies(kbdev, &policies);
	if (n <= 0 || !policies) {
		put_device(dev);
		return -ENODATA;
	}

	for (i = 0; i < n; i++) {
		if (!policies[i] || !policies[i]->name)
			continue;
		if (sysfs_streq(policies[i]->name, buf)) {
			/* Use Mali's own policy transition routine, not a shadow value. */
			ski_mali_set_policy(kbdev, policies[i]);
			ret = count;
			break;
		}
	}

	put_device(dev);
	return ret;
}

static __nocfi ssize_t gpu_model_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "Mali-G57 MC2\n");
}

static __nocfi ssize_t gpu_busy_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	unsigned int loading = 0;

	if (!ski_resolve_helpers())
		return -EAGAIN;
	ski_get_loading(&loading);
	return scnprintf(buf, PAGE_SIZE, "%u %%\n", loading);
}

static __nocfi ssize_t gpu_clock_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	unsigned int khz;

	if (!ski_resolve_helpers())
		return -EAGAIN;
	/* Frequency is independently readable; voltage may be zero while the GPU is idle. */
	khz = ski_cur_freq(TARGET_GPU);
	return scnprintf(buf, PAGE_SIZE, "%u\n", khz);
}

static __nocfi ssize_t gpu_freq_table_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	const struct gpufreq_opp_info *table;
	ssize_t len = 0;
	int count = 0, i;

	table = ski_gpu_table(&count);
	if (!table)
		return -EAGAIN;
	for (i = 0; i < count && len < PAGE_SIZE - 16; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%u ",
				 table[i].freq);
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static __nocfi ssize_t gpu_max_clock_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	const struct gpufreq_opp_info *table;
	unsigned int khz;
	int count = 0, idx = 0;

	table = ski_gpu_table(&count);
	if (!table)
		return -EAGAIN;
	if (!ski_get_ceiling(&idx) || idx < 0 || idx >= count)
		idx = 0;
	khz = table[idx].freq;
	if (ski_custom_max_khz && ski_custom_max_khz < khz)
		khz = ski_custom_max_khz;
	return scnprintf(buf, PAGE_SIZE, "%u\n", khz);
}

static __nocfi ssize_t gpu_max_clock_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int khz;
	int idx;

	if (kstrtouint(buf, 0, &khz) || !khz)
		return -EINVAL;
	if (!ski_resolve_helpers())
		return -EAGAIN;
	idx = ski_freq_to_idx(khz);
	if (idx < 0)
		return idx;
	if (!ski_custom_upbound(idx))
		return -EIO;
	ski_custom_max_khz = idx ? khz : 0;
	return count;
}

static __nocfi ssize_t gpu_min_clock_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	unsigned long khz = 0;

	if (!ski_resolve_helpers())
		return -EAGAIN;
	if (!ski_get_bottom(&khz))
		return -EIO;
	if (ski_custom_min_khz > khz)
		khz = ski_custom_min_khz;
	return scnprintf(buf, PAGE_SIZE, "%lu\n", khz);
}

static __nocfi ssize_t gpu_min_clock_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int khz;
	int idx, n;

	if (kstrtouint(buf, 0, &khz) || !khz)
		return -EINVAL;
	if (!ski_resolve_helpers())
		return -EAGAIN;
	idx = ski_freq_to_idx(khz);
	if (idx < 0)
		return idx;
	if (!ski_custom_boost(idx))
		return -EIO;
	n = sym_gpufreq_get_opp_num(TARGET_GPU);
	ski_custom_min_khz = (idx == n - 1) ? 0 : khz;
	return count;
}

static struct kobj_attribute ski_gpu_attrs[] = {
	__ATTR_RO(gpu_available_governor),

	__ATTR_RO(gpu_busy),
	__ATTR_RO(gpu_clock),
	__ATTR_RO(gpu_freq_table),
	__ATTR(gpu_governor, 0644, gpu_governor_show, gpu_governor_store),
	__ATTR(gpu_max_clock, 0644, gpu_max_clock_show, gpu_max_clock_store),
	__ATTR(gpu_min_clock, 0644, gpu_min_clock_show, gpu_min_clock_store),
	__ATTR_RO(gpu_model),
};

static struct attribute *ski_gpu_attr_ptrs[ARRAY_SIZE(ski_gpu_attrs) + 1];

static const struct attribute_group ski_gpu_attr_group = {
	.attrs = ski_gpu_attr_ptrs,
};

static int sysfs_gpu_init(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(ski_gpu_attrs); i++)
		ski_gpu_attr_ptrs[i] = &ski_gpu_attrs[i].attr;

	ski_kobj = kobject_create_and_add("gpu", kernel_kobj);
	if (!ski_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(ski_kobj, &ski_gpu_attr_group);
	if (ret) {
		kobject_put(ski_kobj);
		ski_kobj = NULL;
	}
	return ret;
}

static void sysfs_gpu_exit(void)
{
	if (ski_kobj) {
		sysfs_remove_group(ski_kobj, &ski_gpu_attr_group);
		kobject_put(ski_kobj);
		ski_kobj = NULL;
	}
}


static int __init sysfs_gpu_module_init(void)
{
	int ret = sysfs_gpu_init();

	if (ret) {
		pr_err("sysfs_gpu: failed to create /sys/kernel/gpu (%d)\\n", ret);
		return ret;
	}
	pr_info("sysfs_gpu: GPU sysfs interface ready at /sys/kernel/gpu\\n");
	return 0;
}

static void __exit sysfs_gpu_module_exit(void)
{
	sysfs_gpu_exit();
	pr_info("sysfs_gpu: unloaded\\n");
}

module_init(sysfs_gpu_module_init);
module_exit(sysfs_gpu_module_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Anomali1304");
MODULE_DESCRIPTION("MediaTek GPU SKI-compatible sysfs interface");
MODULE_VERSION("1.0.0");
