// SPDX-License-Identifier: GPL-2.0-only
// MADE FOR XIAOMI 13 5.15 A13 KERNEL!
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <uapi/linux/sched/types.h>
#include "rsc.h"

#define RSC_CLAMP_MIN_PATH	"/proc/sys/kernel/sched_util_clamp_min"
#define RSC_TOPAPP_TASKS	"/dev/cpuset/top-app/tasks"
#define RSC_CPU_MAXFMT		"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq"
#define RSC_CPU_CURFMT		"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq"
#define RSC_VMCACHE_N		4
#define RSC_VMSTAT_TTL_MS	1000
#define RSC_BUDDY_TTL_MS	500

extern unsigned long uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id);
static int rsc_cpu_maxf[NR_CPUS];
static bool rsc_cpu_maxf_ok;
struct rsc_vmstat_cache {
	const char *key0;
	u64 val;
	unsigned long exp;
};
static struct rsc_vmstat_cache rsc_vmc[RSC_VMCACHE_N];
static u64 rsc_buddy_cached[3];
static unsigned long rsc_buddy_exp;
static unsigned long rsc_n_deduped;

struct rsc_boost {
	bool used;
	bool cap;
	pid_t pid;
	u32 umin;
	u32 umax;
	bool rt;
	int old_policy;
	int old_prio;
	unsigned long expires;
};

static struct rsc_boost rsc_boosts[RSC_MAX_BOOSTS];
static DEFINE_MUTEX(rsc_engine_lock);
static struct delayed_work rsc_revert_work;
static bool rsc_engine_up;
static bool rsc_revert_work_inited;
static unsigned long rsc_n_applied, rsc_n_reverted, rsc_n_failed;
static bool rsc_sys_boost_active;
static u32 rsc_sys_boost_saved = RSC_UCLAMP_MAX;
static u32 rsc_sys_boost_target;
static struct delayed_work rsc_sys_restore_work;
static bool rsc_sys_work_inited;
static unsigned long rsc_n_sys_boost;
unsigned int rsc_debug_mask = 0;
unsigned int rsc_func_mask;
module_param_named(debug, rsc_debug_mask, uint, 0644);
MODULE_PARM_DESC(debug, "/sys/rsc/debug log mask (0 = quiet)");
module_param_named(func_debug, rsc_func_mask, uint, 0644);
MODULE_PARM_DESC(func_debug, "/sys/rsc/func_debug per-function mask");

bool rsc_log_enabled(void)
{
	return READ_ONCE(rsc_debug_mask) || READ_ONCE(rsc_func_mask);
}

