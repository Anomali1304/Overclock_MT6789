// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/suspend.h>
#include <linux/string.h>
#include <linux/cpufreq.h>
#include <linux/bitfield.h>
#include <linux/pm_qos.h>
#include <linux/kthread.h>
#include <linux/energy_model.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

static atomic_t oc_mt6789_suspended = ATOMIC_INIT(0);

static int oc_mt6789_pm_notifier(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		atomic_set(&oc_mt6789_suspended, 1);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		atomic_set(&oc_mt6789_suspended, 0);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block oc_mt6789_pm_nb = {
	.notifier_call = oc_mt6789_pm_notifier,
};

static __nocfi int __kprobe_nop(struct kprobe *kp, struct pt_regs *regs)
{
	return 0;
}

static unsigned long resolve_ksym(const char *name)
{
	struct kprobe kp = {
		.symbol_name = name,
		.pre_handler = __kprobe_nop,
	};
	unsigned long addr;
	int ret = register_kprobe(&kp);

	if (ret < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

enum gpufreq_posdiv {
	POSDIV_POWER_1  = 0,
	POSDIV_POWER_2  = 1,
	POSDIV_POWER_4  = 2,
	POSDIV_POWER_8  = 3,
	POSDIV_POWER_16 = 4,
};
enum gpufreq_target { TARGET_STACK = 0, TARGET_GPU = 1, TARGET_DEFAULT = 2 };

struct gpufreq_opp_info {
	unsigned int        freq;
	unsigned int        volt;
	unsigned int        vsram;
	enum gpufreq_posdiv posdiv;
	unsigned int        vaging;
	unsigned int        power;
};

typedef const struct gpufreq_opp_info *(*fn_get_wt_t)(enum gpufreq_target);
typedef int (*fn_get_opp_num_t)(enum gpufreq_target);

static fn_get_wt_t      sym_gpufreq_get_working_table;
static fn_get_opp_num_t sym_gpufreq_get_opp_num;

static fn_get_wt_t      sym_gpufreq_get_signed_table;

static struct gpufreq_opp_info **sym_ged_g_working_table;

typedef unsigned long (*fn_kallsyms_lookup_name_t)(const char *name);
static fn_kallsyms_lookup_name_t sym_kallsyms_lookup_name;

typedef bool (*fn_mtk_fh_set_rate_t)(const char *name, unsigned long dds, int postdiv);
static fn_mtk_fh_set_rate_t sym_mtk_fh_set_rate;

typedef void (*fn_stats_free_t)(struct cpufreq_policy *policy);
typedef void (*fn_stats_create_t)(struct cpufreq_policy *policy);
static fn_stats_free_t   sym_cpufreq_stats_free_table;
static fn_stats_create_t sym_cpufreq_stats_create_table;

#define MFG_PLL_NAME "mfgpll"

#define APMIXED_PHYS     0x1000C000UL
#define APMIXED_SIZE     0x1000UL
#define MFGPLL_CON1_OFF  0x26C

static void __iomem *g_apmixed_va;
#define MFGPLL_CON1  (g_apmixed_va + MFGPLL_CON1_OFF)

#define PLL_FIN        26U
#define DDS_SHIFT      14U
#define TO_MHZ_HEAD    100U
#define TO_MHZ_TAIL    10U
#define ROUNDING_VALUE 5U
#define POSDIV_SHIFT   24U

#define POSDIV_2_MAX_FREQ  1750000U
#define POSDIV_4_MIN_FREQ  375000U
#define VGPU_MAX_VOLT      100000U
#define VGPU_MIN_VOLT      50000U

#define GPU_OPP_MAX 72

static unsigned int gpu_target_khz  = 0;
static unsigned int gpu_target_volt  = 0;
static unsigned int gpu_target_vsram = 0;
static int          gpu_oc_apply     = 0;
static char gpu_oc_result[128] = "not applied";
static char gpu_opp_table[4096] = "";

module_param(gpu_target_khz,  uint, 0644);
MODULE_PARM_DESC(gpu_target_khz,  "GPU target freq KHz (0=off, max 1750000)");
module_param(gpu_target_volt,  uint, 0644);
MODULE_PARM_DESC(gpu_target_volt,  "GPU target volt mV*100 (e.g. 90000=900mV)");
module_param(gpu_target_vsram, uint, 0644);
MODULE_PARM_DESC(gpu_target_vsram, "GPU vsram mV*100 (0=same as volt)");
module_param_string(gpu_oc_result, gpu_oc_result, sizeof(gpu_oc_result), 0444);
MODULE_PARM_DESC(gpu_oc_result, "GPU OC status (read-only)");

static DEFINE_MUTEX(oc_lock);

static bool                     g_gpu_patched[GPU_OPP_MAX];
static struct gpufreq_opp_info  g_gpu_orig[GPU_OPP_MAX];
static atomic_t                 g_last_commit_idx = ATOMIC_INIT(-1);
static unsigned int             g_last_pll_freq;
static bool                     g_gpu_oc_active;
static bool                     g_hw_mapped;

static struct workqueue_struct *g_oc_wq;
static struct delayed_work      g_gpu_pll_work;

static unsigned int calc_pcw(unsigned int freq_khz, enum gpufreq_posdiv posdiv)
{
	return (((freq_khz / TO_MHZ_HEAD * (1u << (unsigned)posdiv)) << DDS_SHIFT)
		/ PLL_FIN + ROUNDING_VALUE) / TO_MHZ_TAIL;
}

static enum gpufreq_posdiv freq_to_posdiv_gpu(unsigned int freq_khz)
{
	if (freq_khz > 950000)  return POSDIV_POWER_2;
	if (freq_khz > 475000)  return POSDIV_POWER_4;
	if (freq_khz > 237500)  return POSDIV_POWER_8;
	return POSDIV_POWER_16;
}

static __nocfi bool write_pll_con1_safe(void __iomem *reg, const char *pll_name,
				 unsigned int freq_old_khz, unsigned int freq_new_khz)
{
	enum gpufreq_posdiv target_posdiv;
	unsigned int pcw, pll;

	if (!reg || !sym_mtk_fh_set_rate)
		return false;

	target_posdiv = freq_to_posdiv_gpu(freq_new_khz);
	pcw = calc_pcw(freq_new_khz, target_posdiv);
	if (!pcw)
		return false;

	if (freq_new_khz > freq_old_khz) {

		if (!sym_mtk_fh_set_rate(pll_name, pcw, target_posdiv))
			return false;
		pll = (readl(reg) & 0xF8FFFFFFu) | ((unsigned)target_posdiv << POSDIV_SHIFT);
		writel(pll, reg);
		udelay(20);
	} else {

		pll = (readl(reg) & 0xF8FFFFFFu) | ((unsigned)target_posdiv << POSDIV_SHIFT);
		writel(pll, reg);
		udelay(20);
		if (!sym_mtk_fh_set_rate(pll_name, pcw, target_posdiv))
			return false;
	}
	return true;
}

static __nocfi void gpu_pll_work_fn(struct work_struct *work)
{
	struct gpufreq_opp_info *wt;
	unsigned int freq_old, freq_new;
	int idx;

	if (atomic_read(&oc_mt6789_suspended))
		return;
	if (!g_gpu_oc_active || !g_apmixed_va || !sym_gpufreq_get_working_table)
		return;

	idx = atomic_read(&g_last_commit_idx);
	if (idx < 0 || idx >= GPU_OPP_MAX || !g_gpu_patched[idx])
		return;

	wt = (struct gpufreq_opp_info *)sym_gpufreq_get_working_table(TARGET_GPU);
	if (!wt)
		return;

	freq_new = wt[idx].freq;
	freq_old = g_last_pll_freq ? g_last_pll_freq : g_gpu_orig[idx].freq;

	if (write_pll_con1_safe(MFGPLL_CON1, MFG_PLL_NAME, freq_old, freq_new))
		g_last_pll_freq = freq_new;
	else
		snprintf(gpu_oc_result, sizeof(gpu_oc_result),
			 "FAIL: PLL write refused (idx=%d) — OC not applied to PLL", idx);
}

static __nocfi int gpu_patch_one_index(struct gpufreq_opp_info *wt,
					struct gpufreq_opp_info *st,
					int n, int idx,
					unsigned int freq, unsigned int volt,
					unsigned int vsram)
{
	if (idx < 0 || idx >= n || idx >= GPU_OPP_MAX)
		return -EINVAL;

	if (!g_gpu_patched[idx]) {
		g_gpu_orig[idx] = wt[idx];
		g_gpu_patched[idx] = true;
	}

	wt[idx].freq   = freq;
	wt[idx].volt   = volt;
	wt[idx].vsram  = vsram;
	wt[idx].posdiv = freq_to_posdiv_gpu(freq);

	if (st) {
		st[idx].freq   = freq;
		st[idx].volt   = volt;
		st[idx].vsram  = vsram;
		st[idx].posdiv = freq_to_posdiv_gpu(freq);
	}
	return 0;
}

static __nocfi void ged_snapshot_resync(int idx, const struct gpufreq_opp_info *val)
{
	if (!sym_ged_g_working_table || !*sym_ged_g_working_table)
		return;

	(*sym_ged_g_working_table)[idx] = *val;
}

static __nocfi int gpu_patch_working_table(unsigned int freq, unsigned int volt,
			   unsigned int vsram)
{
	struct gpufreq_opp_info *wt, *st = NULL;
	int n, ret;

	if (!sym_gpufreq_get_working_table || !sym_gpufreq_get_opp_num) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: GPU symbols not resolved");
		return -ENODEV;
	}

	n  = sym_gpufreq_get_opp_num(TARGET_GPU);
	wt = (struct gpufreq_opp_info *)sym_gpufreq_get_working_table(TARGET_GPU);
	if (sym_gpufreq_get_signed_table)
		st = (struct gpufreq_opp_info *)sym_gpufreq_get_signed_table(TARGET_GPU);

	if (!wt || n <= 0) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: table NULL or opp_num=%d", n);
		return -ENODATA;
	}

	ret = gpu_patch_one_index(wt, st, n, 0, freq, volt, vsram);
	if (ret) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: idx0 patch rejected (%d)", ret);
		return ret;
	}
	ged_snapshot_resync(0, &wt[0]);
	snprintf(gpu_oc_result, sizeof(gpu_oc_result),
		 "OK: idx0 freq=%u volt=%u vsram=%u (n=%d, signed=%s, ged_sync=%s)",
		 freq, volt, vsram, n, st ? "yes" : "no",
		 (sym_ged_g_working_table && *sym_ged_g_working_table) ? "yes" : "no");
	return 0;
}

