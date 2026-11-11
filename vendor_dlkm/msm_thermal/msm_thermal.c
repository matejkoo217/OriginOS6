// SPDX-License-Identifier: GPL-2.0
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sysfs.h>

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

#define DRV_NAME "msm_thermal"
#define CPU_ONLINE_FMT "/sys/devices/system/cpu/cpu%d/online"

static struct kobject *core_ctl_kobj;
static int cpus_offlined;
static int cpu_online_write(int cpu, bool on)
{
	char path[64];
	char val[2] = { on ? '1' : '0', '\0' };
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	scnprintf(path, sizeof(path), CPU_ONLINE_FMT, cpu);
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, val, 1, &pos);
	filp_close(f, NULL);

	return n < 0 ? (int)n : 0;
}
static int cpu_online_read(int cpu)
{
	char path[64], buf[8];
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	int v = 1;
	scnprintf(path, sizeof(path), CPU_ONLINE_FMT, cpu);
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return 1;
	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (n > 0) {
		buf[n] = '\0';
		if (kstrtoint(strim(buf), 10, &v))
			v = 1;
	}
	return v;
}
static unsigned int offline_mask(void)
{
	unsigned int mask = 0;
	int cpu;
	for (cpu = 1; cpu < nr_cpu_ids; cpu++)
		if (!cpu_online_read(cpu))
			mask |= BIT(cpu);
	return mask;
}

static ssize_t cpus_offlined_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", offline_mask());
}

static ssize_t cpus_offlined_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int want, cur, online = 0;
	int cpu, ret;
	if (kstrtouint(buf, 0, &want))
		return -EINVAL;
	if (want & BIT(0)) {
		pr_err(DRV_NAME ": CPU0 cannot be offlined\n");
		return -EINVAL;
	}
	want &= GENMASK(nr_cpu_ids - 1, 0);
	cur = offline_mask();
	cpus_offlined = want;

	if (want == cur)
		return count;
	for (cpu = 1; cpu < nr_cpu_ids; cpu++)
		if (!(want & BIT(cpu)))
			online++;
	if (!online) {
		pr_warn(DRV_NAME ": refusing to offline every CPU\n");
		return -EINVAL;
	}
	for (cpu = 1; cpu < nr_cpu_ids; cpu++) {
		bool on = !(want & BIT(cpu));
		bool is_on = cpu_online_read(cpu);
		if (on == is_on)
			continue;
		ret = cpu_online_write(cpu, on);
		pr_info(DRV_NAME ": cpu%d -> %s (rc=%d)\n", cpu,
			on ? "online" : "offline", ret);
		if (ret)
			return ret;
	}
	pr_info(DRV_NAME ": cpus_offlined %u -> %u\n", cur, want);
	return count;
}
static struct kobj_attribute cpus_offlined_attr = __ATTR(cpus_offlined, 0644, cpus_offlined_show, cpus_offlined_store);
static int __init msm_thermal_init(void)
{
	if (!nr_cpu_ids)
		return -ENODEV;
	core_ctl_kobj = kobject_create_and_add("core_control", &THIS_MODULE->mkobj.kobj);
	if (!core_ctl_kobj)
		return -ENOMEM;
	if (sysfs_create_file(core_ctl_kobj, &cpus_offlined_attr.attr)) {
		kobject_put(core_ctl_kobj);
		core_ctl_kobj = NULL;
		return -ENOMEM;
	}
	pr_info(DRV_NAME ": core_control up (%d cpus, offline mask %u)\n",
		nr_cpu_ids, offline_mask());
	return 0;
}
static void __exit msm_thermal_exit(void)
{
	if (!core_ctl_kobj)
		return;
	sysfs_remove_file(core_ctl_kobj, &cpus_offlined_attr.attr);
	kobject_put(core_ctl_kobj);
	core_ctl_kobj = NULL;
}
module_init(msm_thermal_init);
module_exit(msm_thermal_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CPU hotplug for vivo PEM.");
MODULE_AUTHOR("~jkoo");