#define RSC_LOG(fmt, ...)						\
	do {								\
		if (rsc_log_enabled())					\
			pr_info_ratelimited(fmt, ##__VA_ARGS__);	\
	} while (0)

unsigned int rsc_boost_ms = 3000;
module_param(rsc_boost_ms, uint, 0644);
MODULE_PARM_DESC(rsc_boost_ms, "how long a QoS/RT/cap boost stays applied (ms)");
unsigned int rsc_ui_boost_umin = 512;
module_param_named(ui_boost_umin, rsc_ui_boost_umin, uint, 0644);
MODULE_PARM_DESC(ui_boost_umin, "uclamp util_min used by svp_ui_boost / ex_boost / fp_hint (default 512, max 1024)");
unsigned int rsc_sys_boost_step = 256;
module_param_named(sys_boost_step, rsc_sys_boost_step, uint, 0644);
MODULE_PARM_DESC(sys_boost_step, "system-wide sched_util_clamp_min step per sched_boost level (default 256)");

int rsc_read_int_file(const char *path, int def)
{
	struct file *f;
	char b[32];
	ssize_t n;
	loff_t pos = 0;
	int v;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return def;
	n = kernel_read(f, b, sizeof(b) - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return def;
	b[n] = '\0';
	if (kstrtoint(strim(b), 0, &v))
		return def;
	return v;
}
int rsc_write_int_file(const char *path, u32 val)
{
	struct file *f;
	char b[16];
	int len;
	loff_t pos = 0;
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return (int)PTR_ERR(f);
	len = scnprintf(b, sizeof(b), "%u", val);
	kernel_write(f, b, len, &pos);
	filp_close(f, NULL);
	return 0;
}
static void rsc_init_maxfreq(void)
{
	int c;

	if (rsc_cpu_maxf_ok)
		return;
	for_each_possible_cpu(c) {
		char path[64];

		scnprintf(path, sizeof(path), RSC_CPU_MAXFMT, c);
		rsc_cpu_maxf[c] = rsc_read_int_file(path, 0);
	}
	rsc_cpu_maxf_ok = true;
}
int rsc_cpu_maxfreq(int cpu)
{
	if (cpu < 0 || cpu >= NR_CPUS)
		return 0;
	rsc_init_maxfreq();
	return rsc_cpu_maxf[cpu];
}
int rsc_cpuset_first_pid(const char *path)
{
	int pid = 0;
	struct file *f;
	char b[128];
	ssize_t n;
	loff_t pos = 0;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return 0;
	n = kernel_read(f, b, sizeof(b) - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return 0;
	b[n] = '\0';
	{
		char *e;
		for (e = b; *e && *e != '\n' && *e != ' ' && *e != '\t'; e++)
			;
		*e = '\0';
	}
	if (kstrtoint(b, 0, &pid))
		pid = 0;
	return pid;
}
int rsc_collect_cpuset_pids(const char *path, pid_t *out, int max)
{
	struct file *f;
	char b[1024];
	ssize_t n;
	loff_t pos = 0;
	int count = 0;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return 0;
	while (count < max && (n = kernel_read(f, b, sizeof(b) - 1, &pos)) > 0) {
		const char *p = b;
		b[n] = '\0';
		while (count < max && *p) {
			char tok[12];
			size_t tl = 0;
			int pid = 0;
			while (*p >= '0' && *p <= '9' && tl < sizeof(tok) - 1)
				tok[tl++] = *p++;
			tok[tl] = '\0';
			if (tl && !kstrtoint(tok, 0, &pid) && pid > 0)
				out[count++] = pid;
			while (*p && (*p < '0' || *p > '9'))
				p++;
		}
	}
	filp_close(f, NULL);
	return count;
}
int rsc_parse_ints(const char *buf, int *out, int max)
{
	const char *p = buf;
	int n = 0;
	while (n < max) {
		char tmp[16];
		size_t len = 0;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (!*p)
			break;
		while (*p && *p != ' ' && *p != '\t' && *p != '\r' &&
		       *p != '\n' && len < sizeof(tmp) - 1)
			tmp[len++] = *p++;
		tmp[len] = '\0';
		if (kstrtoint(tmp, 0, &out[n]))
			break;
		n++;
	}
	return n;
}
struct task_struct *rsc_pid_task(pid_t pid)
{
	struct task_struct *p;

	if (pid <= 0)
		return NULL;
	rcu_read_lock();
	p = find_task_by_vpid(pid);
	if (p)
		get_task_struct(p);
	rcu_read_unlock();
	return p;
}
static int rsc_uclamp_set(struct task_struct *p, u32 umin, u32 umax)
{
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_flags = SCHED_FLAG_KEEP_POLICY |
			       SCHED_FLAG_KEEP_PARAMS |
			       SCHED_FLAG_UTIL_CLAMP_MIN |
			       SCHED_FLAG_UTIL_CLAMP_MAX,
		.sched_util_min = umin,
		.sched_util_max = umax,
	};
	return sched_setattr_nocheck(p, &attr);
}
static void rsc_slot_clear(struct rsc_boost *b)
{
	struct task_struct *p = rsc_pid_task(b->pid);
	if (p) {
		if (b->rt) {
			struct sched_param sp = { .sched_priority = b->old_prio };
			sched_setscheduler_nocheck(p, b->old_policy, &sp);
		}
		rsc_uclamp_set(p, 0, RSC_UCLAMP_MAX);
		put_task_struct(p);
	}
	b->used = false;
	rsc_n_reverted++;
}

static void rsc_revert_fn(struct work_struct *w)
{
	unsigned long next = 0;
	bool pending = false;
	int i;
	mutex_lock(&rsc_engine_lock);
	for (i = 0; i < RSC_MAX_BOOSTS; i++) {
		struct rsc_boost *b = &rsc_boosts[i];
		if (!b->used)
			continue;
		if (time_before(jiffies, b->expires)) {
			unsigned long d = b->expires - jiffies;
			if (!pending || d < next)
				next = d;
			pending = true;
			continue;
		}
		rsc_slot_clear(b);
	}
	mutex_unlock(&rsc_engine_lock);
	if (pending)
		schedule_delayed_work(&rsc_revert_work, next ? next : 1);
}
static int rsc_slot_apply(pid_t pid, u32 umin, u32 umax, bool rt, bool cap)
{
	struct task_struct *p;
	struct rsc_boost *slot = NULL, *free_slot = NULL;
	unsigned int ms = READ_ONCE(rsc_boost_ms);
	int i, ret;
	if (!rsc_engine_up)
		return -ENODEV;
	if (pid <= 0)
		return -EINVAL;
	if (umax == 0 || umax > RSC_UCLAMP_MAX)
		umax = RSC_UCLAMP_MAX;
	if (umin > umax)
		umin = umax;
	mutex_lock(&rsc_engine_lock);
	for (i = 0; i < RSC_MAX_BOOSTS; i++) {
		if (rsc_boosts[i].used && rsc_boosts[i].pid == pid) {
			slot = &rsc_boosts[i];
			break;
		}
		if (!rsc_boosts[i].used && !free_slot)
			free_slot = &rsc_boosts[i];
	}
	if (!slot)
		slot = free_slot;
	if (!slot) {
		mutex_unlock(&rsc_engine_lock);
		rsc_n_failed++;
		return -ENOSPC;
	}
	if (!cap && !umin && !rt) {
		if (slot->used)
			rsc_slot_clear(slot);
		mutex_unlock(&rsc_engine_lock);
		return 0;
	}
	if (slot->used && slot->umin == umin && slot->umax == umax &&
	    slot->rt == rt && slot->cap == cap) {
		rsc_n_deduped++;
		mutex_unlock(&rsc_engine_lock);
		return 0;
	}
	p = rsc_pid_task(pid);
	if (!p) {
		mutex_unlock(&rsc_engine_lock);
		rsc_n_failed++;
		return -ESRCH;
	}
	ret = rsc_uclamp_set(p, umin, umax);
	if (ret) {
		put_task_struct(p);
		mutex_unlock(&rsc_engine_lock);
		rsc_n_failed++;
		return ret;
	}
	if (rt) {
		struct sched_param sp = { .sched_priority = RSC_RT_PRIO };
		if (!slot->used || !slot->rt) {
			slot->old_policy = p->policy;
			slot->old_prio = p->rt_priority;
		}
		sched_setscheduler_nocheck(p, SCHED_RR, &sp);
	}
	put_task_struct(p);
	slot->used = true;
	slot->cap = cap;
	slot->pid = pid;
	slot->umin = umin;
	slot->umax = umax;
	slot->rt = rt;
	slot->expires = jiffies + msecs_to_jiffies(ms);
	rsc_n_applied++;
	mutex_unlock(&rsc_engine_lock);
	schedule_delayed_work(&rsc_revert_work, msecs_to_jiffies(ms));
	RSC_LOG(": %s pid=%d umin=%u umax=%u rt=%d %ums\n", cap ? "cap" : "boost", pid, umin, umax, rt, ms);
	return 0;
}
int rsc_boost_apply(pid_t pid, u32 umin, u32 umax, bool rt)
{
	if (!READ_ONCE(rsc_gate_engine))
		return -EPERM;
	if (!READ_ONCE(rsc_gate_qos) && !rt)
		return -EPERM;
	if (rt && !READ_ONCE(rsc_gate_svp))
		return -EPERM;
	return rsc_slot_apply(pid, umin, umax, rt, false);
}
int rsc_boost_apply_cap(pid_t pid, u32 umax)
{
	if (umax >= RSC_UCLAMP_MAX)
		return rsc_slot_apply(pid, 0, RSC_UCLAMP_MAX, false, false);
	return rsc_slot_apply(pid, 0, umax, false, true);
}
void rsc_caps_clear(void)
{
	int i;
	mutex_lock(&rsc_engine_lock);
	for (i = 0; i < RSC_MAX_BOOSTS; i++)
		if (rsc_boosts[i].used && rsc_boosts[i].cap)
			rsc_slot_clear(&rsc_boosts[i]);
	mutex_unlock(&rsc_engine_lock);
}
void rsc_clear_all(void)
{
	int i;
	mutex_lock(&rsc_engine_lock);
	for (i = 0; i < RSC_MAX_BOOSTS; i++)
		if (rsc_boosts[i].used)
			rsc_slot_clear(&rsc_boosts[i]);
	mutex_unlock(&rsc_engine_lock);
}
static void rsc_sys_restore_fn(struct work_struct *w)
{
	if (!rsc_sys_boost_active)
		return;
	rsc_write_int_file(RSC_CLAMP_MIN_PATH, rsc_sys_boost_saved);
	rsc_sys_boost_active = false;
	pr_info(DRV_NAME ": system boost off (clamp_min %u restored)\n",
		rsc_sys_boost_saved);
}
void rsc_system_boost(int level)
{
	u32 target;
	if (!rsc_sys_work_inited || !READ_ONCE(rsc_gate_engine))
		return;
	if (level <= 0) {
		cancel_delayed_work_sync(&rsc_sys_restore_work);
		if (rsc_sys_boost_active) {
			rsc_write_int_file(RSC_CLAMP_MIN_PATH,
					   rsc_sys_boost_saved);
			rsc_sys_boost_active = false;
			pr_info(DRV_NAME ": system boost cleared\n");
		}
		return;
	}
	target = min_t(u32, RSC_UCLAMP_MAX,
		       (u32)level * max_t(u32, READ_ONCE(rsc_sys_boost_step), 64));
	if (target < 128)
		target = 128;
	if (rsc_sys_boost_active && rsc_sys_boost_target == target) {
		rsc_n_deduped++;
		return;
	}
	if (!rsc_sys_boost_active) {
		int cur = rsc_read_int_file(RSC_CLAMP_MIN_PATH, RSC_UCLAMP_MAX);

		rsc_sys_boost_saved = cur > 0 ? cur : RSC_UCLAMP_MAX;
	}
	rsc_sys_boost_target = target;
	rsc_write_int_file(RSC_CLAMP_MIN_PATH, target);
	rsc_sys_boost_active = true;
	rsc_n_sys_boost++;
	mod_delayed_work(system_wq, &rsc_sys_restore_work, msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	RSC_LOG(": system boost level=%d clamp_min=%u (was %u)\n", level, target, rsc_sys_boost_saved);
}
bool rsc_system_boost_active(void)
{
	return rsc_sys_boost_active;
}
static int rsc_tgid_threads(pid_t tgid, pid_t *out, int max)
{
	struct task_struct *g, *t;
	int n = 0;
	if (tgid <= 0)
		return 0;
	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (task_tgid_vnr(t) == tgid) {
			out[n++] = task_pid_vnr(t);
			if (n >= max)
				break;
		}
	}
	rcu_read_unlock();
	return n;
}
int rsc_topapp_boost(u32 umin)
{
	pid_t pids[32];
	int n, i;
	umin = min_t(u32, umin, RSC_UCLAMP_MAX);
	if (umin)
		umin = min_t(u32, umin, READ_ONCE(rsc_ui_boost_umin));
	n = rsc_tgid_threads(rsc_cpuset_first_pid(RSC_TOPAPP_TASKS), pids, ARRAY_SIZE(pids));
	if (!n)
		n = rsc_collect_cpuset_pids(RSC_TOPAPP_TASKS, pids, ARRAY_SIZE(pids));
	for (i = 0; i < n; i++) {
		if (umin)
			rsc_boost_apply(pids[i], umin, RSC_UCLAMP_MAX, false);
		else
			rsc_boost_apply(pids[i], 0, RSC_UCLAMP_MAX, false);
	}
	if (n)
		RSC_LOG(": top-app boost umin=%u over %d task(s)\n",
				    umin, n);
	return n;
}
#define RSC_CPULIMIT_MAGIC 0x053EC600u // :P

static int rsc_write_str_file(const char *path, const char *s);
static bool filp_open_ok(const char *path)
{
	struct file *f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return false;
	filp_close(f, NULL);
	return true;
}
static const char *const rsc_bg_cpus_paths[] = {
	"/dev/cpuset/background/cpus",
	"/dev/cpuset/system-background/cpus",
	"/dev/cpuset/l-background/cpus",
	"/dev/cpuset/h-background/cpus",
};
static const char *const rsc_bg_tasks_paths[] = {
	"/dev/cpuset/background/tasks",
	"/dev/cpuset/system-background/tasks",
	"/dev/cpuset/l-background/tasks",
	"/dev/cpuset/h-background/tasks",
};
#define RSC_CPULIMIT_UTIL_MAX 256
#define RSC_BG_GROUPS ARRAY_SIZE(rsc_bg_cpus_paths)

static char rsc_bg_saved[RSC_BG_GROUPS][32];
static bool rsc_bg_saved_ok[RSC_BG_GROUPS];
static bool rsc_cpulimit_on;
static bool rsc_cpulimit_inited;
static struct delayed_work rsc_cpulimit_work;
static int rsc_cpulimit_scene_now;
static unsigned long rsc_n_cpulimit;
static int rsc_read_str_file(const char *path, char *out, size_t len)
{
	struct file *f;
	ssize_t n;
	loff_t pos = 0;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return (int)PTR_ERR(f);
	n = kernel_read(f, out, len - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return -EIO;
	out[n] = '\0';
	strim(out);
	return 0;
}
static void rsc_little_mask(char *out, size_t len);
static void rsc_little_mask_to_cpumask(struct cpumask *m)
{
	char buf[32];
	rsc_little_mask(buf, sizeof(buf));
	rsc_mask_parse(buf, m);
}
static void rsc_little_mask(char *out, size_t len)
{
	int cpu, minf = 0, freq;
	struct cpumask mask;
	char *p = out;
	int i;
	cpumask_clear(&mask);
	for_each_possible_cpu(cpu) {
		freq = rsc_cpu_maxfreq(cpu);
		if (freq > 0 && (minf == 0 || freq < minf))
			minf = freq;
	}
	if (!minf) {
		scnprintf(out, len, "0");
		return;
	}
	for_each_possible_cpu(cpu) {
		if (rsc_cpu_maxfreq(cpu) == minf)
			cpumask_set_cpu(cpu, &mask);
	}
	*out = '\0';
	for (i = 0; i < nr_cpu_ids; i++) {
		int start, end;
		if (!cpumask_test_cpu(i, &mask))
			continue;
		start = i;
		while (i + 1 < nr_cpu_ids && cpumask_test_cpu(i + 1, &mask))
			i++;
		end = i;
		p += scnprintf(p, len - (p - out), "%s%d", p == out ? "" : ",",
			       start);
		if (end > start)
			p += scnprintf(p, len - (p - out), "-%d", end);
	}
	if (!out[0])
		scnprintf(out, len, "0");
}
static void rsc_cpulimit_restore(void)
{
	int i;
	rsc_caps_clear();
	for (i = 0; i < RSC_BG_GROUPS; i++) {
		if (!rsc_bg_saved_ok[i])
			continue;
		rsc_write_str_file(rsc_bg_cpus_paths[i], rsc_bg_saved[i]);
		rsc_bg_saved_ok[i] = false;
	}
	rsc_cpulimit_on = false;
	rsc_cpulimit_scene_now = 0;
}
static void rsc_cpulimit_work_fn(struct work_struct *w)
{
	if (rsc_cpulimit_on) {
		rsc_cpulimit_restore();
		pr_info(DRV_NAME ": cpulimit scene restored\n");
	}
}
int rsc_cpulimit_scene(int scene)
{
	char mask[32];
	int i, applied = 0;
	if (!rsc_cpulimit_inited)
		return -ENODEV;
	if (scene < 0 || scene > 6)
		return -EINVAL;
	if (rsc_cpulimit_on && rsc_cpulimit_scene_now == scene) {
		rsc_n_deduped++;
		return 0;
	}
	cancel_delayed_work_sync(&rsc_cpulimit_work);
	if (scene == 0) {
		rsc_cpulimit_restore();
		pr_info(DRV_NAME ": cpulimit scene 0 (restored)\n");
		return 0;
	}
	rsc_little_mask(mask, sizeof(mask));
	for (i = 0; i < RSC_BG_GROUPS; i++) {
		if (rsc_bg_saved_ok[i])
			continue;
		if (rsc_read_str_file(rsc_bg_cpus_paths[i], rsc_bg_saved[i],
				      sizeof(rsc_bg_saved[i])))
			continue;
		rsc_bg_saved_ok[i] = true;
	}
	for (i = 0; i < RSC_BG_GROUPS; i++) {
		if (!rsc_bg_saved_ok[i])
			continue;
		if (!rsc_write_str_file(rsc_bg_cpus_paths[i], mask))
			applied++;
	}
	{
		pid_t pids[96];
		int total = 0, g;
		for (g = 0; g < RSC_BG_GROUPS && total < ARRAY_SIZE(pids); g++)
			total += rsc_collect_cpuset_pids(rsc_bg_tasks_paths[g],
					pids + total,
					ARRAY_SIZE(pids) - total);
		for (i = 0; i < total; i++)
			rsc_boost_apply_cap(pids[i], RSC_CPULIMIT_UTIL_MAX);
		RSC_LOG(": cpulimit scene %d: capped %d background task(s) at util_max %d\n",
				    scene, total, RSC_CPULIMIT_UTIL_MAX);
	}
	rsc_cpulimit_on = true;
	rsc_cpulimit_scene_now = scene;
	rsc_n_cpulimit++;
	schedule_delayed_work(&rsc_cpulimit_work, msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	RSC_LOG(": cpulimit scene %d -> background groups on cpus %s (%d groups)\n", scene, mask, applied);
	return applied ? 0 : -ENODEV;
}
bool rsc_cpulimit_active(void)
{
	return rsc_cpulimit_on;
}
int rsc_cpulimit_scene_active(void)
{
	return rsc_cpulimit_scene_now;
}
#define RSC_POLICY_MAXFMT "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq"
#define RSC_POLICY_MINFMT "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq"
#define RSC_POLICY_CPUFMT "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq"
#define RSC_POL_MAX	8
struct rsc_freq_slot {
	char saved[16];
	char saved_min[16];
	bool ok;
	bool ok_min;
};
static struct rsc_freq_slot rsc_freq[RSC_POL_MAX];
static bool rsc_freq_active;
static bool rsc_freq_inited;
static struct delayed_work rsc_freq_work;
static void rsc_freq_restore(void)
{
	int pol;
	for (pol = 0; pol < RSC_POL_MAX; pol++) {
		char path[80];
		if (!rsc_freq[pol].ok)
			continue;
		scnprintf(path, sizeof(path), RSC_POLICY_MAXFMT, pol);
		rsc_write_str_file(path, rsc_freq[pol].saved);
		if (rsc_freq[pol].ok_min) {
			scnprintf(path, sizeof(path), RSC_POLICY_MINFMT, pol);
			rsc_write_str_file(path, rsc_freq[pol].saved_min);
			rsc_freq[pol].ok_min = false;
		}
		rsc_freq[pol].ok = false;
	}
	rsc_freq_active = false;
}
static void rsc_freq_work_fn(struct work_struct *w)
{
	if (rsc_freq_active) {
		rsc_freq_restore();
		pr_info(DRV_NAME ": freq_max cap released\n");
	}
}
int rsc_freq_max_apply(u32 khz)
{
	int pol, applied = 0;
	if (!rsc_freq_inited)
		return -ENODEV;
	cancel_delayed_work_sync(&rsc_freq_work);
	if (!khz) {
		rsc_freq_restore();
		return 0;
	}
	for (pol = 0; pol < RSC_POL_MAX; pol++) {
		char pmin[80], pmax[80], pcap[80];
		int minf, capf, cur;
		u32 target;
		scnprintf(pmin, sizeof(pmin), RSC_POLICY_MINFMT, pol);
		minf = rsc_read_int_file(pmin, 0);
		if (minf <= 0)
			continue;
		scnprintf(pmax, sizeof(pmax), RSC_POLICY_MAXFMT, pol);
		scnprintf(pcap, sizeof(pcap), RSC_POLICY_CPUFMT, pol);
		capf = rsc_read_int_file(pcap, 0);
		if (!capf)
			capf = khz;
		if (!rsc_freq[pol].ok) {
			if (rsc_read_str_file(pmax, rsc_freq[pol].saved, sizeof(rsc_freq[pol].saved)))
				continue;
			rsc_freq[pol].ok = true;
		}
		cur = rsc_read_int_file(pmax, 0);
		target = min_t(u32, khz, (u32)capf);
		if ((int)target < minf)
			target = minf;
		rsc_write_int_file(pmax, target);
		applied++;
		RSC_LOG(": freq_max policy%d %d -> %u kHz\n", pol, cur, target);
	}
	rsc_freq_active = true;
	schedule_delayed_work(&rsc_freq_work, msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	return applied ? 0 : -ENODEV;
}
bool rsc_freq_max_active(void)
{
	return rsc_freq_active;
}
#define RSC_CPU_ONLINE		"/sys/devices/system/cpu/online"
#define RSC_CPU_ONLINEFMT	"/sys/devices/system/cpu/cpu%d/online"

static struct cpumask rsc_cpu_saved;
static bool rsc_cpu_saved_ok;
static bool rsc_cpu_active;
static bool rsc_cpu_inited;
static struct delayed_work rsc_cpu_work;

void rsc_mask_parse(const char *s, struct cpumask *m)
{
	cpumask_clear(m);
	while (*s) {
		int a = 0, b = 0;
		bool range = false;
		while (*s == ' ' || *s == ',')
			s++;
		if (*s < '0' || *s > '9')
			break;
		while (*s >= '0' && *s <= '9')
			a = a * 10 + (*s++ - '0');
		b = a;
		if (*s == '-') {
			s++;
			b = 0;
			while (*s >= '0' && *s <= '9')
				b = b * 10 + (*s++ - '0');
			range = true;
		}
		for (; a <= b && a < nr_cpu_ids; a++)
			cpumask_set_cpu(a, m);
		if (!range)
			continue;
	}
}
static int rsc_cpu_online_count(void)
{
	struct cpumask m;
	char buf[64];
	int n;
	if (rsc_read_str_file(RSC_CPU_ONLINE, buf, sizeof(buf)))
		return num_online_cpus();
	rsc_mask_parse(buf, &m);
	n = cpumask_weight(&m);
	return n ? n : num_online_cpus();
}
static void rsc_cpu_restore(void)
{
	int cpu;
	if (!rsc_cpu_saved_ok) {
		rsc_cpu_active = false;
		return;
	}
	for_each_possible_cpu(cpu) {
		char path[64];
		bool want;
		if (!cpu)
			continue;
		want = cpumask_test_cpu(cpu, &rsc_cpu_saved);
		scnprintf(path, sizeof(path), RSC_CPU_ONLINEFMT, cpu);
		if (filp_open_ok(path))
			rsc_write_int_file(path, want ? 1 : 0);
	}
	rsc_cpu_saved_ok = false;
	rsc_cpu_active = false;
}
static void rsc_cpu_work_fn(struct work_struct *w)
{
	if (rsc_cpu_active) {
		rsc_cpu_restore();
		pr_info(DRV_NAME ": resume_cpus restored\n");
	}
}
int rsc_resume_cpus_apply(const char *spec)
{
	struct cpumask want, cur;
	char buf[64];
	int cpu, changed = 0, keep;
	if (!rsc_cpu_inited)
		return -ENODEV;
	cancel_delayed_work_sync(&rsc_cpu_work);
	if (!spec || !*spec || !strcmp(spec, "0")) {
		rsc_cpu_restore();
		return 0;
	}
	{
		char tmp[32];
		int n = 0;
		const char *q = spec;
		while (*q == ' ')
			q++;
		while (*q >= '0' && *q <= '9' && n < (int)sizeof(tmp) - 1)
			tmp[n++] = *q++;
		while (*q == ' ' || *q == '\n')
			q++;
		tmp[n] = '\0';
		if (!*q && n) {
			int cnt;
			if (!kstrtoint(tmp, 10, &cnt) && cnt > 0) {
				cpumask_clear(&want);
				for (cpu = 0; cpu < cnt && cpu < nr_cpu_ids;
				     cpu++)
					cpumask_set_cpu(cpu, &want);
			} else {
				rsc_mask_parse(spec, &want);
			}
		} else {
			rsc_mask_parse(spec, &want);
		}
	}
	if (cpumask_empty(&want))
		return -EINVAL;
	if (!rsc_cpu_saved_ok) {
		if (!rsc_read_str_file(RSC_CPU_ONLINE, buf, sizeof(buf)))
			rsc_mask_parse(buf, &rsc_cpu_saved);
		rsc_cpu_saved_ok = true;
	}
	keep = rsc_cpu_online_count();
	cpumask_copy(&cur, &rsc_cpu_saved);
	for_each_possible_cpu(cpu) {
		char path[64];
		bool on = cpumask_test_cpu(cpu, &want);
		bool was = cpumask_test_cpu(cpu, &cur);
		if (!cpu || on == was || !was)
			continue;
		if (!on) {
			if (keep <= 1)
				continue;
			keep--;
		}
		scnprintf(path, sizeof(path), RSC_CPU_ONLINEFMT, cpu);
		if (filp_open_ok(path)) {
			rsc_write_int_file(path, on ? 1 : 0);
			changed++;
		}
	}
	rsc_cpu_active = true;
	schedule_delayed_work(&rsc_cpu_work, msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	RSC_LOG(": resume_cpus applied, %d cpu(s) changed\n", changed);
	return 0;
}
bool rsc_resume_cpus_active(void)
{
	return rsc_cpu_active;
}
struct rsc_kswapd_slot {
	pid_t pid;
	struct cpumask mask;
	bool ok;
};
#define RSC_KSWAPD_MAX	8
static struct rsc_kswapd_slot rsc_kswapd[RSC_KSWAPD_MAX];
static bool rsc_kswapd_active;
static bool rsc_kswapd_inited;
static struct delayed_work rsc_kswapd_work;
static void rsc_kswapd_restore(void)
{
	struct task_struct *g, *t;
	int i;
	rcu_read_lock();
	for (i = 0; i < RSC_KSWAPD_MAX; i++) {
		if (!rsc_kswapd[i].ok)
			continue;
		for_each_process_thread(g, t) {
			if (task_pid_vnr(t) != rsc_kswapd[i].pid)
				continue;
			get_task_struct(t);
			rcu_read_unlock();
			set_cpus_allowed_ptr(t, &rsc_kswapd[i].mask);
			put_task_struct(t);
			rcu_read_lock();
			break;
		}
		rsc_kswapd[i].ok = false;
	}
	rcu_read_unlock();
	rsc_kswapd_active = false;
}
static void rsc_kswapd_work_fn(struct work_struct *w)
{
	if (rsc_kswapd_active) {
		rsc_kswapd_restore();
		pr_info(DRV_NAME ": kswapd affinity restored\n");
	}
}
int rsc_kswapd_affinity_apply(const char *spec)
{
	struct cpumask mask;
	struct task_struct *g, *t;
	int n = 0;
	if (!rsc_kswapd_inited)
		return -ENODEV;
	cancel_delayed_work_sync(&rsc_kswapd_work);
	if (!spec || !*spec || !strcmp(spec, "0")) {
		rsc_kswapd_restore();
		return 0;
	}

	{
		bool simple = true;
		const char *q;
		for (q = spec; *q; q++)
			if (*q == '-' || *q == ',')
				simple = false;
		if (simple)
			rsc_little_mask_to_cpumask(&mask);
		else
			rsc_mask_parse(spec, &mask);
	}
	if (cpumask_empty(&mask))
		return -EINVAL;
	n = 0;
	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (strncmp(t->comm, "kswapd", 6))
			continue;
		if (n >= RSC_KSWAPD_MAX)
			break;
		if (!rsc_kswapd[n].ok) {
			cpumask_copy(&rsc_kswapd[n].mask, &t->cpus_mask);
			rsc_kswapd[n].ok = true;
			rsc_kswapd[n].pid = task_pid_vnr(t);
		}
		n++;
	}
	rcu_read_unlock();
	if (!n)
		return -ESRCH;
	{
		int i;
		rcu_read_lock();
		for (i = 0; i < n; i++) {
			for_each_process_thread(g, t) {
				if (task_pid_vnr(t) != rsc_kswapd[i].pid)
					continue;
				get_task_struct(t);
				rcu_read_unlock();
				set_cpus_allowed_ptr(t, &mask);
				put_task_struct(t);
				rcu_read_lock();
				break;
			}
		}
		rcu_read_unlock();
	}
	rsc_kswapd_active = true;
	schedule_delayed_work(&rsc_kswapd_work, msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	RSC_LOG(": kswapd affinity set on %d thread(s)\n", n);
	return 0;
}
bool rsc_kswapd_active_get(void)
{
	return rsc_kswapd_active;
}
#define RSC_PMQOS_DEV	"/dev/cpu_dma_latency"

static struct file *rsc_pmqos_file;
static bool rsc_pmqos_active;
static bool rsc_pmqos_inited;
static struct delayed_work rsc_pmqos_work;
static void rsc_pmqos_release(void)
{
	if (rsc_pmqos_file) {
		filp_close(rsc_pmqos_file, NULL);
		rsc_pmqos_file = NULL;
	}
	rsc_pmqos_active = false;
}
static void rsc_pmqos_work_fn(struct work_struct *w)
{
	if (rsc_pmqos_active) {
		rsc_pmqos_release();
		pr_info(DRV_NAME ": pm_qos released\n");
	}
}
int rsc_pm_qos_apply(int usec)
{
	s32 v = usec;
	loff_t pos = 0;
	if (!rsc_pmqos_inited)
		return -ENODEV;
	cancel_delayed_work_sync(&rsc_pmqos_work);
	if (usec <= 0) {
		rsc_pmqos_release();
		return 0;
	}
	if (!rsc_pmqos_file) {
		rsc_pmqos_file = filp_open(RSC_PMQOS_DEV, O_WRONLY, 0);
		if (IS_ERR(rsc_pmqos_file)) {
			int err = (int)PTR_ERR(rsc_pmqos_file);

			rsc_pmqos_file = NULL;
			return err;
		}
	}
	kernel_write(rsc_pmqos_file, &v, sizeof(v), &pos);
	rsc_pmqos_active = true;
	schedule_delayed_work(&rsc_pmqos_work, msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	RSC_LOG(": pm_qos cpu_dma_latency=%d us\n", usec);
	return 0;
}
bool rsc_pm_qos_active(void)
{
	return rsc_pmqos_active;
}
#define RSC_WMARK_PATH	"/proc/sys/vm/watermark_boost_factor"
#define RSC_WMARK_ON	15000

static bool rsc_memqos_on;
static bool rsc_memqos_wmark;
static bool rsc_memqos_wall;
static bool rsc_wmark_active;
static int rsc_wmark_saved;
static void rsc_wmark_set(bool on)
{
	if (on && !rsc_wmark_active) {
		int cur = rsc_read_int_file(RSC_WMARK_PATH, -1);
		if (cur >= 0) {
			rsc_wmark_saved = cur;
			rsc_write_int_file(RSC_WMARK_PATH, RSC_WMARK_ON);
			rsc_wmark_active = true;
			RSC_LOG(DRV_NAME ": watermark boost on (%d -> %d)\n",
				cur, RSC_WMARK_ON);
		}
	} else if (!on && rsc_wmark_active) {
		rsc_write_int_file(RSC_WMARK_PATH, (u32)rsc_wmark_saved);
		rsc_wmark_active = false;
		RSC_LOG(DRV_NAME ": watermark boost off (%d restored)\n",
			rsc_wmark_saved);
	}
}
int rsc_memqos_apply(int n, const int *v)
{
	if (n == 2)
		return -EINVAL;
	if (n == 1) {
		rsc_memqos_on = v[0] != 0;
	} else if (n >= 3) {
		rsc_memqos_on = v[0] != 0;
		rsc_memqos_wmark = v[1] != 0;
		rsc_memqos_wall = v[2] != 0;
	} else {
		return -EINVAL;
	}
	rsc_wmark_set(rsc_memqos_on && rsc_memqos_wmark);
	return 0;
}
void rsc_memqos_get(bool *on, bool *wmark, bool *wall)
{
	if (on)
		*on = rsc_memqos_on;
	if (wmark)
		*wmark = rsc_memqos_wmark;
	if (wall)
		*wall = rsc_memqos_wall;
}
static bool rsc_ux_on[4];

int rsc_ux_state(int scenario, int state)
{
	if (scenario < 1 || scenario > 3)
		return -EINVAL;
	rsc_ux_on[scenario] = state != 0;
	if (!state) {
		if (!rsc_ux_on[1] && !rsc_ux_on[3])
			rsc_system_boost(0);
		if (!rsc_ux_on[2])
			rsc_topapp_boost(0);
		return 0;
	}
	if (scenario == 1)
		rsc_system_boost(1);
	else if (scenario == 2)
		rsc_topapp_boost(READ_ONCE(rsc_ui_boost_umin));
	else
		rsc_system_boost(2);
	RSC_LOG(DRV_NAME ": ux_state scenario=%d state=%d\n", scenario, state);
	return 0;
}
unsigned int rsc_ux_mask(void)
{
	unsigned int m = 0;
	int i;
	for (i = 1; i <= 3; i++)
		if (rsc_ux_on[i])
			m |= BIT(i);
	return m;
}
static unsigned long rsc_feature_mask;

unsigned long rsc_switch_apply(unsigned long val)
{
	if (val & 0x80000000UL) {
		rsc_feature_mask &= ~(val & ~0x80000000UL);
	} else {
		rsc_feature_mask |= val;
		if (!val)
			rsc_feature_mask = 0;
	}
	rsc_feature_mask &= 0x1ffffUL;
	if (!rsc_feature_mask)
		rsc_clear_all();
	return rsc_feature_mask;
}

unsigned long rsc_switch_mask(void)
{
	return rsc_feature_mask;
}
int rsc_freq_lock_apply(const char *spec)
{
	int pols[RSC_POL_MAX], npol = 0, pol, changed = 0;
	const char *p = spec;
	bool any = false;
	if (!rsc_freq_inited)
		return -ENODEV;
	cancel_delayed_work_sync(&rsc_freq_work);
	for (pol = 0; pol < RSC_POL_MAX; pol++) {
		char path[80];
		scnprintf(path, sizeof(path), RSC_POLICY_MINFMT, pol);
		if (rsc_read_int_file(path, 0) > 0 && npol < 3)
			pols[npol++] = pol;
	}
	while (*p) {
		int cl = 0, khz = 0;
		while (*p == ' ')
			p++;
		if (*p < '0' || *p > '9')
			break;
		while (*p >= '0' && *p <= '9')
			cl = cl * 10 + (*p++ - '0');
		if (*p == ':')
			p++;
		while (*p >= '0' && *p <= '9')
			khz = khz * 10 + (*p++ - '0');
		if (!khz || cl < 0 || cl >= npol)
			continue;
		pol = pols[cl];
		{
			char pmin[80], pmax[80], pcap[80];
			int minf, capf;
			scnprintf(pmin, sizeof(pmin), RSC_POLICY_MINFMT, pol);
			scnprintf(pmax, sizeof(pmax), RSC_POLICY_MAXFMT, pol);
			scnprintf(pcap, sizeof(pcap), RSC_POLICY_CPUFMT, pol);
			capf = rsc_read_int_file(pcap, 0);
			minf = rsc_read_int_file(pmin, 0);
			if (!rsc_freq[pol].ok) {
				if (!rsc_read_str_file(pmax, rsc_freq[pol].saved,
						       sizeof(rsc_freq[pol].saved)))
					rsc_freq[pol].ok = true;
			}
			if (!rsc_freq[pol].ok_min) {
				if (!rsc_read_str_file(pmin,
						      rsc_freq[pol].saved_min,
						      sizeof(rsc_freq[pol].saved_min)))
					rsc_freq[pol].ok_min = true;
			}
			if (capf && khz > capf)
				khz = capf;
			if (minf && khz < minf)
				khz = minf;
			rsc_write_int_file(pmin, khz);
			rsc_write_int_file(pmax, khz);
			changed++;
			any = true;
			RSC_LOG(DRV_NAME ": freq_lock cluster%d (policy%d) = %d kHz\n",
				cl, pol, khz);
		}
	}
	if (!any)
		return -EINVAL;
	rsc_freq_active = true;
	schedule_delayed_work(&rsc_freq_work, msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	return 0;
}
struct rsc_aff_slot {
	pid_t pid;
	struct cpumask mask;
	bool ok;
};
#define RSC_AFF_MAX	16
static struct rsc_aff_slot rsc_aff[RSC_AFF_MAX];
static bool rsc_aff_active;
static bool rsc_aff_inited;
static struct delayed_work rsc_aff_work;

void rsc_prime_mask(struct cpumask *m)
{
	int cpu, best = 0, bestcpu = 0;
	cpumask_clear(m);
	for_each_possible_cpu(cpu) {
		int f = rsc_cpu_maxfreq(cpu);
		if (f >= best) {
			best = f;
			bestcpu = cpu;
		}
	}
	cpumask_set_cpu(bestcpu, m);
}
static void rsc_aff_restore(void)
{
	struct task_struct *g, *t;
	int i;
	rcu_read_lock();
	for (i = 0; i < RSC_AFF_MAX; i++) {
		if (!rsc_aff[i].ok)
			continue;
		for_each_process_thread(g, t) {
			if (task_pid_vnr(t) != rsc_aff[i].pid)
				continue;
			get_task_struct(t);
			rcu_read_unlock();
			set_cpus_allowed_ptr(t, &rsc_aff[i].mask);
			put_task_struct(t);
			rcu_read_lock();
			break;
		}
		rsc_aff[i].ok = false;
	}
	rcu_read_unlock();
	rsc_aff_active = false;
}
static void rsc_aff_work_fn(struct work_struct *w)
{
	if (rsc_aff_active) {
		rsc_aff_restore();
		RSC_LOG(DRV_NAME ": rt_ui_boost_to_prime affinity restored\n");
	}
}
int rsc_rt_affinity_apply(int enable, struct cpumask *target, const int *pids, int n)
{
	int i, done = 0;
	if (!rsc_aff_inited)
		return -ENODEV;
	cancel_delayed_work_sync(&rsc_aff_work);
	if (!enable) {
		rsc_aff_restore();
		return 0;
	}
	if (!pids || n <= 0)
		return -EINVAL;
	rcu_read_lock();
	for (i = 0; i < n && i < RSC_AFF_MAX; i++) {
		struct task_struct *p = find_task_by_vpid(pids[i]);
		if (!p)
			continue;
		get_task_struct(p);
		cpumask_copy(&rsc_aff[i].mask, &p->cpus_mask);
		rsc_aff[i].pid = pids[i];
		rsc_aff[i].ok = true;
		rcu_read_unlock();
		set_cpus_allowed_ptr(p, target);
		put_task_struct(p);
		rcu_read_lock();
		done++;
	}
	rcu_read_unlock();
	if (!done)
		return -ESRCH;
	rsc_aff_active = true;
	schedule_delayed_work(&rsc_aff_work,
			      msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	RSC_LOG(DRV_NAME ": rt_ui_boost_to_prime %d pid(s) on the given mask\n",
		done);
	return 0;
}
bool rsc_rt_affinity_active(void)
{
	return rsc_aff_active;
}
int rsc_vmstat_sum(const char *const *keys, int n, u64 *out)
{
	struct file *f;
	int ci = -1, i;
	char *buf;
	loff_t pos = 0;
	ssize_t r;
	u64 total = 0;
	int found = 0;
	for (i = 0; i < RSC_VMCACHE_N; i++) {
		if (rsc_vmc[i].key0 == keys[0] &&
		    time_before(jiffies, rsc_vmc[i].exp)) {
			*out = rsc_vmc[i].val;
			return 0;
		}
		if (!rsc_vmc[i].key0)
			ci = i;
	}
	buf = kmalloc(16384, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	f = filp_open("/proc/vmstat", O_RDONLY, 0);
	if (IS_ERR(f)) {
		kfree(buf);
		return (int)PTR_ERR(f);
	}
	r = kernel_read(f, buf, 16383, &pos);
	filp_close(f, NULL);
	if (r <= 0) {
		kfree(buf);
		return -EIO;
	}
	buf[r] = '\0';
	for (i = 0; i < n; i++) {
		const char *p = buf;
		size_t klen = strlen(keys[i]);
		while (p && *p) {
			const char *eol = strchr(p, '\n');
			if (!strncmp(p, keys[i], klen) && p[klen] == ' ') {
				const char *v = p + klen;
				char tok[24];
				size_t tl = 0;
				while (*v == ' ' || *v == '\t')
					v++;
				while (*v >= '0' && *v <= '9' &&
				       tl < sizeof(tok) - 1)
					tok[tl++] = *v++;
				tok[tl] = '\0';
				if (tl) {
					u64 val = 0;
					if (!kstrtou64(tok, 10, &val)) {
						total += val;
						found++;
					}
				}
				break;
			}
			if (!eol)
				break;
			p = eol + 1;
		}
	}
	kfree(buf);
	if (!found)
		return -ENOENT;
	*out = total;
	if (ci < 0)
		ci = 0;
	rsc_vmc[ci].key0 = keys[0];
	rsc_vmc[ci].val = total;
	rsc_vmc[ci].exp = jiffies + msecs_to_jiffies(RSC_VMSTAT_TTL_MS);
	return 0;
}
bool rsc_gate_engine = true;
bool rsc_gate_mbd = true;
bool rsc_mbd_enabled(void)
{
	return READ_ONCE(rsc_gate_mbd);
}
int rsc_buddyinfo(u64 *free_pages, u64 *blocks, u64 *suitable)
{
	struct file *f;
	char *buf;
	loff_t pos = 0;
	ssize_t n;
	u64 fp = 0, bl = 0, su = 0;
	char *line, *next;
	if (time_before(jiffies, rsc_buddy_exp)) {
		if (free_pages)
			*free_pages = rsc_buddy_cached[0];
		if (blocks)
			*blocks = rsc_buddy_cached[1];
		if (suitable)
			*suitable = rsc_buddy_cached[2];
		return 0;
	}
	buf = kmalloc(8192, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	f = filp_open("/proc/buddyinfo", O_RDONLY, 0);
	if (IS_ERR(f)) {
		kfree(buf);
		return (int)PTR_ERR(f);
	}
	n = kernel_read(f, buf, 8191, &pos);
	filp_close(f, NULL);
	if (n <= 0) {
		kfree(buf);
		return -EIO;
	}
	buf[n] = '\0';
	for (line = buf; line && *line; line = next) {
		char *p;
		int order = 0;
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		p = strstr(line, "zone");
		if (!p)
			continue;
		p += 4;
		while (*p && (*p < '0' || *p > '9'))
			p++;
		while (*p && order <= 10) {
			char tok[24];
			size_t tl = 0;
			u64 cnt = 0;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p < '0' || *p > '9')
				break;
			while (*p >= '0' && *p <= '9' &&
			       tl < sizeof(tok) - 1)
				tok[tl++] = *p++;
			tok[tl] = '\0';
			if (kstrtou64(tok, 10, &cnt))
				break;
			fp += cnt << order;
			bl += cnt;
			if (order >= 3)
				su += cnt << order;
			order++;
		}
	}
	kfree(buf);
	rsc_buddy_cached[0] = fp;
	rsc_buddy_cached[1] = bl;
	rsc_buddy_cached[2] = su;
	rsc_buddy_exp = jiffies + msecs_to_jiffies(RSC_BUDDY_TTL_MS);
	if (free_pages)
		*free_pages = fp;
	if (blocks)
		*blocks = bl;
	if (suitable)
		*suitable = su;
	return 0;
}
int rsc_cpu_lines(char *buf, size_t size)
{
	int cpu, maxf = 0, freq, used = 0;
	for_each_possible_cpu(cpu) {
		freq = rsc_cpu_maxfreq(cpu);
		if (freq > maxf)
			maxf = freq;
	}
	if (!maxf)
		return sysfs_emit(buf, "0\n");
	for_each_possible_cpu(cpu) {
		char p2[64];
		int mf, cf;
		u32 cap = 0;
		scnprintf(p2, sizeof(p2), RSC_CPU_CURFMT, cpu);
		mf = rsc_cpu_maxfreq(cpu);
		cf = rsc_read_int_file(p2, 0);
		if (mf > 0)
			cap = (u32)((u64)mf * RSC_UCLAMP_MAX / maxf);
		used += scnprintf(buf + used, size - used, "%d %d %d %u\n",
				  cpu, mf, cf, cap);
		if (used >= size - 24)
			break;
	}
	return used;
}
static const char *const rsc_cg_tasks[9] = {
	"/dev/cpuset/l-background/tasks",
	"/dev/cpuset/background/tasks",
	"/dev/cpuset/system-background/tasks",
	"/dev/cpuset/h-background/tasks",
	"/dev/cpuset/l-foreground/tasks",
	"/dev/cpuset/foreground/tasks",
	"/dev/cpuset/top-app/tasks",
	"/dev/cpuset/restricted/tasks",
	"/dev/cpuctl/l-background/tasks",
};
static const char *const rsc_cg_procs[9] = {
	"/dev/cpuset/l-background/cgroup.procs",
	"/dev/cpuset/background/cgroup.procs",
	"/dev/cpuset/system-background/cgroup.procs",
	"/dev/cpuset/h-background/cgroup.procs",
	"/dev/cpuset/l-foreground/cgroup.procs",
	"/dev/cpuset/foreground/cgroup.procs",
	"/dev/cpuset/top-app/cgroup.procs",
	"/dev/cpuset/restricted/cgroup.procs",
	"/dev/cpuctl/l-background/cgroup.procs",
};
static int rsc_write_str_file(const char *path, const char *s)
{
	struct file *f;
	loff_t pos = 0;
	int len;
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return (int)PTR_ERR(f);
	len = strlen(s);
	kernel_write(f, s, len, &pos);
	filp_close(f, NULL);
	return 0;
}
int rsc_cgroup_set(pid_t pid, unsigned int idx, bool procs)
{
	char buf[16];
	const char *path;
	if (pid <= 0 || idx >= ARRAY_SIZE(rsc_cg_tasks))
		return -EINVAL;
	path = procs ? rsc_cg_procs[idx] : rsc_cg_tasks[idx];
	scnprintf(buf, sizeof(buf), "%d", pid);
	RSC_LOG(": cgroup pid=%d -> %s\n", pid, path);
	return rsc_write_str_file(path, buf);
}
#define RSC_MIN_FREE_PATH	"/proc/sys/vm/min_free_kbytes"
#define RSC_MF_MAX_KB		(256 * 1024)

static bool rsc_mf_active;
static bool rsc_mf_inited;
static u32 rsc_mf_base;
static struct delayed_work rsc_mf_work;
static unsigned long rsc_n_mf_boost;
static void rsc_mf_restore_fn(struct work_struct *w)
{
	if (!rsc_mf_active)
		return;
	rsc_write_int_file(RSC_MIN_FREE_PATH, rsc_mf_base);
	rsc_mf_active = false;
	pr_info(DRV_NAME ": min_free_kbytes restored to %u\n", rsc_mf_base);
}
void rsc_min_free_boost(int kb)
{
	u32 target;
	if (!rsc_mf_inited)
		return;
	if (kb <= 0) {
		cancel_delayed_work_sync(&rsc_mf_work);
		if (rsc_mf_active) {
			rsc_write_int_file(RSC_MIN_FREE_PATH, rsc_mf_base);
			rsc_mf_active = false;
		}
		return;
	}
	if (!rsc_mf_active) {
		int cur = rsc_read_int_file(RSC_MIN_FREE_PATH, 0);
		rsc_mf_base = cur > 0 ? (u32)cur : 0;
	}
	target = min_t(u32, rsc_mf_base + (u32)kb, RSC_MF_MAX_KB);
	rsc_write_int_file(RSC_MIN_FREE_PATH, target);
	rsc_mf_active = true;
	rsc_n_mf_boost++;
	mod_delayed_work(system_wq, &rsc_mf_work, msecs_to_jiffies(READ_ONCE(rsc_boost_ms)));
	RSC_LOG(": min_free_kbytes %u -> %u (+%d)\n", rsc_mf_base, target, kb);
}
bool rsc_min_free_boost_active(void)
{
	return rsc_mf_active;
}
unsigned long rsc_task_util(pid_t pid)
{
	struct task_struct *p = rsc_pid_task(pid);
	unsigned long util = 0;
	if (!p)
		return 0;
	util = READ_ONCE(p->se.avg.util_avg);
	put_task_struct(p);
	return util;
}
int rsc_topapp_util(char *buf, size_t len)
{
	pid_t pids[32];
	struct task_struct *p;
	int n, i, used = 0;
	n = rsc_collect_cpuset_pids(RSC_TOPAPP_TASKS, pids, ARRAY_SIZE(pids));
	for (i = 0; i < n && used < len; i++) {
		u64 ns;
		p = rsc_pid_task(pids[i]);
		if (!p)
			continue;
		ns = p->se.sum_exec_runtime;
		used += scnprintf(buf + used, len - used, "%d %lu %llu\n",
				  pids[i], READ_ONCE(p->se.avg.util_avg),
				  ns / NSEC_PER_MSEC);
		put_task_struct(p);
	}
	return used;
}
void rsc_current_boost(int util_min)
{
	rsc_boost_apply(task_pid_vnr(current), clamp(util_min, 0, RSC_UCLAMP_MAX), RSC_UCLAMP_MAX, false);
}
unsigned long rsc_deduped(void)
{
	return rsc_n_deduped;
}

void rsc_engine_stats(unsigned long *applied, unsigned long *reverted, unsigned long *failed, int *active, unsigned long *sysboost)
{
	int i;
	if (applied)
		*applied = rsc_n_applied;
	if (reverted)
		*reverted = rsc_n_reverted;
	if (failed)
		*failed = rsc_n_failed;
	if (sysboost)
		*sysboost = rsc_n_sys_boost;
	if (active) {
		*active = 0;
		mutex_lock(&rsc_engine_lock);
		for (i = 0; i < RSC_MAX_BOOSTS; i++)
			if (rsc_boosts[i].used)
				(*active)++;
		mutex_unlock(&rsc_engine_lock);
	}
}
int rsc_engine_start(void)
{
	INIT_DELAYED_WORK(&rsc_revert_work, rsc_revert_fn);
	rsc_revert_work_inited = true;
	INIT_DELAYED_WORK(&rsc_sys_restore_work, rsc_sys_restore_fn);
	rsc_sys_work_inited = true;
	INIT_DELAYED_WORK(&rsc_mf_work, rsc_mf_restore_fn);
	rsc_mf_inited = true;
	INIT_DELAYED_WORK(&rsc_cpulimit_work, rsc_cpulimit_work_fn);
	rsc_cpulimit_inited = true;
	INIT_DELAYED_WORK(&rsc_freq_work, rsc_freq_work_fn);
	rsc_freq_inited = true;
	INIT_DELAYED_WORK(&rsc_cpu_work, rsc_cpu_work_fn);
	rsc_cpu_inited = true;
	INIT_DELAYED_WORK(&rsc_kswapd_work, rsc_kswapd_work_fn);
	rsc_kswapd_inited = true;
	INIT_DELAYED_WORK(&rsc_pmqos_work, rsc_pmqos_work_fn);
	rsc_pmqos_inited = true;
	INIT_DELAYED_WORK(&rsc_aff_work, rsc_aff_work_fn);
	rsc_aff_inited = true;
	rsc_engine_up = true;
	pr_info(DRV_NAME ": QoS engine up (uclamp + RT + caps, %ums)\n",
		READ_ONCE(rsc_boost_ms));
	return 0;
}
void rsc_engine_stop(void)
{
	if (rsc_revert_work_inited)
		cancel_delayed_work_sync(&rsc_revert_work);
	if (rsc_sys_work_inited)
		cancel_delayed_work_sync(&rsc_sys_restore_work);
	if (rsc_mf_inited)
		cancel_delayed_work_sync(&rsc_mf_work);
	if (rsc_cpulimit_inited) {
		cancel_delayed_work_sync(&rsc_cpulimit_work);
		rsc_cpulimit_restore();
	}
	if (rsc_freq_inited) {
		cancel_delayed_work_sync(&rsc_freq_work);
		rsc_freq_restore();
	}
	if (rsc_cpu_inited) {
		cancel_delayed_work_sync(&rsc_cpu_work);
		rsc_cpu_restore();
	}
	if (rsc_kswapd_inited) {
		cancel_delayed_work_sync(&rsc_kswapd_work);
		rsc_kswapd_restore();
	}
	if (rsc_pmqos_inited) {
		cancel_delayed_work_sync(&rsc_pmqos_work);
		rsc_pmqos_release();
	}
	if (rsc_aff_inited) {
		cancel_delayed_work_sync(&rsc_aff_work);
		rsc_aff_restore();
	}
	rsc_wmark_set(false);
	rsc_engine_up = false;
	rsc_clear_all();
	if (rsc_sys_boost_active) {
		rsc_write_int_file(RSC_CLAMP_MIN_PATH, rsc_sys_boost_saved);
		rsc_sys_boost_active = false;
	}
	if (rsc_mf_active) {
		rsc_write_int_file(RSC_MIN_FREE_PATH, rsc_mf_base);
		rsc_mf_active = false;
	}
}