static __nocfi void gpu_restore_working_table(void)
{
	struct gpufreq_opp_info *wt, *st = NULL;
	int i, n;

	if (!sym_gpufreq_get_working_table || !sym_gpufreq_get_opp_num)
		return;
	wt = (struct gpufreq_opp_info *)sym_gpufreq_get_working_table(TARGET_GPU);
	if (!wt)
		return;
	if (sym_gpufreq_get_signed_table)
		st = (struct gpufreq_opp_info *)sym_gpufreq_get_signed_table(TARGET_GPU);
	n = sym_gpufreq_get_opp_num(TARGET_GPU);

	for (i = 0; i < GPU_OPP_MAX && i < n; i++) {
		if (!g_gpu_patched[i])
			continue;
		wt[i] = g_gpu_orig[i];
		if (st)
			st[i] = g_gpu_orig[i];
		ged_snapshot_resync(i, &g_gpu_orig[i]);
		g_gpu_patched[i] = false;
	}
	atomic_set(&g_last_commit_idx, -1);
	g_last_pll_freq = 0;
}

static __nocfi void update_gpu_opp_table(void)
{
	struct gpufreq_opp_info *wt;
	int i, n, off = 0;

	if (!sym_gpufreq_get_working_table || !sym_gpufreq_get_opp_num) {
		snprintf(gpu_opp_table, sizeof(gpu_opp_table), "symbols not ready");
		return;
	}
	n  = sym_gpufreq_get_opp_num(TARGET_GPU);
	wt = (struct gpufreq_opp_info *)sym_gpufreq_get_working_table(TARGET_GPU);
	if (!wt || n <= 0) { snprintf(gpu_opp_table, sizeof(gpu_opp_table), "unavailable"); return; }

	off = snprintf(gpu_opp_table, sizeof(gpu_opp_table), "GPU OPP (n=%d):\n", n);
	for (i = 0; i < n && off < (int)sizeof(gpu_opp_table) - 60; i++)
		off += snprintf(gpu_opp_table + off, sizeof(gpu_opp_table) - off,
			"[%2d]%s %7u KHz %7u mV*100 vsram=%7u\n",
			i, (i < GPU_OPP_MAX && g_gpu_patched[i]) ? "*" : " ",
			wt[i].freq, wt[i].volt, wt[i].vsram);
}

