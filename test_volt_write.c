// SPDX-License-Identifier: GPL-2.0
/*
 * test_volt_write.c
 *
 * STEP 2 — CONTROLLED WRITE TEST buat field voltage (bits[28:12]) di
 * REG_FREQ_LUT_TABLE yang ditemukan lewat korelasi manual vs
 * /proc/eem_lite/eem_cur_volt (lihat overclock_mt6789.c untuk detail LUT_VOLT).
 *
 * INI MENULIS KE HARDWARE BENERAN. Baca aturan mainnya:
 *
 *   1. Cuma boleh nulis ke index yang BUKAN cur_idx (index yang lagi aktif
 *      dipakai CPU sekarang). Kalau target == cur_idx, ditolak.
 *   2. Cuma boleh naikin voltage (delta > 0), dan di-clamp ke
 *      MAX_TEST_DELTA (default 2000 raw unit = 20.00mV). Delta negatif
 *      ditolak di modul ini -- ini murni test "apakah writable & bertahan",
 *      bukan buat nyari batas bawah.
 *   3. Semua bit lain di raw32 (freq, flagA, flagB, reserved) dipertahankan
 *      APA ADANYA dari nilai asli -- cuma LUT_VOLT yang diubah.
 *   4. Nilai asli disimpan sebelum ditulis, dan otomatis dikembalikan saat
 *      rmmod, atau kapan saja lewat test_restore=1.
 *   5. 3 detik setelah apply, modul baca ulang register itu sendiri dan
 *      catat apakah nilainya masih sama atau sudah ketimpa balik --
 *      hasilnya muncul di test_result tanpa perlu trigger apa-apa lagi.
 *
 * Cara pakai:
 *   insmod test_volt_write.ko test_target_cpu=0 test_target_idx=15 \
 *          test_volt_delta_raw=625
 *   cat /sys/module/test_volt_write/parameters/test_result
 *   # tunggu >3 detik, cat lagi test_result buat lihat hasil drift-check
 *   cat /sys/module/test_volt_write/parameters/test_readback
 *   echo 1 > /sys/module/test_volt_write/parameters/test_restore   # opsional, manual
 *   rmmod test_volt_write   # otomatis restore juga
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/stringify.h>

#define TAG "test_volt_write"

/* --- Layout register, disalin dari overclock_mt6789.c (lihat file itu
 * untuk catatan lengkap gimana field ini ditemukan). --- */
#define LUT_MAX_ENTRIES   32U
#define LUT_FREQ          GENMASK(11, 0)
#define LUT_VOLT          GENMASK(28, 12)
#define LUT_FLAG_A        BIT(29)
#define LUT_FLAG_B        BIT(30)
#define LUT_ROW_SIZE      0x4

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

#define MAX_TEST_DELTA_RAW  2000U   /* 20.00 mV, hard cap, tidak bisa dilewati param */

static unsigned int test_target_cpu     = 0;
static unsigned int test_target_idx     = 0xFFFFFFFF; /* wajib diisi user, tanpa default aman */
static unsigned int test_volt_delta_raw = 625;         /* ~1 step EEM, 6.25mV */
static int          test_apply          = 0;
static int          test_restore_trig   = 0;

module_param(test_target_cpu, uint, 0644);
MODULE_PARM_DESC(test_target_cpu, "Representative CPU# buat cluster yang mau ditest (cocokin sama cpu_ll_rep_cpu/cpu_b_rep_cpu di overclock_mt6789)");
module_param(test_target_idx, uint, 0644);
MODULE_PARM_DESC(test_target_idx, "LUT index yang mau ditest. WAJIB diisi, HARUS beda dari cur_idx yang lagi aktif.");
module_param(test_volt_delta_raw, uint, 0644);
MODULE_PARM_DESC(test_volt_delta_raw, "Kenaikan voltage, satuan sama kayak eem_volt (raw/100=mV). Max " __stringify(MAX_TEST_DELTA_RAW) ".");

static DEFINE_MUTEX(test_lock);
static bool     g_have_orig;
static u32      g_orig_raw;
static u32      g_written_raw;
static unsigned int g_locked_cpu;
static unsigned int g_locked_idx;
static void __iomem *g_row_addr;

static char test_result[300] = "belum dijalankan. isi test_target_idx lalu echo 1 > test_apply";

static struct workqueue_struct *g_wq;
static struct delayed_work g_drift_check_work;

