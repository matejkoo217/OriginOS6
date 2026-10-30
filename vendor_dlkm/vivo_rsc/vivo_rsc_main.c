// SPDX-License-Identifier: GPL-2.0-only
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/namei.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/uidgid.h>
#include "rsc.h"

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_IMPORT_NS(MINIDUMP);

#define RSC_SYSTEM_UID	1000
#define RSC_SYSTEM_GID	1000
#define RSC_PROC_ENTRIES 6

bool rsc_gate_qos = true;
bool rsc_gate_svp = true;

static struct kobject *rsc_kobj;
static struct kobject *rsc_svp_kobj;
static struct kobject *rsc_fas_kobj;
static struct kobject *rsc_fas_gpu_kobj;
static struct kobject *rsc_fas_mem_kobj;
static struct kobject *rsc_afs_kobj;
static struct kobject *rsc_qos_kobj;
static struct kobject *rsc_mbd_kobj;
static struct kobject *rsc_memqos_kobj;
static struct kobject *rsc_sys_heap_kobj;
static struct kobject *rsc_sys_heap_charge_kobj;
static struct kobject *rsc_vgarph_kobj;
static struct proc_dir_entry *vivo_rsc_dir;
static struct proc_dir_entry *rsc_proc_entries[RSC_PROC_ENTRIES];
static const char *const rsc_proc_names[RSC_PROC_ENTRIES] = {
	"frame_set_priority",
	"binder_block_query",
	"binder_boost_desc",
	"check_lmk",
	"mem_blocks_mb",
	"mem_antifrag",
};
bool rsc_permissive = true;
module_param(rsc_permissive, bool, 0644);
MODULE_PARM_DESC(rsc_permissive, "world-writable (0666) nodes (default 1); 0 = strict 0660 root:system");
#define RSC_SHOW_INERT(_name)						\
static ssize_t _name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "0\n");					\
}
#define RSC_STORE_INERT(_name)						\
static ssize_t _name##_store(struct kobject *kobj,			\
			     struct kobj_attribute *attr,		\
			     const char *buf, size_t len)		\
{									\
	return len;							\
}
#define RSC_ATTR_ECHO(_name)						\
static char _name##_val[RSC_VAL_LEN];					\
static ssize_t _name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	if (!_name##_val[0])						\
		return sysfs_emit(buf, "0\n");				\
	return sysfs_emit(buf, "%s\n", _name##_val);			\
}									\
static ssize_t _name##_store(struct kobject *kobj,			\
			     struct kobj_attribute *attr,		\
			     const char *buf, size_t len)		\
{									\
	size_t n = min(len, sizeof(_name##_val) - 1);			\
									\
	memcpy(_name##_val, buf, n);					\
	_name##_val[n] = '\0';						\
	strim(_name##_val);						\
	return len;							\
}									\
static struct kobj_attribute _name##_attr =				\
	__ATTR(_name, 0660, _name##_show, _name##_store)

#define RSC_ATTR_RW(_name)						\
RSC_SHOW_INERT(_name)							\
RSC_STORE_INERT(_name)							\
static struct kobj_attribute _name##_attr =				\
	__ATTR(_name, 0660, _name##_show, _name##_store)
#define RSC_ATTR_RW_644(_name)						\
RSC_SHOW_INERT(_name)							\
RSC_STORE_INERT(_name)							\
static struct kobj_attribute _name##_attr =				\
	__ATTR(_name, 0644, _name##_show, _name##_store)

#define RSC_ATTR_RO(_name)						\
RSC_SHOW_INERT(_name)							\
static struct kobj_attribute _name##_attr =				\
	__ATTR(_name, 0444, _name##_show, NULL)

#define RSC_ATTR_RO_G(_name)						\
RSC_SHOW_INERT(_name)							\
static struct kobj_attribute _name##_attr =				\
	__ATTR(_name, 0440, _name##_show, NULL)
RSC_ATTR_ECHO(ktop_latency);
RSC_ATTR_ECHO(ktop_percent);
RSC_ATTR_ECHO(ktop_set);
RSC_ATTR_RW(binder_cmd);
RSC_ATTR_RW(binder_event);
RSC_ATTR_RW(binder_frozen_callback);
RSC_ATTR_RW(binder_frozen_callback_cmd);
RSC_ATTR_RW(get_fork_pid);
RSC_ATTR_RW(get_specific_tid);
RSC_ATTR_RW(launch);
RSC_ATTR_RW(frame_qos_list);
RSC_ATTR_RW(rsc_gpu_freq_break);
RSC_ATTR_ECHO(vaudit_switch);
RSC_ATTR_ECHO(vaudit_syscheck);
RSC_ATTR_RW(vaudit);
RSC_ATTR_RW(appboost_late);
RSC_ATTR_RW(rsc_trace_enable);
RSC_ATTR_RW(rsc_dyn_svp_enable);
RSC_ATTR_RW(cpu_multi_benchmack);
RSC_ATTR_RW(sort_util_pid);
RSC_ATTR_RW(support_vboostsdk);
RSC_ATTR_RW(deplist_waker);
RSC_ATTR_RW(fla_comm_launch);
RSC_ATTR_RW(fla_echo_break_freq_enable);
RSC_ATTR_RW(fla_support_non);
RSC_ATTR_RW(enable);
RSC_ATTR_RW_644(higqos_slowpath_timeout);
RSC_ATTR_RW_644(lowqos_slowpath_tcnt);
RSC_ATTR_RW_644(memqos_debug_enable);
RSC_ATTR_RW_644(memqos_filemap_fault_trace_enabled);
RSC_ATTR_RW_644(memqos_mb_pg_pool_maxpages);
RSC_ATTR_RW_644(memqos_page_pool);
RSC_ATTR_RW_644(memqos_page_pool_enabled);
RSC_ATTR_RW_644(memqos_rc_pg_pool_maxpages);
RSC_ATTR_RW_644(memqos_slowpath_trace_enabled);
RSC_ATTR_RW_644(memqos_unmb_pg_pool_maxpages);
RSC_ATTR_RW_644(memqos_wait_time);
RSC_ATTR_RW_644(midqos_slowpath_tcnt);
RSC_ATTR_RW_644(slowpath_max_tcnt);
RSC_ATTR_RW_644(slowpath_trace_time_limit);
RSC_ATTR_RW(sys_heap_pools_charge);
RSC_ATTR_RW(ray_tracing);

static struct attribute *rsc_root_attrs[] = {
	&ktop_fpid_attr.attr,
	&ktop_latency_attr.attr,
	&ktop_percent_attr.attr,
	&ktop_set_attr.attr,
	&cpucap_notif_attr.attr,
	&cpucap_notif_debug_attr.attr,
	&cpucap_notif_detail_attr.attr,
	&binder_cmd_attr.attr,
	&binder_event_attr.attr,
	&binder_frozen_callback_attr.attr,
	&binder_frozen_callback_cmd_attr.attr,
	&get_fork_pid_attr.attr,
	&get_specific_tid_attr.attr,
	&launch_attr.attr,
	&frame_qos_list_attr.attr,
	&rsc_cgroup_attr.attr,
	&rsc_svp_attr.attr,
	&rsc_sched_boost_ctrl_attr.attr,
	&rsc_switch_attr.attr,
	&sched_boost_attr.attr,
	&sched_stat_attr.attr,
	&ux_state_attr.attr,
	&rsc_gpu_freq_break_attr.attr,
	&enabled_attr.attr,
	&debug_attr.attr,
	&func_debug_attr.attr,
	&vaudit_switch_attr.attr,
	&vaudit_syscheck_attr.attr,
	&vaudit_attr.attr,
	&appboost_late_attr.attr,
	&rsc_boost_attr.attr,
	&rsc_trace_enable_attr.attr,
	&rsc_dyn_svp_enable_attr.attr,
	&rsc_cpu_status_attr.attr,
	NULL,
};
static struct attribute *rsc_svp_attrs[] = {
	&cpu_usage_attr.attr,
	&cpulimit_attr.attr,
	&cpulimit_uid_attr.attr,
	&cpu_multi_benchmack_attr.attr,
	&launcher_boost_rt_attr.attr,
	&rt_ui_boost_to_prime_attr.attr,
	&sort_util_pid_attr.attr,
	&support_vboostsdk_attr.attr,
	&svp_ui_boost_attr.attr,
	&kswapd_affinity_limit_attr.attr,
	NULL,
};
static struct attribute *rsc_fas_attrs[] = {
	&deplist_waker_attr.attr,
	&ex_boost_attr.attr,
	&fla_comm_launch_attr.attr,
	&fla_echo_break_freq_enable_attr.attr,
	&fla_freq_lock_attr.attr,
	&fla_support_non_attr.attr,
	&freq_max_attr.attr,
	&resume_cpus_attr.attr,
	NULL,
};
static struct attribute *rsc_fas_gpu_attrs[] = {
	&pm_qos_attr.attr,
	NULL,
};
static struct attribute *rsc_fas_mem_attrs[] = {
	&enable_attr.attr,
	NULL,
};
static struct attribute *rsc_afs_attrs[] = {
	&fp_hint_attr.attr,
	NULL,
};
static struct attribute *rsc_qos_attrs[] = {
	&get_task_qos_attr.attr,
	&qos_enable_attr.attr,
	&qos_stat_attr.attr,
	&set_task_qos_attr.attr,
	NULL,
};
static struct attribute *rsc_mbd_attrs[] = {
	&mbd_enable_attr.attr,
	&refault_attr.attr,
	&refault_readable_attr.attr,
	&slowpath_attr.attr,
	&slowpath_readable_attr.attr,
	NULL,
};
static struct attribute *rsc_memqos_attrs[] = {
	&higqos_slowpath_timeout_attr.attr,
	&lowqos_slowpath_tcnt_attr.attr,
	&memqos_debug_enable_attr.attr,
	&memqos_filemap_fault_trace_enabled_attr.attr,
	&memqos_mb_pg_pool_maxpages_attr.attr,
	&memqos_page_pool_attr.attr,
	&memqos_page_pool_enabled_attr.attr,
	&memqos_param_attr.attr,
	&memqos_rc_pg_pool_maxpages_attr.attr,
	&memqos_slowpath_trace_enabled_attr.attr,
	&memqos_unmb_pg_pool_maxpages_attr.attr,
	&memqos_wait_time_attr.attr,
	&midqos_slowpath_tcnt_attr.attr,
	&min_free_kbytes_boost_attr.attr,
	&slowpath_max_tcnt_attr.attr,
	&slowpath_trace_time_limit_attr.attr,
	NULL,
};
static struct attribute *rsc_sys_heap_attrs[] = {
	&memory_info_attr.attr,
	NULL,
};
static struct attribute *rsc_sys_heap_charge_attrs[] = {
	&sys_heap_pools_charge_attr.attr,
	NULL,
};
static struct attribute *rsc_vgarph_attrs[] = {
	&ray_tracing_attr.attr,
	NULL,
};
static int rsc_chown_system(const char *dir, const char *name)
{
	struct path path;
	struct inode *inode;
	char full[256];
	int rc;
	if (snprintf(full, sizeof(full), "%s/%s", dir, name) >= sizeof(full))
		return -ENAMETOOLONG;
	rc = kern_path(full, LOOKUP_FOLLOW, &path);
	if (rc)
		return rc;
	inode = d_inode(path.dentry);
	inode_lock(inode);
	inode->i_uid = KUIDT_INIT(RSC_SYSTEM_UID);
	inode->i_gid = KGIDT_INIT(RSC_SYSTEM_GID);
	inode_unlock(inode);
	path_put(&path);
	return 0;
}
static int rsc_add_files(struct kobject *parent, const char *dir, struct attribute **attrs, umode_t mode)
{
	int i;
	int rc;
	for (i = 0; attrs[i]; i++) {
		rc = sysfs_create_file(parent, attrs[i]);
		if (rc) {
			pr_err(DRV_NAME ": failed to create %s: %d\n",
			       attrs[i]->name, rc);
			return rc;
		}
		if (rsc_permissive) {
			rc = sysfs_chmod_file(parent, attrs[i], 0666);
			if (rc) {
				pr_err(DRV_NAME ": failed to chmod %s: %d\n",
				       attrs[i]->name, rc);
				return rc;
			}
		} else if (mode == 0660) {
			rc = rsc_chown_system(dir, attrs[i]->name);
			if (rc)
				pr_warn(DRV_NAME ": failed to chown %s/%s: %d\n",
					dir, attrs[i]->name, rc);
		}
	}
	return 0;
}
static ssize_t rsc_proc_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	return simple_read_from_buffer(ubuf, len, ppos, "0\n", 2);
}
static ssize_t rsc_proc_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
	return len;
}
static const struct proc_ops rsc_proc_ops = {
	.proc_read = rsc_proc_read,
	.proc_write = rsc_proc_write,
};
static ssize_t rsc_proc_frame_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
	char b[32];
	int v;
	if (len >= sizeof(b))
		len = sizeof(b) - 1;
	if (copy_from_user(b, ubuf, len))
		return -EFAULT;
	b[len] = '\0';
	if (!kstrtoint(b, 0, &v))
		rsc_current_boost(v);
	return len;
}
static const struct proc_ops rsc_proc_frame_ops = {
	.proc_read = rsc_proc_read,
	.proc_write = rsc_proc_frame_write,
};
static ssize_t rsc_proc_mem_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct sysinfo si = {};
	char b[192];
	int n;
	si_meminfo(&si);
	si_swapinfo(&si);
	n = scnprintf(b, sizeof(b),
		      "mem_total=%llu mem_free=%llu swap_total=%llu swap_free=%llu\n",
		      (u64)si.totalram >> (20 - PAGE_SHIFT),
		      (u64)si.freeram >> (20 - PAGE_SHIFT),
		      (u64)si.totalswap >> (20 - PAGE_SHIFT),
		      (u64)si.freeswap >> (20 - PAGE_SHIFT));
	return simple_read_from_buffer(ubuf, len, ppos, b, n);
}
static const struct proc_ops rsc_proc_mem_ops = {
	.proc_read = rsc_proc_mem_read,
	.proc_write = rsc_proc_write,
};
static ssize_t rsc_proc_lmk_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct sysinfo si = {};
	bool low;
	char b[4];
	si_meminfo(&si);
	low = si.totalram && (si.freeram * 100 / si.totalram) < 5;
	scnprintf(b, sizeof(b), "%d\n", low ? 1 : 0);
	return simple_read_from_buffer(ubuf, len, ppos, b, 2);
}
static const struct proc_ops rsc_proc_lmk_ops = {
	.proc_read = rsc_proc_lmk_read,
	.proc_write = rsc_proc_write,
};
static ssize_t rsc_proc_antifrag_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	u64 free_pages = 0, blocks = 0, suitable = 0;
	char b[160];
	int n;
	rsc_buddyinfo(&free_pages, &blocks, &suitable);
	n = scnprintf(b, sizeof(b), "free_pages=%llu free_blocks_total=%llu free_blocks_suitable=%llu\n", free_pages, blocks, suitable);
	return simple_read_from_buffer(ubuf, len, ppos, b, n);
}
static const struct proc_ops rsc_proc_antifrag_ops = {
	.proc_read = rsc_proc_antifrag_read,
	.proc_write = rsc_proc_write,
};
static const struct proc_ops *rsc_proc_ops_for(const char *name)
{
	if (!strcmp(name, "frame_set_priority"))
		return &rsc_proc_frame_ops;
	if (!strcmp(name, "mem_blocks_mb"))
		return &rsc_proc_mem_ops;
	if (!strcmp(name, "check_lmk"))
		return &rsc_proc_lmk_ops;
	if (!strcmp(name, "mem_antifrag"))
		return &rsc_proc_antifrag_ops;
	return &rsc_proc_ops;
}
static void vivo_rsc_cleanup(void)
{
	int i;
	rsc_engine_stop();
	for (i = 0; i < RSC_PROC_ENTRIES; i++) {
		if (rsc_proc_entries[i]) {
			proc_remove(rsc_proc_entries[i]);
			rsc_proc_entries[i] = NULL;
		}
	}
	if (vivo_rsc_dir) {
		proc_remove(vivo_rsc_dir);
		vivo_rsc_dir = NULL;
	}
	kobject_put(rsc_vgarph_kobj);
	kobject_put(rsc_sys_heap_charge_kobj);
	kobject_put(rsc_sys_heap_kobj);
	kobject_put(rsc_memqos_kobj);
	kobject_put(rsc_mbd_kobj);
	kobject_put(rsc_qos_kobj);
	kobject_put(rsc_afs_kobj);
	kobject_put(rsc_fas_mem_kobj);
	kobject_put(rsc_fas_gpu_kobj);
	kobject_put(rsc_fas_kobj);
	kobject_put(rsc_svp_kobj);
	kobject_put(rsc_kobj);
	rsc_vgarph_kobj = NULL;
	rsc_sys_heap_charge_kobj = NULL;
	rsc_sys_heap_kobj = NULL;
	rsc_memqos_kobj = NULL;
	rsc_mbd_kobj = NULL;
	rsc_qos_kobj = NULL;
	rsc_afs_kobj = NULL;
	rsc_fas_mem_kobj = NULL;
	rsc_fas_gpu_kobj = NULL;
	rsc_fas_kobj = NULL;
	rsc_svp_kobj = NULL;
	rsc_kobj = NULL;
}
static int __init vivo_rsc_init(void)
{
	int i;
	int rc;
	rsc_engine_start();
	rsc_kobj = kobject_create_and_add("rsc", NULL);
	if (!rsc_kobj)
		return -ENOMEM;
	rsc_svp_kobj = kobject_create_and_add("svp", rsc_kobj);
	rsc_fas_kobj = kobject_create_and_add("fas", rsc_kobj);
	rsc_fas_gpu_kobj = kobject_create_and_add("fas_gpu", rsc_fas_kobj);
	rsc_fas_mem_kobj = kobject_create_and_add("fas_mem", rsc_fas_kobj);
	rsc_afs_kobj = kobject_create_and_add("afs", rsc_kobj);
	rsc_qos_kobj = kobject_create_and_add("qos", rsc_kobj);
	rsc_mbd_kobj = kobject_create_and_add("mbd", rsc_kobj);
	rsc_memqos_kobj = kobject_create_and_add("memqos", rsc_kobj);
	rsc_sys_heap_kobj = kobject_create_and_add("system_heap", rsc_kobj);
	rsc_sys_heap_charge_kobj =
		kobject_create_and_add("charge", rsc_sys_heap_kobj);
	rsc_vgarph_kobj = kobject_create_and_add("vgarph", rsc_kobj);
	if (!rsc_svp_kobj || !rsc_fas_kobj || !rsc_fas_gpu_kobj ||
	    !rsc_fas_mem_kobj || !rsc_afs_kobj || !rsc_qos_kobj ||
	    !rsc_mbd_kobj || !rsc_memqos_kobj || !rsc_sys_heap_kobj ||
	    !rsc_sys_heap_charge_kobj || !rsc_vgarph_kobj) {
		rc = -ENOMEM;
		goto err;
	}
	rc = sysfs_create_file(rsc_kobj, &ktop_attr.attr);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_kobj, "/sys/rsc", rsc_root_attrs, 0660);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_svp_kobj, "/sys/rsc/svp", rsc_svp_attrs, 0660);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_fas_kobj, "/sys/rsc/fas", rsc_fas_attrs, 0660);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_fas_gpu_kobj, "/sys/rsc/fas/fas_gpu", rsc_fas_gpu_attrs, 0660);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_fas_mem_kobj, "/sys/rsc/fas/fas_mem", rsc_fas_mem_attrs, 0660);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_afs_kobj, "/sys/rsc/afs", rsc_afs_attrs, 0660);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_qos_kobj, "/sys/rsc/qos", rsc_qos_attrs, 0660);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_mbd_kobj, "/sys/rsc/mbd", rsc_mbd_attrs, 0644);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_memqos_kobj, "/sys/rsc/memqos", rsc_memqos_attrs, 0644);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_sys_heap_kobj, "/sys/rsc/system_heap", rsc_sys_heap_attrs, 0644);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_sys_heap_charge_kobj, "/sys/rsc/system_heap/charge", rsc_sys_heap_charge_attrs, 0660);
	if (rc)
		goto err;
	rc = rsc_add_files(rsc_vgarph_kobj, "/sys/rsc/vgarph", rsc_vgarph_attrs, 0660);
	if (rc)
		goto err;
	vivo_rsc_dir = proc_mkdir("vivo_rsc", NULL);
	if (!vivo_rsc_dir) {
		rc = -ENOMEM;
		goto err;
	}
	for (i = 0; i < RSC_PROC_ENTRIES; i++) {
		rsc_proc_entries[i] = proc_create(rsc_proc_names[i],
					rsc_permissive ? 0666 : 0660,
					vivo_rsc_dir, rsc_proc_ops_for(rsc_proc_names[i]));
		if (!rsc_proc_entries[i]) {
			rc = -ENOMEM;
			goto err;
		}
		if (!rsc_permissive)
			proc_set_user(rsc_proc_entries[i],
				      KUIDT_INIT(RSC_SYSTEM_UID),
				      KGIDT_INIT(RSC_SYSTEM_GID));
	}
	pr_info(DRV_NAME ": loaded (/sys/rsc, /proc/vivo_rsc)%s\n",
		rsc_permissive ? " [permissive 0666]" : "");
	return 0;
err:
	vivo_rsc_cleanup();
	return rc;
}
static void __exit vivo_rsc_exit(void)
{
	vivo_rsc_cleanup();
	pr_info(DRV_NAME ": unloaded\n");
}
module_init(vivo_rsc_init);
module_exit(vivo_rsc_exit);
MODULE_DESCRIPTION("Vivo RSC userspace ABI with uclamp/RT QoS engine");
MODULE_AUTHOR("~jkoo");
MODULE_LICENSE("GPL");