static int gpu_opp_table_get(char *buf, const struct kernel_param *kp)
{
	mutex_lock(&oc_lock);
	update_gpu_opp_table();
	mutex_unlock(&oc_lock);
	return scnprintf(buf, PAGE_SIZE, "%s", gpu_opp_table);
}
static const struct kernel_param_ops gpu_opp_table_ops = { .get = gpu_opp_table_get };
module_param_cb(gpu_opp_table, &gpu_opp_table_ops, NULL, 0444);
MODULE_PARM_DESC(gpu_opp_table, "READ-ONLY: live GPU OPP table dump (freq/volt/vsram), recomputed on every read");

static struct kretprobe krp_gpufreq_commit;

struct gpu_commit_args {
	enum gpufreq_target target;
	int                 oppidx;
};

static __nocfi int krp_gpufreq_commit_entry(struct kretprobe_instance *ri,
					    struct pt_regs *regs)
{
	struct gpu_commit_args *a = (struct gpu_commit_args *)ri->data;

	a->target = (enum gpufreq_target)(regs->regs[0] & 0xFFFFFFFFu);
	a->oppidx = (int)(regs->regs[1] & 0xFFFFFFFFu);
	return 0;
}

static __nocfi int krp_gpufreq_commit_ret(struct kretprobe_instance *ri,
					  struct pt_regs *regs)
{
	struct gpu_commit_args *a = (struct gpu_commit_args *)ri->data;
	bool is_patched_idx = (a->oppidx >= 0 && a->oppidx < GPU_OPP_MAX &&
				g_gpu_patched[a->oppidx]);

	if (atomic_read(&oc_mt6789_suspended))
		return 0;
	if (g_gpu_oc_active && g_oc_wq && a->target == TARGET_GPU && is_patched_idx) {
		atomic_set(&g_last_commit_idx, a->oppidx);
		queue_delayed_work(g_oc_wq, &g_gpu_pll_work, 0);
	}
	return 0;
}

static int lazy_map_hw(void)
{
	if (g_hw_mapped)
		return 0;

	g_apmixed_va = ioremap(APMIXED_PHYS, APMIXED_SIZE);
	if (!g_apmixed_va)
		pr_warn("oc_mt6789: apmixed ioremap failed -> GPU PLL write disabled\n");

	g_hw_mapped = true;
	return g_apmixed_va ? 0 : -ENODEV;
}

static int gpu_oc_apply_set(const char *val, const struct kernel_param *kpp)
{
	int v, ret;

	ret = kstrtoint(val, 0, &v);
	if (ret || v != 1)
		return ret;

	if (atomic_read(&oc_mt6789_suspended)) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: device suspending, refused");
		gpu_oc_apply = 0;
		return 0;
	}

	mutex_lock(&oc_lock);

	if (lazy_map_hw() < 0) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: hardware mapping failed");
		goto out;
	}

	if (gpu_target_khz == 0) {
		cancel_delayed_work_sync(&g_gpu_pll_work);
		gpu_restore_working_table();
		g_gpu_oc_active = false;
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "OK: GPU OC disabled, stock restored");
		goto out;
	}

	if (gpu_target_khz < POSDIV_4_MIN_FREQ || gpu_target_khz > POSDIV_2_MAX_FREQ) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result),
			 "FAIL: freq %u out of [%u, %u] KHz",
			 gpu_target_khz, POSDIV_4_MIN_FREQ, POSDIV_2_MAX_FREQ);
		goto out;
	}
	if (gpu_target_volt < VGPU_MIN_VOLT || gpu_target_volt > VGPU_MAX_VOLT) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result),
			 "FAIL: volt %u out of [%u, %u] mV*100",
			 gpu_target_volt, VGPU_MIN_VOLT, VGPU_MAX_VOLT);
		goto out;
	}
	if (gpu_target_vsram == 0)
		gpu_target_vsram = gpu_target_volt;
	if (gpu_target_vsram < VGPU_MIN_VOLT || gpu_target_vsram > VGPU_MAX_VOLT) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result),
			 "FAIL: vsram %u out of [%u, %u] mV*100",
			 gpu_target_vsram, VGPU_MIN_VOLT, VGPU_MAX_VOLT);
		goto out;
	}

	ret = gpu_patch_working_table(gpu_target_khz, gpu_target_volt, gpu_target_vsram);
	if (ret)
		goto out;

	g_gpu_oc_active = true;
	update_gpu_opp_table();

out:
	gpu_oc_apply = 0;
	mutex_unlock(&oc_lock);
	return 0;
}
static int gpu_oc_apply_get(char *buf, const struct kernel_param *kpp)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}
static const struct kernel_param_ops gpu_oc_apply_ops = {
	.set = gpu_oc_apply_set,
	.get = gpu_oc_apply_get,
};
module_param_cb(gpu_oc_apply, &gpu_oc_apply_ops, &gpu_oc_apply, 0644);
MODULE_PARM_DESC(gpu_oc_apply, "Write 1 to apply gpu_target_khz/volt/vsram to OPP idx0");

#define LUT_MAX_ENTRIES   32U
#define LUT_FREQ          GENMASK(11, 0)
/* Candidate voltage field inferred from MT6789 LUT/EEM correlation.
 * This is not proof of independent PMIC/MCUPM rail control. */
#define LUT_VOLT          GENMASK(28, 12)
#define LUT_ROW_SIZE      0x4
#define CPU_VOLT_STEP_RAW       625U
#define CPU_VOLT_DELTA_DEFAULT  20000U
#define CPU_VOLT_ABS_DEFAULT    112000U
#define CPU_VOLT_ABS_HARD_MAX   115000U

enum {
	REG_FREQ_LUT_TABLE,
	REG_FREQ_ENABLE,
	REG_FREQ_PERF_STATE,
	REG_FREQ_HW_STATE,
	REG_EM_POWER_TBL,
	REG_FREQ_LATENCY,
	REG_ARRAY_SIZE,
};

struct cpufreq_mtk_mirror {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	int nr_opp;
	cpumask_t related_cpus;
};

static unsigned int cpu_ll_rep_cpu = 0;
static unsigned int cpu_b_rep_cpu  = 6;
module_param(cpu_ll_rep_cpu, uint, 0444);
module_param(cpu_b_rep_cpu, uint, 0444);
MODULE_PARM_DESC(cpu_ll_rep_cpu, "Representative CPU# for little cluster domain");
MODULE_PARM_DESC(cpu_b_rep_cpu, "Representative CPU# for big cluster domain");

#define MAX_OC_PERCENT_OVER_STOCK  60

#define MAX_OC_ABSOLUTE_KHZ  2600000U