static void drift_check_fn(struct work_struct *work)
{
	u32 now;

	mutex_lock(&test_lock);
	if (!g_have_orig || !g_row_addr) {
		mutex_unlock(&test_lock);
		return;
	}

	now = readl_relaxed(g_row_addr);

	if (now == g_written_raw) {
		snprintf(test_result, sizeof(test_result),
			 "OK: cpu%u idx%u ditulis 0x%08x, %us kemudian MASIH 0x%08x (bertahan, nggak ada yang nimpa balik). Belum di-restore -- echo 1 > test_restore kalau udah selesai lihat.",
			 g_locked_cpu, g_locked_idx, g_written_raw, 3, now);
	} else if (now == g_orig_raw) {
		snprintf(test_result, sizeof(test_result),
			 "REVERTED: cpu%u idx%u sempat jadi 0x%08x, tapi %us kemudian BALIK ke nilai asli 0x%08x sendiri -- ada sesuatu (firmware/EEM refresh?) yang nge-restore baris ini otomatis.",
			 g_locked_cpu, g_locked_idx, g_written_raw, 3, now);
	} else {
		snprintf(test_result, sizeof(test_result),
			 "UNEXPECTED: cpu%u idx%u ditulis 0x%08x, %us kemudian jadi 0x%08x -- beda dari yang ditulis MAUPUN dari asli. Cek manual, jangan lanjut dulu.",
			 g_locked_cpu, g_locked_idx, g_written_raw, 3, now);
	}

	mutex_unlock(&test_lock);
}

static int do_restore_locked(void)
{
	if (!g_have_orig || !g_row_addr)
		return -ENOENT;

	writel_relaxed(g_orig_raw, g_row_addr);
	pr_info(TAG ": restored cpu%u idx%u ke raw asli 0x%08x\n",
		g_locked_cpu, g_locked_idx, g_orig_raw);
	snprintf(test_result, sizeof(test_result),
		 "RESTORED: cpu%u idx%u dikembalikan ke 0x%08x", g_locked_cpu, g_locked_idx, g_orig_raw);
	g_have_orig = false;
	g_row_addr = NULL;
	return 0;
}

static int test_apply_set(const char *val, const struct kernel_param *kp)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c;
	unsigned int cur_idx;
	u32 raw, orig_volt, new_volt;
	int v, ret;

	ret = kstrtoint(val, 0, &v);
	if (ret || v != 1)
		return ret;

	if (test_target_idx == 0xFFFFFFFF) {
		snprintf(test_result, sizeof(test_result),
			 "FAIL: test_target_idx belum diisi. Set dulu ke index yang BUKAN cur_idx.");
		return 0;
	}
	if (test_volt_delta_raw == 0 || test_volt_delta_raw > MAX_TEST_DELTA_RAW) {
		snprintf(test_result, sizeof(test_result),
			 "FAIL: test_volt_delta_raw harus 1..%u (0.01mV..%u.%02umV)",
			 MAX_TEST_DELTA_RAW, MAX_TEST_DELTA_RAW / 100, MAX_TEST_DELTA_RAW % 100);
		return 0;
	}

	mutex_lock(&test_lock);

	if (g_have_orig) {
		snprintf(test_result, sizeof(test_result),
			 "FAIL: masih ada write aktif dari test sebelumnya (cpu%u idx%u) yang belum di-restore. echo 1 > test_restore dulu.",
			 g_locked_cpu, g_locked_idx);
		goto out;
	}

	policy = cpufreq_cpu_get(test_target_cpu);
	if (!policy) {
		snprintf(test_result, sizeof(test_result), "FAIL: cpu%u tidak ada/offline", test_target_cpu);
		goto out;
	}
	if (!policy->driver_data) {
		snprintf(test_result, sizeof(test_result), "FAIL: cpu%u driver_data NULL (driver != mtk-cpufreq-hw?)", test_target_cpu);
		cpufreq_cpu_put(policy);
		goto out;
	}
	c = (struct cpufreq_mtk_mirror *)policy->driver_data;

	if (test_target_idx >= (unsigned int)c->nr_opp || test_target_idx >= LUT_MAX_ENTRIES) {
		snprintf(test_result, sizeof(test_result),
			 "FAIL: idx%u di luar jangkauan (nr_opp=%d)", test_target_idx, c->nr_opp);
		cpufreq_cpu_put(policy);
		goto out;
	}

	cur_idx = readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]);
	if (test_target_idx == cur_idx) {
		snprintf(test_result, sizeof(test_result),
			 "FAIL: idx%u itu cur_idx sekarang (lagi aktif dipakai). Pilih index lain.", test_target_idx);
		cpufreq_cpu_put(policy);
		goto out;
	}

	g_row_addr = c->reg_bases[REG_FREQ_LUT_TABLE] + (test_target_idx * LUT_ROW_SIZE);
	raw = readl_relaxed(g_row_addr);
	orig_volt = FIELD_GET(LUT_VOLT, raw);
	new_volt = orig_volt + test_volt_delta_raw;

	if (new_volt > FIELD_MAX(LUT_VOLT)) {
		snprintf(test_result, sizeof(test_result),
			 "FAIL: orig_volt(%u) + delta(%u) overflow field 17-bit", orig_volt, test_volt_delta_raw);
		cpufreq_cpu_put(policy);
		g_row_addr = NULL;
		goto out;
	}

	g_orig_raw    = raw;
	g_written_raw = (raw & ~LUT_VOLT) | FIELD_PREP(LUT_VOLT, new_volt);
	g_locked_cpu  = test_target_cpu;
	g_locked_idx  = test_target_idx;

	writel_relaxed(g_written_raw, g_row_addr);
	/* readback segera */
	raw = readl_relaxed(g_row_addr);

	g_have_orig = true;

	cpufreq_cpu_put(policy);

	if (raw == g_written_raw) {
		snprintf(test_result, sizeof(test_result),
			 "APPLIED: cpu%u idx%u volt %u->%u (raw 0x%08x->0x%08x), readback segera COCOK. Drift-check nyusul %us lagi.",
			 test_target_cpu, test_target_idx, orig_volt, new_volt, g_orig_raw, g_written_raw, 3);
		queue_delayed_work(g_wq, &g_drift_check_work, msecs_to_jiffies(3000));
	} else {
		snprintf(test_result, sizeof(test_result),
			 "FAIL-READBACK: ditulis 0x%08x tapi kebaca balik 0x%08x -- write ditolak/tidak nempel sama sekali.",
			 g_written_raw, raw);
		/* nggak nempel -- nggak perlu restore/drift-check */
		g_have_orig = false;
		g_row_addr = NULL;
	}

