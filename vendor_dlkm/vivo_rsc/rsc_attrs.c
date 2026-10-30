// SPDX-License-Identifier: GPL-2.0-only
// MADE FOR XIAOMI 13 5.15 A13 KERNEL!
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include <linux/uidgid.h>
#include <uapi/linux/sched/types.h>
#include "rsc.h"

MODULE_IMPORT_NS(MINIDUMP);

#define RSC_TOPAPP_TASKS	"/dev/cpuset/top-app/tasks"
#define RSC_CPU_MAXFMT		"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq"

extern unsigned long uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id);
static char rsc_last_qos[RSC_VAL_LEN] = "0";
static char rsc_last_get[RSC_VAL_LEN] = "0";

static ssize_t ktop_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct file *f;
	char tmp[512];
	loff_t pos = 0;
	ssize_t n;
	int cnt = 0, i;
	f = filp_open(RSC_TOPAPP_TASKS, O_RDONLY, 0);
	if (IS_ERR(f))
		return sysfs_emit(buf, "0\n");
	while ((n = kernel_read(f, tmp, sizeof(tmp), &pos)) > 0)
		for (i = 0; i < n; i++)
			if (tmp[i] == '\n')
				cnt++;
	filp_close(f, NULL);
	return sysfs_emit(buf, "%d\n", cnt);
}
struct kobj_attribute ktop_attr = __ATTR_RO(ktop);

static ssize_t ktop_fpid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", rsc_cpuset_first_pid(RSC_TOPAPP_TASKS));
}

static ssize_t ktop_fpid_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	return len;
}
struct kobj_attribute ktop_fpid_attr = __ATTR(ktop_fpid, 0660, ktop_fpid_show, ktop_fpid_store);

static ssize_t cpucap_notif_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int cpu, maxf = 0, freq, len = 0;
	for_each_possible_cpu(cpu) {
		freq = rsc_cpu_maxfreq(cpu);
		if (freq > maxf)
			maxf = freq;
	}
	if (!maxf)
		return sysfs_emit(buf, "0\n");
	for_each_possible_cpu(cpu) {
		u32 cap = 0;
		freq = rsc_cpu_maxfreq(cpu);
		if (freq > 0)
			cap = (u32)((u64)freq * RSC_UCLAMP_MAX / maxf);
		len += scnprintf(buf + len, PAGE_SIZE - len, "%d:%u ", cpu, cap);
		if (len >= PAGE_SIZE - 16)
			break;
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}
static ssize_t cpucap_notif_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	return len;
}
struct kobj_attribute cpucap_notif_attr = __ATTR(cpucap_notif, 0444, cpucap_notif_show, cpucap_notif_store);
struct kobj_attribute cpucap_notif_debug_attr = __ATTR(cpucap_notif_debug, 0660, cpucap_notif_show, cpucap_notif_store);
struct kobj_attribute cpucap_notif_detail_attr = __ATTR(cpucap_notif_detail, 0660, cpucap_notif_show, cpucap_notif_store);

static ssize_t sched_boost_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", rsc_system_boost_active() ? 1 : 0);
}
static ssize_t sched_boost_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v))
		rsc_system_boost(v);
	return len;
}
struct kobj_attribute sched_boost_attr = __ATTR(sched_boost, 0660, sched_boost_show, sched_boost_store);
struct kobj_attribute rsc_sched_boost_ctrl_attr = __ATTR(rsc_sched_boost_ctrl, 0660, sched_boost_show, sched_boost_store);

static ssize_t sched_stat_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned long applied, reverted, failed;
	rsc_engine_stats(&applied, &reverted, &failed, NULL, NULL);
	return sysfs_emit(buf, "applied %lu reverted %lu failed %lu deduped %lu\n",
			  applied, reverted, failed, rsc_deduped());
}

static ssize_t stat_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	return len;
}
struct kobj_attribute sched_stat_attr = __ATTR(sched_stat, 0660, sched_stat_show, stat_store);