static unsigned int cpu_ll_target_khz = 0;
static unsigned int cpu_b_target_khz  = 0;

/* Candidate cpufreq-hw LUT voltage; this is not a direct PMIC command. */
static unsigned int cpu_volt_follow = 1;
static unsigned int cpu_volt_max_delta_raw = CPU_VOLT_DELTA_DEFAULT;
static unsigned int cpu_volt_abs_max_raw = CPU_VOLT_ABS_DEFAULT;
static unsigned int cpu_ll_volt_raw;
static unsigned int cpu_b_volt_raw;
static unsigned int cpu_volt_reassert = 1;
static unsigned int g_ll_applied_volt, g_b_applied_volt;
static bool g_ll_voltage_active, g_b_voltage_active;

module_param(cpu_volt_follow, uint, 0644);
MODULE_PARM_DESC(cpu_volt_follow, "1=derive idx0 LUT voltage above stock; 0=frequency-only unless explicit voltage is set");
module_param(cpu_volt_max_delta_raw, uint, 0644);
MODULE_PARM_DESC(cpu_volt_max_delta_raw, "Maximum LUT voltage increase over stock, raw mV*100; default +200mV");
module_param(cpu_volt_abs_max_raw, uint, 0644);
MODULE_PARM_DESC(cpu_volt_abs_max_raw, "Final LUT voltage ceiling raw mV*100; hard-clamped to 115000 (1.15V)");
module_param(cpu_ll_volt_raw, uint, 0644);
MODULE_PARM_DESC(cpu_ll_volt_raw, "Explicit A55 idx0 LUT voltage raw mV*100; 0=derive from LUT slope");
module_param(cpu_b_volt_raw, uint, 0644);
MODULE_PARM_DESC(cpu_b_volt_raw, "Explicit A76 idx0 LUT voltage raw mV*100; 0=derive from LUT slope");
module_param(cpu_volt_reassert, uint, 0644);
MODULE_PARM_DESC(cpu_volt_reassert, "1=reassert LUT voltage if EEM/SVS overwrites it; 0=off");
/* Requested CPU minimums are kept independently from cpufreq_policy.
 * Some MTK drivers rebuild policy limits and overwrite policy->min. */
static unsigned int g_ll_requested_min_khz;
static unsigned int g_b_requested_min_khz;
static int          cpu_oc_apply      = 0;
module_param(cpu_ll_target_khz, uint, 0644);
MODULE_PARM_DESC(cpu_ll_target_khz, "Little cluster idx0 target freq in KHz (0=leave alone)");
module_param(cpu_b_target_khz, uint, 0644);
MODULE_PARM_DESC(cpu_b_target_khz, "Big cluster idx0 target freq in KHz (0=leave alone)");

static char cpu_oc_result[256] = "not applied yet";
static int cpu_oc_result_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", cpu_oc_result);
}
static const struct kernel_param_ops cpu_oc_result_ops = { .get = cpu_oc_result_get };
module_param_cb(cpu_oc_result, &cpu_oc_result_ops, NULL, 0444);
MODULE_PARM_DESC(cpu_oc_result, "CPU OC status (read-only)");

static unsigned int g_ll_orig_khz, g_b_orig_khz;
static unsigned int g_ll_orig_volt, g_b_orig_volt;
static bool         g_ll_have_orig, g_b_have_orig;

static unsigned int cpu_volt_abs_max(void)
{
	return min_t(unsigned int, cpu_volt_abs_max_raw,
		     CPU_VOLT_ABS_HARD_MAX);
}

#define QUIESCE_POLL_US     500U
#define QUIESCE_TIMEOUT_US  50000U

static int quiesce_off_idx0(struct cpufreq_policy *policy, struct cpufreq_mtk_mirror *c,
			     struct freq_qos_request *qos_req)
{
	unsigned int waited_us = 0;
	int ret;

	if (c->nr_opp < 2)
		return -ENODATA;

	ret = freq_qos_add_request(&policy->constraints, qos_req, FREQ_QOS_MAX,
				    policy->freq_table[1].frequency);
	if (ret < 0)
		return ret;

	while (waited_us < QUIESCE_TIMEOUT_US) {
		if (readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]) != 0)
			return 0;

		usleep_range(QUIESCE_POLL_US, QUIESCE_POLL_US * 2);
		waited_us += QUIESCE_POLL_US;
	}

	freq_qos_remove_request(qos_req);
	return -ETIMEDOUT;
}

static int patch_em_table(unsigned int rep_cpu, unsigned int use_khz)
{
	struct em_perf_domain *pd;
	struct em_perf_state *top;

	pd = em_cpu_get(rep_cpu);
	if (!pd || !pd->table || !pd->nr_perf_states)
		return -ENODEV;

	top = &pd->table[pd->nr_perf_states - 1];
	if (top->frequency == use_khz)
		return 0;

	top->frequency = use_khz;
	return 0;
}

