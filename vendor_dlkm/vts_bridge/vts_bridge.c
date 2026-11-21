// SPDX-License-Identifier: GPL-2.0-only
// vts_bridge vivo touch/VTS userspace ABI for the fuxi.
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/signal.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define DRV_NAME "vts_bridge"

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

static int vb_read_file(const char *path, char *buf, size_t len)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	if (!buf || len < 2)
		return -EINVAL;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_read(f, buf, len - 1, &pos);
	filp_close(f, NULL);
	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	return (int)n;
}
static DEFINE_MUTEX(vb_lock);
static int vb_lcd_on = 1;
static int vb_proximity = 1;
static int vb_charging;
static int vb_game_mode;
static char vb_last_raw[128] = "";
static bool vb_support_singletap;
module_param_named(support_singletap, vb_support_singletap, bool, 0644);
MODULE_PARM_DESC(support_singletap, "advertise /sys/class/vts/vivo_ts/properties/support_singletap");
static int vb_symbol_lookup(const char *name, void **addr)
{
	struct kprobe kp = { .symbol_name = name };
	int ret;
	ret = register_kprobe(&kp);
	if (ret)
		return ret;
	*addr = (void *)kp.addr;
	unregister_kprobe(&kp);
	return 0;
}
static void *vb_fts_data;
static struct kprobe vb_ev_kp = { .symbol_name = "fts_event_handler" };
static struct kprobe vb_pn_kp = { .symbol_name = "fts_drm_panel_notifier_callback" };
static int vb_note_data(void *d)
{
	if ((unsigned long)d >= 0xffff000000000000UL) {
		WRITE_ONCE(vb_fts_data, d);
		return 1;
	}
	return 0;
}
static int vb_ev_pre(struct kprobe *p, struct pt_regs *regs)
{
	vb_note_data((void *)regs->regs[1]);
	return 0;
}
static int vb_pn_pre(struct kprobe *p, struct pt_regs *regs)
{
	vb_note_data((void *)regs->regs[2]);
	return 0;
}
static void *vb_fts_resume_work;
static int vb_resume_ret = -ENOSYS;
static __attribute__((no_sanitize("cfi"))) int vb_call_resume_work(void *w)
{
	int (*fn)(void *) = vb_fts_resume_work;
	return fn(w);
}

static ssize_t resume_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", vb_resume_ret);
}

static ssize_t resume_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	char tmp[16];
	size_t n = min(count, sizeof(tmp) - 1);
	int v;
	memcpy(tmp, buf, n);
	tmp[n] = '\0';
	if (kstrtoint(strim(tmp), 0, &v))
		return -EINVAL;
	if (!vb_fts_data || !vb_fts_resume_work)
		return -ENOSYS;
	if (v) {
		WRITE_ONCE(*(u32 *)((char *)vb_fts_data + 0x2d0), 0);
		vb_resume_ret = vb_call_resume_work((char *)vb_fts_data + 0x78);
		pr_info(DRV_NAME ": resume work -> %d\n", vb_resume_ret);
	}
	return count;
}
static struct kobj_attribute resume_attr = __ATTR(resume, 0664, resume_show, resume_store);
static ssize_t proximity_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", vb_proximity);
}

static struct kobj_attribute proximity_attr = __ATTR(proximity, 0444, proximity_show, NULL);
static int vb_watcher_pid;
static ssize_t watcher_pid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", vb_watcher_pid);
}