static ssize_t gate_qos_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(rsc_gate_qos));
}

static ssize_t gate_qos_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v)) {
		WRITE_ONCE(rsc_gate_qos, v != 0);
		if (!v)
			rsc_clear_all();
	}
	return len;
}
struct kobj_attribute qos_enable_attr = __ATTR(qos_enable, 0660, gate_qos_show, gate_qos_store);

static ssize_t gate_svp_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(rsc_gate_svp));
}

static ssize_t gate_svp_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v))
		WRITE_ONCE(rsc_gate_svp, v != 0);
	return len;
}
struct kobj_attribute rsc_svp_attr = __ATTR(rsc_svp, 0660, gate_svp_show, gate_svp_store);
struct kobj_attribute rsc_boost_attr = __ATTR(rsc_boost, 0660, gate_svp_show, gate_svp_store);

static ssize_t set_task_qos_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", rsc_last_qos);
}

static ssize_t set_task_qos_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int a[4];
	int n = rsc_parse_ints(buf, a, 4);
	if (n < 1)
		return len;
	snprintf(rsc_last_qos, sizeof(rsc_last_qos), "%d %d %d %d", a[0], a[1],
		 n >= 3 ? a[2] : RSC_UCLAMP_MAX, n >= 4 ? a[3] : 1);
	if (n >= 4 && a[3] == 0)
		rsc_boost_apply(a[0], 0, RSC_UCLAMP_MAX, false);
	else
		rsc_boost_apply(a[0], clamp(a[1], 0, RSC_UCLAMP_MAX),
				RSC_UCLAMP_MAX, false);
	return len;
}
struct kobj_attribute set_task_qos_attr = __ATTR(set_task_qos, 0660, set_task_qos_show, set_task_qos_store);

static ssize_t get_task_qos_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", rsc_last_get);
}

static ssize_t get_task_qos_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	struct task_struct *p;
	int a[2];
	int n = rsc_parse_ints(buf, a, 2);
	if (n < 1)
		return len;
	p = rsc_pid_task(a[0]);
	if (!p)
		return len;
	snprintf(rsc_last_get, sizeof(rsc_last_get), "%d %lu %lu", a[0],
		 uclamp_eff_value(p, UCLAMP_MIN),
		 uclamp_eff_value(p, UCLAMP_MAX));
	put_task_struct(p);
	return len;
}
struct kobj_attribute get_task_qos_attr = __ATTR(get_task_qos, 0660, get_task_qos_show, get_task_qos_store);

static ssize_t qos_stat_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned long applied, reverted, failed, sysboost;
	int active;
	rsc_engine_stats(&applied, &reverted, &failed, &active, &sysboost);
	return sysfs_emit(buf,
			  "applied %lu reverted %lu failed %lu deduped %lu active %d sysboost %lu\n",
			  applied, reverted, failed, rsc_deduped(), active,
			  sysboost);
}
struct kobj_attribute qos_stat_attr = __ATTR(qos_stat, 0660, qos_stat_show, stat_store);

static ssize_t launcher_boost_rt_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", rsc_last_qos);
}

static ssize_t launcher_boost_rt_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int a[2];
	int n = rsc_parse_ints(buf, a, 2);
	if (n < 2)
		return len;
	snprintf(rsc_last_qos, sizeof(rsc_last_qos), "%d %d", a[0], a[1]);
	if (a[1])
		rsc_boost_apply(a[0], RSC_UCLAMP_MAX, RSC_UCLAMP_MAX, true);
	else
		rsc_boost_apply(a[0], 0, RSC_UCLAMP_MAX, false);
	return len;
}
struct kobj_attribute launcher_boost_rt_attr = __ATTR(launcher_boost_rt, 0660, launcher_boost_rt_show, launcher_boost_rt_store);
#define RSC_CPULIMIT_MAGIC	0x053EC600u
static char rsc_last_cpulimit[RSC_VAL_LEN] = "0";
static ssize_t cpulimit_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", rsc_last_cpulimit);
}