static int patch_cluster_idx0(unsigned int rep_cpu, unsigned int target_khz,
			       unsigned int *orig_khz, unsigned int *orig_volt,
			       bool *have_orig)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c;
	struct freq_qos_request qos_req;
	unsigned int use_khz, cap_khz, old_max, new_volt, override_volt;
	s64 dv = 0;
	u32 raw;
	int ret;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return -ENODEV;

	if (!policy->driver_data || !policy->freq_table) {
		cpufreq_cpu_put(policy);
		return -ENODATA;
	}
	c = (struct cpufreq_mtk_mirror *)policy->driver_data;

	if (!*have_orig) {
		*orig_khz = policy->freq_table[0].frequency;
		raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE]);
		*orig_volt = FIELD_GET(LUT_VOLT, raw);
		*have_orig = true;
	}

	if (target_khz == 0) {
		use_khz = *orig_khz;
	} else {
		cap_khz = *orig_khz + (*orig_khz * MAX_OC_PERCENT_OVER_STOCK) / 100;
		if (cap_khz > MAX_OC_ABSOLUTE_KHZ)
			cap_khz = MAX_OC_ABSOLUTE_KHZ;
		if (target_khz > cap_khz) {
			cpufreq_cpu_put(policy);
			return -ERANGE;
		}
		use_khz = target_khz;
	}


	/* Calculate voltage before any MMIO write; reject out-of-limit requests. */
	override_volt = (rep_cpu == cpu_ll_rep_cpu) ?
			cpu_ll_volt_raw : cpu_b_volt_raw;
	new_volt = *orig_volt;
	if (use_khz > *orig_khz && (cpu_volt_follow || override_volt)) {
		if (override_volt) {
			new_volt = DIV_ROUND_UP(override_volt, CPU_VOLT_STEP_RAW) *
				   CPU_VOLT_STEP_RAW;
			if (new_volt < *orig_volt)
				new_volt = *orig_volt;
		} else if (c->nr_opp >= 4) {
			u32 r1 = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE] + LUT_ROW_SIZE);
			u32 r3 = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE] + 3 * LUT_ROW_SIZE);
			int df = (int)FIELD_GET(LUT_FREQ, r1) - (int)FIELD_GET(LUT_FREQ, r3);
			int dvs = (int)FIELD_GET(LUT_VOLT, r1) - (int)FIELD_GET(LUT_VOLT, r3);
			if (df > 0 && dvs > 0) {
				dv = ((s64)dvs * (int)(use_khz / 1000 - *orig_khz / 1000)) / df;
				dv = (s64)(DIV_ROUND_UP_ULL((u64)max_t(s64, dv, 0),
							    CPU_VOLT_STEP_RAW) * CPU_VOLT_STEP_RAW);
				new_volt = *orig_volt + (unsigned int)dv;
			} else {
				pr_warn("oc_mt6789: cpu%u has no usable LUT voltage slope; frequency-only fallback\n", rep_cpu);
			}
		} else {
			pr_warn("oc_mt6789: cpu%u has fewer than 4 OPPs; frequency-only fallback\n", rep_cpu);
		}
		if (new_volt - *orig_volt > cpu_volt_max_delta_raw ||
		    new_volt > cpu_volt_abs_max()) {
			pr_err("oc_mt6789: cpu%u voltage request %u raw refused (stock=%u, delta limit=%u, abs limit=%u)\n",
			       rep_cpu, new_volt, *orig_volt, cpu_volt_max_delta_raw, cpu_volt_abs_max());
			*have_orig = false;
			cpufreq_cpu_put(policy);
			return -EOVERFLOW;
		}
	}

	old_max = policy->max;

	ret = quiesce_off_idx0(policy, c, &qos_req);
	if (ret) {
		cpufreq_cpu_put(policy);
		return ret;
	}

	raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE]);
	raw = (raw & ~(LUT_FREQ | LUT_VOLT)) |
	      FIELD_PREP(LUT_FREQ, use_khz / 1000) |
	      FIELD_PREP(LUT_VOLT, new_volt);
	writel_relaxed(raw, c->reg_bases[REG_FREQ_LUT_TABLE]);
	if (rep_cpu == cpu_ll_rep_cpu) {
		g_ll_applied_volt = new_volt;
		g_ll_voltage_active = use_khz > *orig_khz && new_volt > *orig_volt;
	} else {
		g_b_applied_volt = new_volt;
		g_b_voltage_active = use_khz > *orig_khz && new_volt > *orig_volt;
	}
	pr_info("oc_mt6789: cpu%u idx0 LUT freq=%u KHz voltage raw %u->%u%s\n",
		rep_cpu, use_khz, *orig_volt, new_volt,
		(use_khz > *orig_khz && new_volt > *orig_volt) ?
		" (candidate voltage raised)" : "");

	down_write(&policy->rwsem);
	policy->freq_table[0].frequency = use_khz;
	policy->cpuinfo.max_freq = use_khz;
	policy->max = use_khz;
	if (policy->min > use_khz || policy->min == old_max)
		policy->min = use_khz;
	up_write(&policy->rwsem);

	freq_qos_remove_request(&qos_req);

	if (patch_em_table(rep_cpu, use_khz))
		pr_warn("oc_mt6789: cpu%u has no EM perf domain — cooling device may still pin stock max\n",
			rep_cpu);

	cpufreq_cpu_put(policy);
	return 0;
}

static int cpu_oc_apply_set(const char *val, const struct kernel_param *kp)
{
	unsigned int trigger;
	int ret_ll = 0, ret_b = 0;

	if (kstrtouint(val, 10, &trigger))
		return -EINVAL;
	if (trigger != 1)
		return 0;

	if (atomic_read(&oc_mt6789_suspended)) {
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: device suspending, refused");
		return 0;
	}

	mutex_lock(&oc_lock);

	if (cpu_ll_target_khz || g_ll_have_orig)
		ret_ll = patch_cluster_idx0(cpu_ll_rep_cpu, cpu_ll_target_khz,
					     &g_ll_orig_khz, &g_ll_orig_volt,
					     &g_ll_have_orig);
	if (cpu_b_target_khz || g_b_have_orig)
		ret_b = patch_cluster_idx0(cpu_b_rep_cpu, cpu_b_target_khz,
					    &g_b_orig_khz, &g_b_orig_volt,
					    &g_b_have_orig);

	if (ret_ll == -EOVERFLOW || ret_b == -EOVERFLOW)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: LUT voltage exceeded delta/absolute ceiling (ll=%d b=%d, delta=%u, abs=%u)",
			 ret_ll, ret_b, cpu_volt_max_delta_raw, cpu_volt_abs_max());
	else if (ret_ll == -ERANGE || ret_b == -ERANGE)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: target exceeds OC limit (ll=%u b=%u KHz)",
			 cpu_ll_target_khz, cpu_b_target_khz);
	else if (ret_ll == -ETIMEDOUT || ret_b == -ETIMEDOUT)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: cluster would not leave idx0 within %ums",
			 QUIESCE_TIMEOUT_US / 1000);
	else if (ret_ll || ret_b)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: ll=%d b=%d", ret_ll, ret_b);
	else
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "OK: ll_max=%uKHz b_max=%uKHz (orig ll=%u b=%u)",
			 cpu_ll_target_khz ? cpu_ll_target_khz : g_ll_orig_khz,
			 cpu_b_target_khz ? cpu_b_target_khz : g_b_orig_khz,
			 g_ll_orig_khz, g_b_orig_khz);

	mutex_unlock(&oc_lock);
	return 0;
}

static int cpu_oc_apply_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}
static const struct kernel_param_ops cpu_oc_apply_ops = {
	.set = cpu_oc_apply_set,
	.get = cpu_oc_apply_get,
};
module_param_cb(cpu_oc_apply, &cpu_oc_apply_ops, &cpu_oc_apply, 0644);
MODULE_PARM_DESC(cpu_oc_apply, "Write 1 to apply cpu_ll_target_khz/cpu_b_target_khz to idx0");