out:
	mutex_unlock(&test_lock);
	return 0;
}

static int test_apply_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}
static const struct kernel_param_ops test_apply_ops = { .set = test_apply_set, .get = test_apply_get };
module_param_cb(test_apply, &test_apply_ops, &test_apply, 0644);
MODULE_PARM_DESC(test_apply, "Write 1 buat jalankan test: naikkan volt di test_target_idx sebesar test_volt_delta_raw");

static int test_restore_set(const char *val, const struct kernel_param *kp)
{
	int v, ret;

	ret = kstrtoint(val, 0, &v);
	if (ret || v != 1)
		return ret;

	mutex_lock(&test_lock);
	cancel_delayed_work_sync(&g_drift_check_work);
	if (do_restore_locked())
		snprintf(test_result, sizeof(test_result), "RESTORE: nggak ada write aktif buat di-restore");
	mutex_unlock(&test_lock);
	return 0;
}
static int test_restore_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}
static const struct kernel_param_ops test_restore_ops = { .set = test_restore_set, .get = test_restore_get };
module_param_cb(test_restore, &test_restore_ops, &test_restore_trig, 0644);
MODULE_PARM_DESC(test_restore, "Write 1 buat langsung restore raw asli ke index yang lagi ditest");

static int test_result_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", test_result);
}
static const struct kernel_param_ops test_result_ops = { .get = test_result_get };
module_param_cb(test_result, &test_result_ops, NULL, 0444);
MODULE_PARM_DESC(test_result, "READ-ONLY: status/hasil test kali terakhir");

static int test_readback_get(char *buf, const struct kernel_param *kp)
{
	u32 raw;

	mutex_lock(&test_lock);
	if (!g_have_orig || !g_row_addr) {
		mutex_unlock(&test_lock);
		return scnprintf(buf, PAGE_SIZE, "tidak ada write aktif sekarang\n");
	}
	raw = readl_relaxed(g_row_addr);
	mutex_unlock(&test_lock);

	return scnprintf(buf, PAGE_SIZE,
		"cpu%u idx%u: orig=0x%08x written=0x%08x now=0x%08x volt_now=%u\n",
		g_locked_cpu, g_locked_idx, g_orig_raw, g_written_raw, raw,
		(unsigned int)FIELD_GET(LUT_VOLT, raw));
}
static const struct kernel_param_ops test_readback_ops = { .get = test_readback_get };
module_param_cb(test_readback, &test_readback_ops, NULL, 0444);
MODULE_PARM_DESC(test_readback, "READ-ONLY: baca ulang raw register di index yang lagi ditest, real-time tiap kali di-cat");

static int __init test_volt_write_init(void)
{
	pr_info(TAG ": init -- STEP 2 controlled write test, HANYA index non-aktif, delta dibatasi %u.%02u mV\n",
		MAX_TEST_DELTA_RAW / 100, MAX_TEST_DELTA_RAW % 100);

	g_wq = alloc_workqueue(TAG "_wq", WQ_UNBOUND, 1);
	if (!g_wq)
		return -ENOMEM;
	INIT_DELAYED_WORK(&g_drift_check_work, drift_check_fn);

	pr_info(TAG ": siap. Set test_target_idx (WAJIB, harus beda dari cur_idx), lalu echo 1 > test_apply\n");
	return 0;
}

static void __exit test_volt_write_exit(void)
{
	mutex_lock(&test_lock);
	cancel_delayed_work_sync(&g_drift_check_work);
	if (g_have_orig) {
		do_restore_locked();
		pr_info(TAG ": auto-restore saat unload selesai\n");
	}
	mutex_unlock(&test_lock);

	if (g_wq)
		destroy_workqueue(g_wq);

	pr_info(TAG ": exit\n");
}

module_init(test_volt_write_init);
module_exit(test_volt_write_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Moon/Rock Project");
MODULE_DESCRIPTION("STEP 2: controlled single-index CPU LUT voltage write test (non-active idx only, capped delta, auto-restore)");