static ssize_t cpulimit_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int a[10];
	int n = rsc_parse_ints(buf, a, 10);
	snprintf(rsc_last_cpulimit, sizeof(rsc_last_cpulimit), "%d", n >= 1 ? a[0] : 0);
	if (n == 1) {
		if ((u32)a[0] < RSC_CPULIMIT_MAGIC ||
		    (u32)a[0] - RSC_CPULIMIT_MAGIC > 6)
			return len;
		rsc_cpulimit_scene((u32)a[0] - RSC_CPULIMIT_MAGIC);
	} else if ((n == 2 || n == 4) && (u32)a[0] == RSC_CPULIMIT_MAGIC) {
		rsc_cpulimit_scene(a[1] ? a[1] : 0);
	} else if (n >= 3 && (u32)a[0] != RSC_CPULIMIT_MAGIC) {
		/* 10-int full form: keep it visible in dmesg, don't guess */
		pr_info_ratelimited(DRV_NAME ": cpulimit full form (%d values) not mapped\n",
				    n);
	}
	return len;
}
struct kobj_attribute cpulimit_attr = __ATTR(cpulimit, 0660, cpulimit_show, cpulimit_store);

static ssize_t cpulimit_uid_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	struct task_struct *g, *t;
	uid_t uid;
	u32 cap;
	int a[2];
	int n = rsc_parse_ints(buf, a, 2);
	if (n < 2)
		return len;
	uid = (uid_t)a[0];
	cap = clamp(a[1], 0, RSC_UCLAMP_MAX);
	if (cap >= RSC_UCLAMP_MAX) {
		rsc_caps_clear();
		return len;
	}
	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (from_kuid(&init_user_ns, task_uid(t)) != uid)
			continue;
		rsc_boost_apply_cap(task_pid_vnr(t), cap);
	}
	rcu_read_unlock();
	return len;
}
struct kobj_attribute cpulimit_uid_attr = __ATTR(cpulimit_uid, 0660, cpulimit_show, cpulimit_uid_store);

static ssize_t ui_boost_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static ssize_t ui_boost_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v))
		rsc_topapp_boost(v > 0 ? READ_ONCE(rsc_ui_boost_umin) : 0);
	return len;
}
struct kobj_attribute svp_ui_boost_attr = __ATTR(svp_ui_boost, 0660, ui_boost_show, ui_boost_store);
struct kobj_attribute ex_boost_attr = __ATTR(ex_boost, 0660, ui_boost_show, ui_boost_store);
struct kobj_attribute fp_hint_attr = __ATTR(fp_hint, 0660, ui_boost_show, ui_boost_store);

static ssize_t rsc_cpu_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return rsc_cpu_lines(buf, PAGE_SIZE);
}
struct kobj_attribute rsc_cpu_status_attr = __ATTR(rsc_cpu_status, 0440, rsc_cpu_status_show, NULL);

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(rsc_gate_engine));
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v)) {
		WRITE_ONCE(rsc_gate_engine, v != 0);
		if (!v)
			rsc_clear_all();
	}
	return len;
}
struct kobj_attribute enabled_attr = __ATTR(enabled, 0660, enabled_show, enabled_store);

static ssize_t mbd_enable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(rsc_gate_mbd));
}
static ssize_t mbd_enable_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v))
		WRITE_ONCE(rsc_gate_mbd, v != 0);
	return len;
}
struct kobj_attribute mbd_enable_attr = __ATTR(mbd_enable, 0644, mbd_enable_show, mbd_enable_store);