static int cpu_min_khz_set(unsigned int rep_cpu, unsigned int *requested,
			   const char *val)
{
	struct cpufreq_policy *policy;
	unsigned int khz;

	if (kstrtouint(val, 10, &khz) || khz == 0)
		return -EINVAL;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return -ENODEV;

	mutex_lock(&oc_lock);

	down_write(&policy->rwsem);
	if (khz > policy->max)
		khz = policy->max;
	if (khz < policy->cpuinfo.min_freq)
		khz = policy->cpuinfo.min_freq;

	*requested = khz;
	policy->min = khz;
	up_write(&policy->rwsem);

	mutex_unlock(&oc_lock);
	cpufreq_cpu_put(policy);
	return 0;
}

static int cpu_min_khz_get(unsigned int rep_cpu, unsigned int requested,
			   char *buf)
{
	struct cpufreq_policy *policy;
	unsigned int value = requested;

	/* A non-zero requested value is the value userspace asked us to hold. */
	if (value)
		return scnprintf(buf, PAGE_SIZE, "%u\n", value);

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return scnprintf(buf, PAGE_SIZE, "0\n");
	value = policy->min;
	cpufreq_cpu_put(policy);
	return scnprintf(buf, PAGE_SIZE, "%u\n", value);
}

static int cpu_ll_min_khz_set(const char *val, const struct kernel_param *kp)
{
	return cpu_min_khz_set(cpu_ll_rep_cpu, &g_ll_requested_min_khz, val);
}
static int cpu_ll_min_khz_get(char *buf, const struct kernel_param *kp)
{
	return cpu_min_khz_get(cpu_ll_rep_cpu, g_ll_requested_min_khz, buf);
}
static const struct kernel_param_ops cpu_ll_min_khz_ops = {
	.set = cpu_ll_min_khz_set,
	.get = cpu_ll_min_khz_get,
};
module_param_cb(cpu_ll_min_khz, &cpu_ll_min_khz_ops, NULL, 0644);
MODULE_PARM_DESC(cpu_ll_min_khz,
		  "Requested little-cluster minimum KHz; persisted and guarded across policy refreshes");

static int cpu_b_min_khz_set(const char *val, const struct kernel_param *kp)
{
	return cpu_min_khz_set(cpu_b_rep_cpu, &g_b_requested_min_khz, val);
}
static int cpu_b_min_khz_get(char *buf, const struct kernel_param *kp)
{
	return cpu_min_khz_get(cpu_b_rep_cpu, g_b_requested_min_khz, buf);
}
static const struct kernel_param_ops cpu_b_min_khz_ops = {
	.set = cpu_b_min_khz_set,
	.get = cpu_b_min_khz_get,
};
module_param_cb(cpu_b_min_khz, &cpu_b_min_khz_ops, NULL, 0644);
MODULE_PARM_DESC(cpu_b_min_khz,
		  "Requested big-cluster minimum KHz; persisted and guarded across policy refreshes");

#define GUARDIAN_POLL_MS  50

static struct task_struct *g_guardian_thread;

static void guardian_reassert_voltage(struct cpufreq_policy *policy,
				      unsigned int rep_cpu)
{
	struct cpufreq_mtk_mirror *c = policy->driver_data;
	unsigned int want, now;
	u32 raw;

	if (!cpu_volt_reassert || !c)
		return;
	if (rep_cpu == cpu_ll_rep_cpu) {
		if (!g_ll_voltage_active)
			return;
		want = g_ll_applied_volt;
	} else {
		if (!g_b_voltage_active)
			return;
		want = g_b_applied_volt;
	}
	raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE]);
	now = FIELD_GET(LUT_VOLT, raw);
	if (now == want)
		return;
	raw = (raw & ~LUT_VOLT) | FIELD_PREP(LUT_VOLT, want);
	writel_relaxed(raw, c->reg_bases[REG_FREQ_LUT_TABLE]);
	pr_warn_ratelimited("oc_mt6789: cpu%u idx0 LUT voltage changed %u->%u; reasserted %u (does not prove rail voltage)\n",
			    rep_cpu, now, want, want);
}

static void guardian_check_one(unsigned int rep_cpu, unsigned int target_khz,
			       unsigned int orig_khz, bool have_orig,
			       unsigned int requested_min)
{
	struct cpufreq_policy *policy;
	unsigned int min_khz;

	/* Kept in the signature for compatibility with the existing call path. */
	(void)orig_khz;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return;

	down_write(&policy->rwsem);

	if (have_orig && target_khz) {
		if (policy->max != target_khz)
			policy->max = target_khz;
		if (policy->cpuinfo.max_freq != target_khz)
			policy->cpuinfo.max_freq = target_khz;
	}

	if (requested_min) {
		min_khz = requested_min;
		if (min_khz > policy->max)
			min_khz = policy->max;
		if (min_khz < policy->cpuinfo.min_freq)
			min_khz = policy->cpuinfo.min_freq;
		policy->min = min_khz;
	}

	if (policy->min > policy->max)
		policy->min = policy->max;

	guardian_reassert_voltage(policy, rep_cpu);
	up_write(&policy->rwsem);
	cpufreq_cpu_put(policy);
}

static int guardian_thread_fn(void *unused)
{
	while (!kthread_should_stop()) {
		mutex_lock(&oc_lock);
		guardian_check_one(cpu_ll_rep_cpu, cpu_ll_target_khz, g_ll_orig_khz,
				   g_ll_have_orig, g_ll_requested_min_khz);
		guardian_check_one(cpu_b_rep_cpu, cpu_b_target_khz, g_b_orig_khz,
				   g_b_have_orig, g_b_requested_min_khz);
		mutex_unlock(&oc_lock);
		msleep_interruptible(GUARDIAN_POLL_MS);
	}
	return 0;
}

#define DUMP_BUF_SIZE 4096
static char g_dump_buf[DUMP_BUF_SIZE];
static int probe_cpus[] = {0, 1, 2, 3, 4, 5, 6, 7};