static ssize_t watcher_pid_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	char tmp[16];
	size_t n = min(count, sizeof(tmp) - 1);
	int v;
	memcpy(tmp, buf, n);
	tmp[n] = '\0';
	if (kstrtoint(strim(tmp), 0, &v))
		return -EINVAL;
	vb_watcher_pid = v;
	return count;
}
static struct kobj_attribute watcher_pid_attr = __ATTR(watcher_pid, 0644, watcher_pid_show, watcher_pid_store);
static bool vb_auto_resume;
module_param_named(auto_resume, vb_auto_resume, bool, 0644);
MODULE_PARM_DESC(auto_resume, "keep the touch IC scanning while the panel is off");
static struct delayed_work vb_keep_scan_work;
static void vb_keep_scan_fn(struct work_struct *work)
{
	u32 gate;
	if (!vb_fts_data || !vb_fts_resume_work)
		return;
	WRITE_ONCE(*(u32 *)((char *)vb_fts_data + 0x2d0), 0);
	vb_resume_ret = vb_call_resume_work((char *)vb_fts_data + 0x78);
	gate = READ_ONCE(*(u32 *)((char *)vb_fts_data + 0x2d0));
	pr_info(DRV_NAME ": keep-scan resume=%d sleep_gate=%u\n",
		vb_resume_ret, gate);
}
static void vb_keep_scan_schedule_ms(const char *why, unsigned int ms)
{
	if (!vb_auto_resume)
		return;
	pr_info(DRV_NAME ": keep-scan scheduled (%s)\n", why);
	schedule_delayed_work(&vb_keep_scan_work, msecs_to_jiffies(ms));
}
static void vb_keep_scan_schedule(const char *why)
{
	vb_keep_scan_schedule_ms(why, 1500);
}
static struct kobject *ts_kobj;
static ssize_t app_name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;

	mutex_lock(&vb_lock);
	ret = sysfs_emit(buf, "%s\n", vb_last_raw);
	mutex_unlock(&vb_lock);
	return ret;
}
static ssize_t app_name_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	char tmp[128], raw[128];
	char *cmd, *name, *state;
	size_t n = min(count, sizeof(tmp) - 1);
	int v = 0;
	memcpy(tmp, buf, n);
	tmp[n] = '\0';
	strim(tmp);
	strscpy(raw, tmp, sizeof(raw));
	cmd = tmp;
	name = strchr(cmd, ':');
	if (name) {
		*name++ = '\0';
		state = strchr(name, ':');
		if (state) {
			*state++ = '\0';
			if (kstrtoint(state, 0, &v))
				v = 0;
		}
	} else {
		name = "";
	}
	mutex_lock(&vb_lock);
	strscpy(vb_last_raw, raw, sizeof(vb_last_raw));
	if (!strcmp(cmd, "vts_lcd"))
		vb_lcd_on = v ? 1 : 0;
	else if (!strcmp(cmd, "vts_proximity"))
		vb_proximity = v ? 1 : 0;
	else if (!strcmp(cmd, "vts_charge"))
		vb_charging = v ? 1 : 0;
	else if (!strcmp(cmd, "vts_game_mode"))
		vb_game_mode = v ? 1 : 0;
	mutex_unlock(&vb_lock);
	if (!strcmp(cmd, "vts_lcd")) {
		if (!v)
			vb_keep_scan_schedule("screen off");
		else
			cancel_delayed_work(&vb_keep_scan_work);
		if (vb_watcher_pid > 0) {
			struct pid *p = find_get_pid(vb_watcher_pid);
			int kr = -ESRCH;
			if (p) {
				kr = kill_pid(p, SIGUSR1, 1);
				put_pid(p);
			}
			pr_info(DRV_NAME ": watcher pid %d p=%px kill=%d\n", vb_watcher_pid, p, kr);
		}
	}
	return count;
}
struct vb_iattr {
	struct kobj_attribute attr;
	int value;
};
static ssize_t vb_i_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct vb_iattr *a = container_of(attr, struct vb_iattr, attr);
	ssize_t ret;
	mutex_lock(&vb_lock);
	ret = sysfs_emit(buf, "%d\n", a->value);
	mutex_unlock(&vb_lock);
	return ret;
}

static ssize_t vb_i_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct vb_iattr *a = container_of(attr, struct vb_iattr, attr);
	char tmp[32];
	size_t n = min(count, sizeof(tmp) - 1);
	int v;
	memcpy(tmp, buf, n);
	tmp[n] = '\0';
	if (kstrtoint(strim(tmp), 0, &v))
		return -EINVAL;
	mutex_lock(&vb_lock);
	a->value = v;
	mutex_unlock(&vb_lock);
	pr_info(DRV_NAME ": /sys/touchscreen/%s = %d\n", attr->attr.name, v);
	return count;
}
#define VB_IATTR(_name)							\
	static struct vb_iattr _name = {				\
		.attr = __ATTR(_name, 0664, vb_i_show, vb_i_store),	\
		.value = 0,						\
	}