static ssize_t refault_readable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	static const char *const keys[] = {
		"workingset_refault_anon",
		"workingset_refault_file",
	};
	u64 v = 0;

	if (!rsc_mbd_enabled() || rsc_vmstat_sum(keys, ARRAY_SIZE(keys), &v))
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%llu\n", v);
}
static ssize_t slowpath_readable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	static const char *const keys[] = {
		"allocstall_normal",
		"allocstall_movable",
	};
	u64 v = 0;

	if (!rsc_mbd_enabled() || rsc_vmstat_sum(keys, ARRAY_SIZE(keys), &v))
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%llu\n", v);
}
static ssize_t mbd_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	return len;
}
struct kobj_attribute refault_readable_attr = __ATTR(refault_readable, 0644, refault_readable_show, mbd_store);
struct kobj_attribute slowpath_readable_attr = __ATTR(slowpath_readable, 0644, slowpath_readable_show, mbd_store);

static ssize_t freq_max_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", rsc_freq_max_active() ? 1 : 0);
}
static ssize_t freq_max_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v))
		rsc_freq_max_apply(v > 0 ? (u32)v : 0);
	return len;
}
struct kobj_attribute freq_max_attr = __ATTR(freq_max, 0660, freq_max_show, freq_max_store);

static ssize_t resume_cpus_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", rsc_resume_cpus_active() ? 1 : 0);
}
static ssize_t resume_cpus_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	char spec[64];
	snprintf(spec, sizeof(spec), "%s", buf);
	strim(spec);
	rsc_resume_cpus_apply(spec);
	return len;
}
struct kobj_attribute resume_cpus_attr = __ATTR(resume_cpus, 0660, resume_cpus_show, resume_cpus_store);

static ssize_t kswapd_affinity_limit_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", rsc_kswapd_active_get() ? 1 : 0);
}
static ssize_t kswapd_affinity_limit_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	char spec[64];
	snprintf(spec, sizeof(spec), "%s", buf);
	strim(spec);
	rsc_kswapd_affinity_apply(spec);
	return len;
}
struct kobj_attribute kswapd_affinity_limit_attr = __ATTR(kswapd_affinity_limit, 0660, kswapd_affinity_limit_show, kswapd_affinity_limit_store);

static ssize_t pm_qos_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", rsc_pm_qos_active() ? 1 : 0);
}
static ssize_t pm_qos_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v))
		rsc_pm_qos_apply(v);
	return len;
}
struct kobj_attribute pm_qos_attr = __ATTR(pm_qos, 0660, pm_qos_show, pm_qos_store);

static ssize_t debug_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "debug: %x\n", READ_ONCE(rsc_debug_mask));
}
static ssize_t debug_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	unsigned int v;
	if (!kstrtouint(buf, 0, &v))
		WRITE_ONCE(rsc_debug_mask, v);
	return len;
}
struct kobj_attribute debug_attr = __ATTR(debug, 0660, debug_show, debug_store);

static const char *const rsc_func_names[] = {
	"set_task_qos", "svp_ui_boost", "ex_boost", "fp_hint",
	"launcher_boost_rt", "cpulimit", "cpulimit_uid", "freq_max",
	"fla_freq_lock", "resume_cpus", "kswapd", "pm_qos", "sched_boost",
	"ux_state", "memqos", "cgroup",
};
static ssize_t func_debug_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "rsc func lv debug mask = 0x%x\n",
			  READ_ONCE(rsc_func_mask));
}
static ssize_t func_debug_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	char name[32];
	unsigned int v;
	int i;
	snprintf(name, sizeof(name), "%s", buf);
	strim(name);
	if (!kstrtouint(name, 0, &v)) {
		WRITE_ONCE(rsc_func_mask, v);
		return len;
	}
	for (i = 0; i < ARRAY_SIZE(rsc_func_names); i++) {
		if (!strcmp(name, rsc_func_names[i])) {
			WRITE_ONCE(rsc_func_mask,
				   READ_ONCE(rsc_func_mask) | BIT(i));
			return len;
		}
	}
	return len;
}
struct kobj_attribute func_debug_attr = __ATTR(func_debug, 0660, func_debug_show, func_debug_store);