static void dump_one_cpu(int cpu, char *buf, size_t *off, size_t bufsize,
			  void *seen_tables[], int *n_seen)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c;
	unsigned int cur_idx;
	int i;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return;

	if (!policy->driver_data || !policy->freq_table) {
		*off += scnprintf(buf + *off, bufsize - *off,
			"cpu%d: no driver_data/freq_table (driver != mtk-cpufreq-hw?)\n", cpu);
		cpufreq_cpu_put(policy);
		return;
	}


	for (i = 0; i < *n_seen; i++) {
		if (seen_tables[i] == (void *)policy->freq_table) {
			cpufreq_cpu_put(policy);
			return;
		}
	}
	seen_tables[*n_seen] = (void *)policy->freq_table;
	(*n_seen)++;

	c = (struct cpufreq_mtk_mirror *)policy->driver_data;
	cur_idx = readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]);

	*off += scnprintf(buf + *off, bufsize - *off,
		"\n=== cpu%d domain (nr_opp=%d, cur_idx=%u) ===\n", cpu, c->nr_opp, cur_idx);
	*off += scnprintf(buf + *off, bufsize - *off,
		"[idx] sw_freq(RAM)   hw_freq(MMIO)   LUT_volt(raw)   match?\n");

	for (i = 0; i < c->nr_opp && i < LUT_MAX_ENTRIES; i++) {
		unsigned int sw_freq = policy->freq_table[i].frequency;
		u32 raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE] + (i * LUT_ROW_SIZE));
		unsigned int hw_freq = FIELD_GET(LUT_FREQ, raw) * 1000;
		unsigned int hw_volt = FIELD_GET(LUT_VOLT, raw);
		const char *mark = (i == cur_idx) ? "*" : " ";
		const char *match = (sw_freq == hw_freq) ? "OK" : "MISMATCH";

		*off += scnprintf(buf + *off, bufsize - *off,
			"[%2d]%s %10u KHz  %10u KHz  %10u   %s\n",
			i, mark, sw_freq, hw_freq, hw_volt, match);

		if (*off >= bufsize - 128)
			return;
	}

	cpufreq_cpu_put(policy);
}

static int cpu_lut_table_get(char *buf, const struct kernel_param *kp)
{
	void *seen_tables[8] = {0};
	int n_seen = 0;
	size_t off = 0;
	int i;

	off += scnprintf(g_dump_buf + off, DUMP_BUF_SIZE - off,
		"cpu_lut_table: cross-check policy->freq_table (RAM) vs REG_FREQ_LUT_TABLE (MMIO)\n");

	for (i = 0; i < ARRAY_SIZE(probe_cpus); i++)
		dump_one_cpu(probe_cpus[i], g_dump_buf, &off, DUMP_BUF_SIZE, seen_tables, &n_seen);

	return scnprintf(buf, PAGE_SIZE, "%s", g_dump_buf);
}
static const struct kernel_param_ops cpu_lut_table_ops = { .get = cpu_lut_table_get };
module_param_cb(cpu_lut_table, &cpu_lut_table_ops, NULL, 0444);
MODULE_PARM_DESC(cpu_lut_table, "READ-ONLY: compares policy->freq_table vs REG_FREQ_LUT_TABLE per domain");

static char cpu_stats_refresh_result[128] = "not triggered yet";

static __nocfi int do_stats_refresh(unsigned int rep_cpu, unsigned int cur_khz)
{
	struct cpufreq_policy *policy;

	if (!sym_cpufreq_stats_free_table || !sym_cpufreq_stats_create_table)
		return -ENOSYS;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return -ENODEV;

	down_write(&policy->rwsem);
	if (cur_khz)
		policy->cur = cur_khz;
	sym_cpufreq_stats_free_table(policy);
	sym_cpufreq_stats_create_table(policy);
	up_write(&policy->rwsem);

	cpufreq_cpu_put(policy);
	return 0;
}

static int cpu_stats_refresh_set(const char *val, const struct kernel_param *kp)
{
	unsigned int which;
	int ret;

	if (kstrtouint(val, 10, &which))
		return -EINVAL;


	if (which != 1 && which != 2)
		return -EINVAL;

	if (atomic_read(&oc_mt6789_suspended)) {
		snprintf(cpu_stats_refresh_result, sizeof(cpu_stats_refresh_result),
			 "FAIL: device suspending, refused");
		return 0;
	}

	mutex_lock(&oc_lock);
	ret = do_stats_refresh(which == 1 ? cpu_ll_rep_cpu : cpu_b_rep_cpu,
				which == 1 ? cpu_ll_target_khz : cpu_b_target_khz);
	if (ret == -ENOSYS)
		snprintf(cpu_stats_refresh_result, sizeof(cpu_stats_refresh_result),
			 "FAIL: stats symbols not resolved");
	else if (ret == -ENODEV)
		snprintf(cpu_stats_refresh_result, sizeof(cpu_stats_refresh_result),
			 "FAIL: cpu%u not found/online", which == 1 ? cpu_ll_rep_cpu : cpu_b_rep_cpu);
	else
		snprintf(cpu_stats_refresh_result, sizeof(cpu_stats_refresh_result),
			 "OK: refreshed cluster %u — check time_in_state now", which);
	mutex_unlock(&oc_lock);
	return 0;
}
static int cpu_stats_refresh_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", cpu_stats_refresh_result);
}
static const struct kernel_param_ops cpu_stats_refresh_ops = {
	.set = cpu_stats_refresh_set,
	.get = cpu_stats_refresh_get,
};
static int cpu_stats_refresh_dummy = 0;
module_param_cb(cpu_stats_refresh, &cpu_stats_refresh_ops, &cpu_stats_refresh_dummy, 0644);
MODULE_PARM_DESC(cpu_stats_refresh, "Write 1=little / 2=big to rebuild that cluster's time_in_state table after a CPU OC apply");



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

static __nocfi ssize_t gpu_available_governor_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	/* Supported policies observed on the MT6789 Mali driver. */
	return scnprintf(buf, PAGE_SIZE, "coarse_demand always_on\n");
}

static __nocfi ssize_t gpu_governor_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	/*
	 * TODO: connect this to the active policy in the matching Mali kbase
	 * driver. Do not claim "Default"; this interface must remain present.
	 * The current device's native power_policy reports coarse_demand.
	 */
	return scnprintf(buf, PAGE_SIZE, "coarse_demand\n");
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
	__ATTR_RO(gpu_governor),
	__ATTR(gpu_max_clock, 0644, gpu_max_clock_show, gpu_max_clock_store),
	__ATTR(gpu_min_clock, 0644, gpu_min_clock_show, gpu_min_clock_store),
	__ATTR_RO(gpu_model),
};

static struct attribute *ski_gpu_attr_ptrs[ARRAY_SIZE(ski_gpu_attrs) + 1];

static const struct attribute_group ski_gpu_attr_group = {
	.attrs = ski_gpu_attr_ptrs,
};

static int oc_ski_init(void)
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

static void oc_ski_exit(void)
{
	if (ski_kobj) {
		sysfs_remove_group(ski_kobj, &ski_gpu_attr_group);
		kobject_put(ski_kobj);
		ski_kobj = NULL;
	}
}

