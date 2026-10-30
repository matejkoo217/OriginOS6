/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIVO_RSC_H
#define _VIVO_RSC_H
#include <linux/kobject.h>
#include <linux/sched.h>
#include <linux/types.h>

#define DRV_NAME "vivo_rsc"
#define RSC_VAL_LEN 64
#define RSC_MAX_BOOSTS 96
#define RSC_UCLAMP_MAX 1024
#define RSC_RT_PRIO 1
#define RSC_UI_BOOST_MIN 512
#define RSC_RESET_COOKIE	0x053EC600u

extern unsigned int rsc_boost_ms;
extern unsigned int rsc_debug_mask;
extern unsigned int rsc_func_mask;
extern unsigned int rsc_ui_boost_umin;
extern unsigned int rsc_sys_boost_step;
extern bool rsc_gate_qos;
extern bool rsc_gate_svp;
extern bool rsc_gate_engine;
extern bool rsc_gate_mbd;

bool rsc_log_enabled(void);
void rsc_engine_stop(void);
int rsc_engine_start(void);
int rsc_parse_ints(const char *buf, int *out, int max);
int rsc_read_int_file(const char *path, int def);
int rsc_write_int_file(const char *path, u32 val);
int rsc_cpuset_first_pid(const char *path);
int rsc_collect_cpuset_pids(const char *path, pid_t *out, int max);
struct task_struct *rsc_pid_task(pid_t pid);
int rsc_boost_apply(pid_t pid, u32 umin, u32 umax, bool rt);
int rsc_boost_apply_cap(pid_t pid, u32 umax);
void rsc_caps_clear(void);
void rsc_clear_all(void);
void rsc_system_boost(int level);
int rsc_cgroup_set(pid_t pid, unsigned int idx, bool procs);
void rsc_min_free_boost(int kb);
bool rsc_min_free_boost_active(void);
unsigned long rsc_task_util(pid_t pid);
int rsc_topapp_util(char *buf, size_t len);
int rsc_cpulimit_scene(int scene);
bool rsc_cpulimit_active(void);
int rsc_cpulimit_scene_active(void);
int rsc_vmstat_sum(const char *const *keys, int n, u64 *out);
int rsc_freq_max_apply(u32 khz);
bool rsc_freq_max_active(void);
int rsc_resume_cpus_apply(const char *spec);
bool rsc_resume_cpus_active(void);
int rsc_kswapd_affinity_apply(const char *spec);
bool rsc_kswapd_active_get(void);
int rsc_pm_qos_apply(int usec);
int rsc_ux_state(int scenario, int state);
unsigned int rsc_ux_mask(void);
unsigned long rsc_switch_apply(unsigned long val);
unsigned long rsc_switch_mask(void);
int rsc_freq_lock_apply(const char *spec);
int rsc_rt_affinity_apply(int enable, struct cpumask *target, const int *pids, int n);
bool rsc_rt_affinity_active(void);
void rsc_prime_mask(struct cpumask *m);
void rsc_mask_parse(const char *s, struct cpumask *m);
int rsc_memqos_apply(int n, const int *v);
void rsc_memqos_get(bool *on, bool *wmark, bool *wall);
bool rsc_pm_qos_active(void);
int rsc_buddyinfo(u64 *free_pages, u64 *blocks, u64 *suitable);
int rsc_cpu_lines(char *buf, size_t size);
int rsc_cpu_maxfreq(int cpu);
unsigned long rsc_deduped(void);
bool rsc_mbd_enabled(void);
bool rsc_system_boost_active(void);
int rsc_topapp_boost(u32 umin);
void rsc_current_boost(int util_min);
void rsc_engine_stats(unsigned long *applied, unsigned long *reverted, unsigned long *failed, int *active, unsigned long *sysboost);

extern struct kobj_attribute ktop_attr;
extern struct kobj_attribute ktop_fpid_attr;
extern struct kobj_attribute cpucap_notif_attr;
extern struct kobj_attribute cpucap_notif_debug_attr;
extern struct kobj_attribute cpucap_notif_detail_attr;
extern struct kobj_attribute sched_boost_attr;
extern struct kobj_attribute rsc_sched_boost_ctrl_attr;
extern struct kobj_attribute sched_stat_attr;
extern struct kobj_attribute rsc_svp_attr;
extern struct kobj_attribute rsc_boost_attr;
extern struct kobj_attribute set_task_qos_attr;
extern struct kobj_attribute get_task_qos_attr;
extern struct kobj_attribute qos_enable_attr;
extern struct kobj_attribute qos_stat_attr;
extern struct kobj_attribute launcher_boost_rt_attr;
extern struct kobj_attribute cpulimit_attr;
extern struct kobj_attribute cpulimit_uid_attr;
extern struct kobj_attribute svp_ui_boost_attr;
extern struct kobj_attribute ex_boost_attr;
extern struct kobj_attribute fp_hint_attr;
extern struct kobj_attribute memory_info_attr;
extern struct kobj_attribute rsc_cgroup_attr;
extern struct kobj_attribute min_free_kbytes_boost_attr;
extern struct kobj_attribute cpu_usage_attr;
extern struct kobj_attribute refault_readable_attr;
extern struct kobj_attribute slowpath_readable_attr;
extern struct kobj_attribute rsc_cpu_status_attr;
extern struct kobj_attribute enabled_attr;
extern struct kobj_attribute mbd_enable_attr;
extern struct kobj_attribute freq_max_attr;
extern struct kobj_attribute resume_cpus_attr;
extern struct kobj_attribute kswapd_affinity_limit_attr;
extern struct kobj_attribute pm_qos_attr;
extern struct kobj_attribute debug_attr;
extern struct kobj_attribute func_debug_attr;
extern struct kobj_attribute ux_state_attr;
extern struct kobj_attribute rsc_switch_attr;
extern struct kobj_attribute fla_freq_lock_attr;
extern struct kobj_attribute rt_ui_boost_to_prime_attr;
extern struct kobj_attribute memqos_param_attr;
extern struct kobj_attribute refault_attr;
extern struct kobj_attribute slowpath_attr;
#endif