static ssize_t ux_state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", rsc_ux_mask());
}
static ssize_t ux_state_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int a[2];

	if (rsc_parse_ints(buf, a, 2) < 2)
		return len;
	rsc_ux_state(a[0], a[1]);
	return len;
}
struct kobj_attribute ux_state_attr = __ATTR(ux_state, 0660, ux_state_show, ux_state_store);
static ssize_t rsc_switch_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lx\n", rsc_switch_mask());
}
static ssize_t rsc_switch_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	unsigned long v;

	if (!kstrtoul(buf, 0, &v))
		rsc_switch_apply(v);
	return len;
}
struct kobj_attribute rsc_switch_attr = __ATTR(rsc_switch, 0660, rsc_switch_show, rsc_switch_store);

static char rsc_last_freq_lock[RSC_VAL_LEN] = "0";
static ssize_t fla_freq_lock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "lockfreq: %s\n", rsc_last_freq_lock);
}
static ssize_t fla_freq_lock_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	char spec[RSC_VAL_LEN];
	snprintf(spec, sizeof(spec), "%s", buf);
	strim(spec);
	snprintf(rsc_last_freq_lock, sizeof(rsc_last_freq_lock), "%s", spec);
	rsc_freq_lock_apply(spec);
	return len;
}
struct kobj_attribute fla_freq_lock_attr = __ATTR(fla_freq_lock, 0660, fla_freq_lock_show, fla_freq_lock_store);

static ssize_t rt_ui_boost_to_prime_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", rsc_rt_affinity_active() ? 1 : 0);
}
static ssize_t rt_ui_boost_to_prime_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	struct cpumask mask;
	char tok[24];
	const char *p = buf;
	int pids[16];
	int enable = 0, count = 0, n = 0, i;
	size_t tl = 0;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p >= '0' && *p <= '9')
		enable = *p++ - '0';
	while (*p == ' ' || *p == '\t')
		p++;
	while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
	       tl < sizeof(tok) - 1)
		tok[tl++] = *p++;
	tok[tl] = '\0';
	while (*p == ' ' || *p == '\t')
		p++;
	while (*p >= '0' && *p <= '9')
		count = count * 10 + (*p++ - '0');
	for (i = 0; i < count && i < (int)ARRAY_SIZE(pids); i++) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p < '0' || *p > '9')
			break;
		pids[i] = 0;
		while (*p >= '0' && *p <= '9')
			pids[i] = pids[i] * 10 + (*p++ - '0');
		n++;
	}
	rsc_mask_parse(tok, &mask);
	if (cpumask_empty(&mask)) {
		unsigned long v;
		if (!kstrtoul(tok, 16, &v) && v) {
			for (i = 0; i < nr_cpu_ids; i++)
				if (v & BIT(i))
					cpumask_set_cpu(i, &mask);
		}
	}
	if (cpumask_empty(&mask))
		rsc_prime_mask(&mask);
	rsc_rt_affinity_apply(enable, &mask, pids, n);
	return len;
}
struct kobj_attribute rt_ui_boost_to_prime_attr = __ATTR(rt_ui_boost_to_prime, 0660, rt_ui_boost_to_prime_show, rt_ui_boost_to_prime_store);

static ssize_t memqos_param_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	bool on, wmark, wall;
	rsc_memqos_get(&on, &wmark, &wall);
	return sysfs_emit(buf,
		"memqos_enabled %s, memqos_wmark_enabled %s, memqos_wall_enabled %s. "
		"slowpath max cnt: 0 qos low try cnt: 0 qos middle try cnt: 0 \n",
		on ? "enabled" : "disabled",
		wmark ? "enabled" : "disabled",
		wall ? "enabled" : "disabled");
}
static ssize_t memqos_param_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int a[3];
	int n = rsc_parse_ints(buf, a, 3);
	if (n <= 0)
		return len;
	rsc_memqos_apply(n, a);
	return len;
}
struct kobj_attribute memqos_param_attr = __ATTR(memqos_param, 0644, memqos_param_show, memqos_param_store);

