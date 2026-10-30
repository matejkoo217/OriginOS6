// SPDX-License-Identifier: GPL-2.0-only
/*
 * board_info - /sys/devices/soc1/{core_num,cpu_type,user_cpu_freq,cpu_freq}
 *              for OriginOS' "About phone".
 *
 *   core_num       num_possible_cpus()                     (fuxi: 8)
 *   cpu_freq       max cpuinfo_max_freq over all policies  (fuxi: 3187200)
 *   user_cpu_freq  that frequency rounded to the marketing
 *                  100 MHz step  (3187200 -> "3.20")
 *   cpu_type       SoC platform code (fuxi / SM8550 -> "8550")
 *
 * cpu_type must stay the platform code: vivo's Settings (CpuInfoImpl, in
 * framework-adapter.jar) builds the resource name "cpuinfo_qcom<value>" and
 * looks it up in vivo-res.apk.  "8550" resolves to
 * cpuinfo_qcom8550 = "Snapdragon(R) 8 Gen 2 Mobile Platform".  Anything else
 * misses that lookup and falls back to cpuinfo_qcom ("Snapdragon", no ®)
 */
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#define CPU0_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"
#define CPU_POLICY_FREQ_FMT "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq"
#define CPU_POLICY_MAX 8
static struct device *soc1_dev;
static DEFINE_MUTEX(soc1_lock);
static bool core_num_override;
static int core_num;
static bool cpu_freq_override;
static int cpu_freq;
static bool user_cpu_freq_override;
static char user_cpu_freq[32];
static bool cpu_type_override;
static char cpu_type[64];
static int read_int_file(const char *path)
{
	struct file *f;
	char buf[32];
	ssize_t n;
	loff_t pos = 0;
	int val;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return -ENOENT;
	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return -EIO;
	buf[n] = '\0';
	if (kstrtoint(strim(buf), 10, &val))
		return -EINVAL;
	return val;
}
static int hw_max_cpu_freq(void)
{
	char path[80];
	int i, v;
	int best = 0;
	for (i = 0; i < CPU_POLICY_MAX; i++) {
		snprintf(path, sizeof(path), CPU_POLICY_FREQ_FMT, i);
		v = read_int_file(path);
		if (v > best)
			best = v;
	}
	if (best <= 0)
		best = read_int_file(CPU0_FREQ_PATH);
	return best > 0 ? best : 0;
}
static const char *hw_cpu_type(void)
{
	return "8550"; //if you dont have compatibile soc then set it to string.
}
static void freq_to_user(int khz, char *out, size_t len)
{
	int mhz = khz / 1000;
	int rounded;
	if (khz <= 0) {
		scnprintf(out, len, "0.00");
		return;
	}
	rounded = ((mhz + 50) / 100) * 100;
	scnprintf(out, len, "%d.%02d", rounded / 1000, (rounded % 1000) / 10);
}
static ssize_t core_num_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int value;
	mutex_lock(&soc1_lock);
	value = core_num_override ? core_num : num_possible_cpus();
	mutex_unlock(&soc1_lock);
	return sysfs_emit(buf, "%d\n", value);
}
static ssize_t core_num_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int value;
	if (kstrtoint(buf, 10, &value))
		return -EINVAL;
	mutex_lock(&soc1_lock);
	core_num = value;
	core_num_override = true;
	mutex_unlock(&soc1_lock);
	return count;
}
static ssize_t cpu_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const char *value;
	mutex_lock(&soc1_lock);
	value = cpu_type_override ? cpu_type : hw_cpu_type();
	mutex_unlock(&soc1_lock);
	return sysfs_emit(buf, "%s\n", value);
}
static ssize_t cpu_type_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	size_t len = min(count, sizeof(cpu_type) - 1);
	mutex_lock(&soc1_lock);
	memcpy(cpu_type, buf, len);
	if (len && cpu_type[len - 1] == '\n')
		len--;
	cpu_type[len] = '\0';
	cpu_type_override = true;
	mutex_unlock(&soc1_lock);
	return count;
}
static ssize_t user_cpu_freq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	char out[32];
	int khz;
	bool forced;
	mutex_lock(&soc1_lock);
	forced = user_cpu_freq_override;
	if (forced)
		strscpy(out, user_cpu_freq, sizeof(out));
	khz = cpu_freq_override ? cpu_freq : 0;
	mutex_unlock(&soc1_lock);
	if (!forced)
		freq_to_user(khz > 0 ? khz : hw_max_cpu_freq(), out, sizeof(out));

	return sysfs_emit(buf, "%s\n", out);
}
static ssize_t user_cpu_freq_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	size_t len = min(count, sizeof(user_cpu_freq) - 1);
	mutex_lock(&soc1_lock);
	memcpy(user_cpu_freq, buf, len);
	if (len && user_cpu_freq[len - 1] == '\n')
		len--;
	user_cpu_freq[len] = '\0';
	user_cpu_freq_override = true;
	mutex_unlock(&soc1_lock);
	return count;
}
static ssize_t cpu_freq_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int value;
	mutex_lock(&soc1_lock);
	value = cpu_freq_override ? cpu_freq : 0;
	mutex_unlock(&soc1_lock);
	if (!value)
		value = hw_max_cpu_freq();
	return sysfs_emit(buf, "%d\n", value);
}
static ssize_t cpu_freq_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int value;
	if (kstrtoint(buf, 10, &value))
		return -EINVAL;
	mutex_lock(&soc1_lock);
	cpu_freq = value;
	cpu_freq_override = true;
	mutex_unlock(&soc1_lock);
	return count;
}
static DEVICE_ATTR_RW(core_num);
static DEVICE_ATTR_RW(cpu_type);
static DEVICE_ATTR_RW(user_cpu_freq);
static DEVICE_ATTR_RW(cpu_freq);
static struct attribute *soc1_attrs[] = {
	&dev_attr_core_num.attr,
	&dev_attr_cpu_type.attr,
	&dev_attr_user_cpu_freq.attr,
	&dev_attr_cpu_freq.attr,
	NULL,
};
static const struct attribute_group soc1_attr_group = {
	.attrs = soc1_attrs,
};
static int __init soc1_init(void)
{
	int ret;
	soc1_dev = root_device_register("soc1");
	if (IS_ERR(soc1_dev)) {
		ret = PTR_ERR(soc1_dev);
		soc1_dev = NULL;
		pr_err("board_info: root_device_register failed: %d\n", ret);
		return ret;
	}
	ret = sysfs_create_group(&soc1_dev->kobj, &soc1_attr_group);
	if (ret) {
		pr_err("board_info: sysfs_create_group failed: %d\n", ret);
		root_device_unregister(soc1_dev);
		soc1_dev = NULL;
		return ret;
	}
	pr_info("board_info: %d cores, %d kHz, type '%s'\n",
		num_possible_cpus(), hw_max_cpu_freq(), hw_cpu_type());
	return 0;
}
static void __exit soc1_exit(void)
{
	if (!soc1_dev)
		return;
	sysfs_remove_group(&soc1_dev->kobj, &soc1_attr_group);
	root_device_unregister(soc1_dev);
	soc1_dev = NULL;
}
module_init(soc1_init);
module_exit(soc1_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("/sys/devices/soc1 derived from SoC");
MODULE_AUTHOR("~jkoo");