static int __init oc_mt6789_init(void)
{
	unsigned long addr;
	int ret;

	pr_info("overclock_mt6789: init (GPU working_table patch + CPU cpufreq-hw LUT patch)\n");
	pr_info("oc_mt6789: CPU safety cap = stock +%d%%, %uMHz absolute ceiling (whichever is lower); raise in small steps and stress-test between each\n",
		MAX_OC_PERCENT_OVER_STOCK, MAX_OC_ABSOLUTE_KHZ / 1000);

	ret = register_pm_notifier(&oc_mt6789_pm_nb);
	if (ret)
		pr_warn("oc_mt6789: register_pm_notifier failed (%d) — suspend guard inactive\n", ret);

	g_oc_wq = alloc_workqueue("oc_mt6789_wq", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!g_oc_wq) {
		pr_err("oc_mt6789: failed to create workqueue\n");
		unregister_pm_notifier(&oc_mt6789_pm_nb);
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&g_gpu_pll_work, gpu_pll_work_fn);



	addr = resolve_ksym("gpufreq_get_working_table");
	if (addr)
		sym_gpufreq_get_working_table = (fn_get_wt_t)addr;
	else
		pr_warn("oc_mt6789: gpufreq_get_working_table not found — GPU module loaded?\n");

	addr = resolve_ksym("gpufreq_get_opp_num");
	if (addr)
		sym_gpufreq_get_opp_num = (fn_get_opp_num_t)addr;

	addr = resolve_ksym("gpufreq_get_signed_table");
	if (addr)
		sym_gpufreq_get_signed_table = (fn_get_wt_t)addr;
	else
		pr_warn("oc_mt6789: gpufreq_get_signed_table not found — signed table stays stock\n");

	addr = resolve_ksym("kallsyms_lookup_name");
	if (addr) {
		sym_kallsyms_lookup_name = (fn_kallsyms_lookup_name_t)addr;
		addr = sym_kallsyms_lookup_name("mtk_fh_set_rate");
		if (addr)

			sym_mtk_fh_set_rate = (fn_mtk_fh_set_rate_t)addr;
		else
			pr_warn("oc_mt6789: mtk_fh_set_rate not found — GPU PCW change fails safe\n");


		addr = sym_kallsyms_lookup_name("g_working_table");
		if (addr) {
			sym_ged_g_working_table = (struct gpufreq_opp_info **)addr;
			pr_info("oc_mt6789: found GED's g_working_table via kallsyms — current_freqency will reflect OC\n");
		} else {
			pr_warn("oc_mt6789: g_working_table not found via kallsyms (CONFIG_KALLSYMS_ALL off, ged.ko not loaded yet, or gpufreq v1 build) — GPU OC still applies at hardware level, but /sys/kernel/ged/hal/current_freqency will keep showing stock freq\n");
		}

		addr = sym_kallsyms_lookup_name("cpufreq_stats_free_table");
		if (addr)
			sym_cpufreq_stats_free_table = (fn_stats_free_t)addr;
		else
			pr_warn("oc_mt6789: cpufreq_stats_free_table not found — cpu_stats_refresh unavailable\n");

		addr = sym_kallsyms_lookup_name("cpufreq_stats_create_table");
		if (addr)
			sym_cpufreq_stats_create_table = (fn_stats_create_t)addr;
		else
			pr_warn("oc_mt6789: cpufreq_stats_create_table not found — cpu_stats_refresh unavailable\n");
	} else {
		pr_warn("oc_mt6789: kallsyms_lookup_name not found — GPU PCW change fails safe, and GED sync is unavailable\n");
	}

	memset(&krp_gpufreq_commit, 0, sizeof(krp_gpufreq_commit));
	krp_gpufreq_commit.kp.symbol_name = "gpufreq_commit";
	krp_gpufreq_commit.entry_handler  = krp_gpufreq_commit_entry;
	krp_gpufreq_commit.handler        = krp_gpufreq_commit_ret;
	krp_gpufreq_commit.data_size      = sizeof(struct gpu_commit_args);
	krp_gpufreq_commit.maxactive      = 4;
	ret = register_kretprobe(&krp_gpufreq_commit);
	if (ret < 0)
		pr_warn("oc_mt6789: kretprobe gpufreq_commit failed (%d)\n", ret);

	g_guardian_thread = kthread_run(guardian_thread_fn, NULL, "oc_mt6789_guard");
	if (IS_ERR(g_guardian_thread)) {
		pr_warn("oc_mt6789: guardian thread failed to start (%ld) — CPU scaling_max_freq may drift back to stock\n",
			PTR_ERR(g_guardian_thread));
		g_guardian_thread = NULL;
	}

	ret = oc_ski_init();
	if (ret)
		pr_warn("oc_mt6789: SKI sysfs setup failed (%d)\n", ret);

	pr_info("oc_mt6789: ready — sysfs at /sys/module/overclock_mt6789/parameters/\n");
	return 0;
}

static void __exit oc_mt6789_exit(void)
{
	oc_ski_exit();
	pr_info("overclock_mt6789: unloading\n");

	unregister_pm_notifier(&oc_mt6789_pm_nb);

	if (g_guardian_thread) {
		kthread_stop(g_guardian_thread);
		g_guardian_thread = NULL;
	}

	mutex_lock(&oc_lock);
	g_gpu_oc_active = false;
	mutex_unlock(&oc_lock);

	if (g_oc_wq) {
		cancel_delayed_work_sync(&g_gpu_pll_work);
		destroy_workqueue(g_oc_wq);
		g_oc_wq = NULL;
	}

	if (krp_gpufreq_commit.kp.addr)
		unregister_kretprobe(&krp_gpufreq_commit);

	mutex_lock(&oc_lock);
	gpu_restore_working_table();
	if (g_ll_have_orig)
		patch_cluster_idx0(cpu_ll_rep_cpu, 0, &g_ll_orig_khz,
				   &g_ll_orig_volt, &g_ll_have_orig);
	if (g_b_have_orig)
		patch_cluster_idx0(cpu_b_rep_cpu, 0, &g_b_orig_khz,
				   &g_b_orig_volt, &g_b_have_orig);
	mutex_unlock(&oc_lock);

	if (g_apmixed_va) {
		iounmap(g_apmixed_va);
		g_apmixed_va = NULL;
	}

	pr_info("overclock_mt6789: unloaded, stock restored where patched\n");
}

module_init(oc_mt6789_init);
module_exit(oc_mt6789_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Anomali1304");
MODULE_DESCRIPTION("GPU (working_table) + CPU (cpufreq-hw LUT) OC for MT6789 Helio G99 — POCO M5 rock");
MODULE_VERSION("1.0.0");