static ssize_t refault_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	static const char *const k_anon[] = { "workingset_refault_anon" };
	static const char *const k_file[] = { "workingset_refault_file" };
	u64 anon = 0, file = 0;
	rsc_vmstat_sum(k_anon, 1, &anon);
	rsc_vmstat_sum(k_file, 1, &file);
	return sysfs_emit(buf, "3.0.0 2 %llu %llu\n", anon, file);
}
static ssize_t slowpath_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	static const char *const k_norm[] = { "allocstall_normal" };
	static const char *const k_move[] = { "allocstall_movable" };
	u64 norm = 0, move = 0;
	rsc_vmstat_sum(k_norm, 1, &norm);
	rsc_vmstat_sum(k_move, 1, &move);
	return sysfs_emit(buf, "3.0.0 2 %llu %llu\n", norm, move);
}
struct kobj_attribute refault_attr = __ATTR_RO(refault);
struct kobj_attribute slowpath_attr = __ATTR_RO(slowpath);

static char rsc_last_cg[RSC_VAL_LEN] = "0";
static ssize_t rsc_cgroup_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", rsc_last_cg);
}
static ssize_t rsc_cgroup_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int a[3];
	int n = rsc_parse_ints(buf, a, 3);

	if (n < 3)
		return len;
	snprintf(rsc_last_cg, sizeof(rsc_last_cg), "%d %d %d", a[0], a[1],
		 a[2]);
	rsc_cgroup_set(a[0], a[1], a[2] != 0);
	return len;
}
struct kobj_attribute rsc_cgroup_attr = __ATTR(rsc_cgroup, 0660, rsc_cgroup_show, rsc_cgroup_store);

static ssize_t min_free_kbytes_boost_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", rsc_min_free_boost_active() ? 1 : 0);
}
static ssize_t min_free_kbytes_boost_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v))
		rsc_min_free_boost(v);
	return len;
}
struct kobj_attribute min_free_kbytes_boost_attr = __ATTR(min_free_kbytes_boost, 0644, min_free_kbytes_boost_show, min_free_kbytes_boost_store);

static bool rsc_cpu_usage_on = true;
static ssize_t cpu_usage_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	if (!rsc_cpu_usage_on)
		return sysfs_emit(buf, "disabled\n");
	return rsc_topapp_util(buf, PAGE_SIZE);
}
static ssize_t cpu_usage_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t len)
{
	int v;
	if (!kstrtoint(buf, 0, &v))
		rsc_cpu_usage_on = v != 0;
	return len;
}
struct kobj_attribute cpu_usage_attr = __ATTR(cpu_usage, 0660, cpu_usage_show, cpu_usage_store);

static ssize_t memory_info_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct sysinfo si = {};
	si_meminfo(&si);
	si_swapinfo(&si);
	return sysfs_emit(buf,
		"mem_total=%llu kB\nmem_free=%llu kB\nmem_shared=%llu kB\n"
		"mem_buffers=%llu kB\nswap_total=%llu kB\nswap_free=%llu kB\n",
		(u64)si.totalram << (PAGE_SHIFT - 10),
		(u64)si.freeram << (PAGE_SHIFT - 10),
		(u64)si.sharedram << (PAGE_SHIFT - 10),
		(u64)si.bufferram << (PAGE_SHIFT - 10),
		(u64)si.totalswap << (PAGE_SHIFT - 10),
		(u64)si.freeswap << (PAGE_SHIFT - 10));
}
struct kobj_attribute memory_info_attr = __ATTR_RO(memory_info);
//phew this is lot , but nothing compared to engine... 🐥