VB_IATTR(gesture_point);
VB_IATTR(gesture_time_stamp);
VB_IATTR(imei_ctl);
VB_IATTR(gesture_switch);
VB_IATTR(gesture_switch_export);
VB_IATTR(gesture_letter_switch);
VB_IATTR(gesture_sign_switch);
VB_IATTR(swipe_switch);
VB_IATTR(finger_gesture_switch);
VB_IATTR(dclick_switch);
VB_IATTR(dclick_simulate_switch);
VB_IATTR(edge_suppress_switch);
VB_IATTR(gloves_mode_switch);
VB_IATTR(calling_switch);
VB_IATTR(interrupt_enable);
VB_IATTR(lcd_state_set);
VB_IATTR(user_defined_gesture_enable);
VB_IATTR(sensor_rx_tx);
static struct kobj_attribute app_name_attr = __ATTR(app_name, 0664, app_name_show, app_name_store);
static struct kobj_attribute *ts_attrs[] = {
	&app_name_attr,
	&gesture_point.attr,
	&gesture_time_stamp.attr,
	&imei_ctl.attr,
	&gesture_switch.attr,
	&gesture_switch_export.attr,
	&gesture_letter_switch.attr,
	&gesture_sign_switch.attr,
	&swipe_switch.attr,
	&finger_gesture_switch.attr,
	&dclick_switch.attr,
	&dclick_simulate_switch.attr,
	&edge_suppress_switch.attr,
	&gloves_mode_switch.attr,
	&calling_switch.attr,
	&interrupt_enable.attr,
	&lcd_state_set.attr,
	&user_defined_gesture_enable.attr,
	&sensor_rx_tx.attr,
	&resume_attr,
	&proximity_attr,
	&watcher_pid_attr,
};
static struct class *vts_class;
static struct device *vts_dev;
static struct kobject *props_kobj;
static struct kobject *status_kobj;
static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0x0033\n");
}
static DEVICE_ATTR_RO(version);
static ssize_t touch_times_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0 0 0 0 0 0 0\n");
}
static DEVICE_ATTR_RO(touch_times);
static ssize_t lcmid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	char tmp[64];
	if (vb_read_file("/sys/lcm/lcm_id", tmp, sizeof(tmp)) > 0)
		return sysfs_emit(buf, "%s\n", strim(tmp));
	return sysfs_emit(buf, "0x20\n");
}
static ssize_t support_singletap_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", vb_support_singletap ? 1 : 0);
}
static struct kobj_attribute lcmid_attr = __ATTR(lcmid, 0444, lcmid_show, NULL);
static struct kobj_attribute support_singletap_attr = __ATTR(support_singletap, 0444, support_singletap_show, NULL);
static struct kobj_attribute *props_attrs[] = {
	&lcmid_attr,
	&support_singletap_attr,
};
static ssize_t charging_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;
	mutex_lock(&vb_lock);
	ret = sysfs_emit(buf, "%d\n", vb_charging);
	mutex_unlock(&vb_lock);
	return ret;
}
static ssize_t game_mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;
	mutex_lock(&vb_lock);
	ret = sysfs_emit(buf, "%d\n", vb_game_mode);
	mutex_unlock(&vb_lock);
	return ret;
}
static ssize_t lcd_on_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;
	mutex_lock(&vb_lock);
	ret = sysfs_emit(buf, "%d\n", vb_lcd_on);
	mutex_unlock(&vb_lock);
	return ret;
}
static ssize_t finger_up_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static ssize_t fingerprint_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static struct kobj_attribute charging_attr = __ATTR(charging, 0444, charging_show, NULL);
static struct kobj_attribute game_mode_attr = __ATTR(game_mode, 0444, game_mode_show, NULL);
static struct kobj_attribute lcd_on_attr = __ATTR(lcd_on, 0444, lcd_on_show, NULL);
static struct kobj_attribute finger_up_attr = __ATTR(finger_up, 0444, finger_up_show, NULL);
static struct kobj_attribute fingerprint_attr = __ATTR(fingerprint, 0444, fingerprint_show, NULL);
static struct kobj_attribute *status_attrs[] = {
	&charging_attr,
	&game_mode_attr,
	&lcd_on_attr,
	&finger_up_attr,
	&fingerprint_attr,
};
static int vb_add_attrs(struct kobject *kobj, struct kobj_attribute **attrs, size_t n)
{
	size_t i;
	int ret;
	for (i = 0; i < n; i++) {
		ret = sysfs_create_file(kobj, &attrs[i]->attr);
		if (ret) {
			pr_err(DRV_NAME ": create %s failed: %d\n",
			       attrs[i]->attr.name, ret);
			while (i--)
				sysfs_remove_file(kobj, &attrs[i]->attr);
			return ret;
		}
		if (attrs[i]->attr.mode & 0200) {
			int cr = sysfs_chmod_file(kobj, &attrs[i]->attr, 0666);
			if (cr)
				pr_warn(DRV_NAME ": chmod %s failed: %d\n", attrs[i]->attr.name, cr);
		}
	}
	return 0;
}
static void vb_del_attrs(struct kobject *kobj, struct kobj_attribute **attrs, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		sysfs_remove_file(kobj, &attrs[i]->attr);
}
static int __init vts_bridge_init(void)
{
	int ret;
	INIT_DELAYED_WORK(&vb_keep_scan_work, vb_keep_scan_fn);
	if (vb_symbol_lookup("fts_resume_work", &vb_fts_resume_work))
		pr_warn(DRV_NAME ": fts_resume_work not resolvable\n");
	vb_ev_kp.pre_handler = vb_ev_pre;
	if (register_kprobe(&vb_ev_kp))
		pr_warn(DRV_NAME ": kprobe fts_event_handler failed\n");
	vb_pn_kp.pre_handler = vb_pn_pre;
	if (register_kprobe(&vb_pn_kp))
		pr_warn(DRV_NAME ": kprobe panel notifier failed\n");
	ts_kobj = kobject_create_and_add("touchscreen", NULL);
	if (!ts_kobj ||
	    vb_add_attrs(ts_kobj, ts_attrs, ARRAY_SIZE(ts_attrs))) {
		pr_err(DRV_NAME ": /sys/touchscreen failed\n");
		return -ENOMEM;
	}
	vts_class = class_create(THIS_MODULE, "vts");
	if (IS_ERR(vts_class)) {
		pr_err(DRV_NAME ": class_create(vts) failed: %ld\n",
		       PTR_ERR(vts_class));
		return PTR_ERR(vts_class);
	}
	vts_dev = device_create(vts_class, NULL, 0, NULL, "vivo_ts");
	if (IS_ERR(vts_dev)) {
		pr_err(DRV_NAME ": device_create(vivo_ts) failed: %ld\n",
		       PTR_ERR(vts_dev));
		return PTR_ERR(vts_dev);
	}
	ret = device_create_file(vts_dev, &dev_attr_version);
	ret |= device_create_file(vts_dev, &dev_attr_touch_times);
	if (ret)
		pr_warn(DRV_NAME ": vivo_ts top-level attrs failed: %d\n", ret);
	props_kobj = kobject_create_and_add("properties", &vts_dev->kobj);
	if (!props_kobj ||
	    vb_add_attrs(props_kobj, props_attrs, ARRAY_SIZE(props_attrs)))
		pr_warn(DRV_NAME ": vivo_ts/properties failed\n");
	status_kobj = kobject_create_and_add("status", &vts_dev->kobj);
	if (!status_kobj ||
	    vb_add_attrs(status_kobj, status_attrs, ARRAY_SIZE(status_attrs)))
		pr_warn(DRV_NAME ": vivo_ts/status failed\n");
	pr_info(DRV_NAME ": ready (/sys/touchscreen, /sys/class/vts/vivo_ts)\n");
	return 0;
}
static void __exit vts_bridge_exit(void)
{
	unregister_kprobe(&vb_ev_kp);
	unregister_kprobe(&vb_pn_kp);
	cancel_delayed_work_sync(&vb_keep_scan_work);
	if (status_kobj) {
		vb_del_attrs(status_kobj, status_attrs,
			     ARRAY_SIZE(status_attrs));
		kobject_put(status_kobj);
		status_kobj = NULL;
	}
	if (props_kobj) {
		vb_del_attrs(props_kobj, props_attrs, ARRAY_SIZE(props_attrs));
		kobject_put(props_kobj);
		props_kobj = NULL;
	}
	if (vts_dev) {
		device_remove_file(vts_dev, &dev_attr_touch_times);
		device_remove_file(vts_dev, &dev_attr_version);
		device_unregister(vts_dev);
		vts_dev = NULL;
	}
	if (vts_class) {
		class_destroy(vts_class);
		vts_class = NULL;
	}
	if (ts_kobj) {
		vb_del_attrs(ts_kobj, ts_attrs, ARRAY_SIZE(ts_attrs));
		kobject_put(ts_kobj);
		ts_kobj = NULL;
	}
}
module_init(vts_bridge_init);
module_exit(vts_bridge_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vivo touch/VTS userspace ABI for fuxi.");
MODULE_AUTHOR("~jkoo");
