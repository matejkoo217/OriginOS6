// SPDX-License-Identifier: GPL-2.0-only
/*
 * Adds read-only attributes to the existing power_supply devices; it does not
 * replace qti_battery_charger.ko and does not alter real charger properties.
 *
 * Attributes:
 *   /sys/class/power_supply/battery/chg_type
 *   /sys/class/power_supply/battery/fg_passed_charge
 *   /sys/class/power_supply/battery/soc_decimal
 *   /sys/class/power_supply/battery/engine
 *   /sys/class/power_supply/battery/charging_speed
 *   /sys/class/power_supply/battery/board_temp_state
 *   /sys/class/power_supply/usb/present
 *   /sys/class/cms_class/...         (engine, adapter_power, mode flags)
 *
 * The battery nodes feed the vivo health HAL (from the OriginOS vendor),
 * which packs them into the vendor bitfields of HealthInfo.  The cms_class
 * nodes are what the HAL actually reads for the packed charge state; engine
 * is derived from the real USB type so the UI shows flash charging only for
 * PD, and adapter_power from real VBUS/IBUS.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#define DRV_NAME "bsts"
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#define QCOM_BATT_PATH(_name) "/sys/class/qcom-battery/" _name
static int qcom_batt_read_int(const char *path)
{
	struct file *f;
	char buf[32];
	ssize_t n;
	loff_t pos = 0;
	int val = 0;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return 0;
	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	if (kstrtoint(strim(buf), 10, &val))
		return 0;
	return val;
}
#define BSTS_RETRY_MS 1000
static struct power_supply *battery_psy;
static struct power_supply *usb_psy;
static DEFINE_MUTEX(compat_lock);
static struct delayed_work bsts_work;
static bool bsts_exiting;
static bool bsts_waiting;
static bool have_chg_type;
static bool have_fg_passed_charge;
static bool have_soc_decimal;
static bool have_engine;
static bool have_charging_speed;
static bool have_board_temp_state;
static bool have_usb_present;
static struct class *cms_class;
static struct class *fuelsummary_class;
static int battery_soh(void)
{
	union power_supply_propval full = { 0 };
	union power_supply_propval design = { 0 };
	int soh = qcom_batt_read_int(QCOM_BATT_PATH("soh"));
	if (soh > 0 && soh <= 100)
		return soh;
	mutex_lock(&compat_lock);
	if (!battery_psy ||
	    power_supply_get_property(battery_psy, POWER_SUPPLY_PROP_CHARGE_FULL,
				      &full) ||
	    power_supply_get_property(battery_psy,
				      POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
				      &design) ||
	    design.intval <= 0) {
		mutex_unlock(&compat_lock);
		return 0;
	}
	mutex_unlock(&compat_lock);
	return (int)div64_u64((u64)full.intval * 100, design.intval);
}
static int fg_passed_charge = -1;
module_param(fg_passed_charge, int, 0644);
MODULE_PARM_DESC(fg_passed_charge,
	"fg_passed_charge override (-1=derive, 0 or 1=force)");
static const char *usb_type_name(enum power_supply_usb_type type)
{
	switch (type) {
	case POWER_SUPPLY_USB_TYPE_SDP:     return "SDP";
	case POWER_SUPPLY_USB_TYPE_DCP:     return "DCP";
	case POWER_SUPPLY_USB_TYPE_CDP:     return "CDP";
	case POWER_SUPPLY_USB_TYPE_ACA:     return "ACA";
	case POWER_SUPPLY_USB_TYPE_C:       return "C";
	case POWER_SUPPLY_USB_TYPE_PD:      return "PD";
	case POWER_SUPPLY_USB_TYPE_PD_DRP:  return "PD_DRP";
	case POWER_SUPPLY_USB_TYPE_PD_PPS:  return "PD_PPS";
	case POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID:
		return "BrickID";
	case POWER_SUPPLY_USB_TYPE_UNKNOWN:
	default:                            return "Unknown";
	}
}
static ssize_t chg_type_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	union power_supply_propval val;
	int rc;
	const char *name = "Unknown";
	mutex_lock(&compat_lock);
	if (!usb_psy) {
		mutex_unlock(&compat_lock);
		return sysfs_emit(buf, "Unknown\n");
	}
	rc = power_supply_get_property(usb_psy,
		POWER_SUPPLY_PROP_USB_TYPE, &val);
	if (!rc)
		name = usb_type_name(val.intval);
	mutex_unlock(&compat_lock);
	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(chg_type);
static ssize_t usb_present_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	union power_supply_propval val;
	int present = 0;
	int rc;
	mutex_lock(&compat_lock);
	if (usb_psy) {
		rc = power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_ONLINE, &val);
		if (!rc)
			present = !!val.intval;
	}
	mutex_unlock(&compat_lock);
	return sysfs_emit(buf, "%d\n", present);
}
static DEVICE_ATTR(present, 0444, usb_present_show, NULL);
static ssize_t fg_passed_charge_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	union power_supply_propval full = { 0 };
	union power_supply_propval design = { 0 };
	int passed;
	int rc_full;
	int rc_design;
	if (READ_ONCE(fg_passed_charge) >= 0)
		return sysfs_emit(buf, "%d\n", !!READ_ONCE(fg_passed_charge));
	mutex_lock(&compat_lock);
	if (!battery_psy) {
		mutex_unlock(&compat_lock);
		return sysfs_emit(buf, "0\n");
	}
	rc_full = power_supply_get_property(battery_psy,
		POWER_SUPPLY_PROP_CHARGE_FULL, &full);
	rc_design = power_supply_get_property(battery_psy,
		POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &design);
	passed = !rc_full && !rc_design && full.intval > 0 && design.intval > 0;
	mutex_unlock(&compat_lock);
	return sysfs_emit(buf, "%d\n", passed);
}
static DEVICE_ATTR_RO(fg_passed_charge);
static ssize_t soc_decimal_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	union power_supply_propval val = { 0 };
	int rc;
	mutex_lock(&compat_lock);
	if (!battery_psy) {
		mutex_unlock(&compat_lock);
		return sysfs_emit(buf, "0\n");
	}
	rc = power_supply_get_property(battery_psy,
		POWER_SUPPLY_PROP_CAPACITY, &val);
	mutex_unlock(&compat_lock);
	if (rc || val.intval < 0 || val.intval > 100)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%d\n", val.intval * 100);
}
static DEVICE_ATTR_RO(soc_decimal);
static int derive_engine(void)
{
	union power_supply_propval online = { 0 };
	union power_supply_propval type = { 0 };
	int engine = 0;
	mutex_lock(&compat_lock);
	if (usb_psy &&
	    !power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_ONLINE, &online) && online.intval &&
	    !power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_USB_TYPE, &type)) {
		if (type.intval == POWER_SUPPLY_USB_TYPE_PD ||
		    type.intval == POWER_SUPPLY_USB_TYPE_PD_DRP ||
		    type.intval == POWER_SUPPLY_USB_TYPE_PD_PPS)
			engine = 3;
	}
	mutex_unlock(&compat_lock);
	return engine;
}
static ssize_t engine_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", derive_engine());
}
static DEVICE_ATTR_RO(engine);
/*
 * Vivo charging speed tier (4 bits, 0-15) carried in the battery-changed
 * broadcast: the health HAL reads it every cycle and logs it as "cs=", then
 * packs it into the vendor bitfield.
 *
 * On a vivo device the value is POWER_SUPPLY_PROP_CHARGING_SPEED (enum 175,
 * served by their charging-IC stack - the cms class attribute is just a
 * wrapper around power_supply_get_property()).  fuxi has no such property, but
 * it does expose the real input power, which is what the tier encodes, so
 * derive a monotonic tier from USB V*I:
 *
 *   0 = not charging, 1 = slow (< 7.5 W), 2 = normal (7.5-18 W),
 *   3 = fast (18-40 W), 4 = super (> 40 W)
 */
#define BSTS_SPEED_NORMAL_MIN_MW	7500
#define BSTS_SPEED_FAST_MIN_MW		18000
#define BSTS_SPEED_SUPER_MIN_MW		40000
static ssize_t charging_speed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	union power_supply_propval online = { 0 };
	union power_supply_propval v = { 0 };
	union power_supply_propval i = { 0 };
	int speed = 0;
	mutex_lock(&compat_lock);
	if (usb_psy &&
	    !power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_ONLINE,
				       &online) && online.intval &&
	    !power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW,
				       &v) &&
	    !power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_CURRENT_NOW,
				       &i) &&
	    v.intval > 0 && i.intval > 0) {
		s64 mw = div_s64((s64)(v.intval / 1000) * (i.intval / 1000), 1000);
		if (mw >= BSTS_SPEED_SUPER_MIN_MW)
			speed = 4;
		else if (mw >= BSTS_SPEED_FAST_MIN_MW)
			speed = 3;
		else if (mw >= BSTS_SPEED_NORMAL_MIN_MW)
			speed = 2;
		else
			speed = 1;
	}
	mutex_unlock(&compat_lock);
	return sysfs_emit(buf, "%d\n", speed);
}
static DEVICE_ATTR_RO(charging_speed);
static ssize_t board_temp_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int temp = qcom_batt_read_int(QCOM_BATT_PATH("connector_temp"));
	return sysfs_emit(buf, "%d\n", temp >= 450 ? 1 : 0);
}
static DEVICE_ATTR_RO(board_temp_state);
static ssize_t cms_engine_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", derive_engine());
}
static struct class_attribute cms_engine_attr = {
	.attr = { .name = "engine", .mode = 0444 },
	.show = cms_engine_show,
};
static ssize_t cms_adapter_power_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	union power_supply_propval v = { 0 };
	union power_supply_propval i = { 0 };
	u64 watts = 0;
	mutex_lock(&compat_lock);
	if (usb_psy &&
	    !power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &v) &&
	    !power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_CURRENT_NOW, &i) &&
	    v.intval > 0 && i.intval > 0)
		watts = div_u64((u64)(v.intval / 1000) * (i.intval / 1000),
				1000000);
	mutex_unlock(&compat_lock);
	return sysfs_emit(buf, "%llu\n", watts);
}
static struct class_attribute cms_adapter_power_attr = {
	.attr = { .name = "adapter_power", .mode = 0444 },
	.show = cms_adapter_power_show,
};
#define CMS_ATTR_RO_ZERO(_name)						\
static ssize_t cms_##_name##_show(struct class *cls,			\
		struct class_attribute *attr, char *buf)		\
{									\
	return sysfs_emit(buf, "0\n");					\
}									\
static struct class_attribute cms_##_name##_attr = {			\
	.attr = { .name = __stringify(_name), .mode = 0444 },		\
	.show = cms_##_name##_show,					\
}
CMS_ATTR_RO_ZERO(batt_last_mode_flag);
CMS_ATTR_RO_ZERO(batt_last_mode_time);
static ssize_t cms_batt_empty_notify_show(struct class *cls,
		struct class_attribute *attr, char *buf)
{
	union power_supply_propval val = { 0 };
	int empty = 0;
	mutex_lock(&compat_lock);
	if (battery_psy &&
	    !power_supply_get_property(battery_psy,
			POWER_SUPPLY_PROP_CAPACITY, &val))
		empty = val.intval <= 1;
	mutex_unlock(&compat_lock);
	return sysfs_emit(buf, "%d\n", empty);
}
static struct class_attribute cms_batt_empty_notify_attr = {
	.attr = { .name = "batt_empty_notify", .mode = 0444 },
	.show = cms_batt_empty_notify_show,
};
static int ps_read_int(struct power_supply **psy, enum power_supply_property prop, int def);
static int read_file_int(const char *path, int def);
static bool wls_tx_want;
static unsigned long wls_tx_req_jiffies = INITIAL_JIFFIES;
static bool wls_saw_device;
static int wls_tx_enabled(void)
{
	int state, mode, boost;
	if (time_before(jiffies, wls_tx_req_jiffies + msecs_to_jiffies(3000)))
		return READ_ONCE(wls_tx_want) ? 1 : 0;
	state = read_file_int(QCOM_BATT_PATH("reverse_chg_state"), -1);
	mode = read_file_int(QCOM_BATT_PATH("reverse_chg_mode"), -1);
	boost = read_file_int(QCOM_BATT_PATH("wireless_boost_en"), -1);
	if (state == 4 && (mode > 0 || boost > 0))
		return 1;
	if (READ_ONCE(wls_saw_device) && state != 4)
		return 0;
	if (mode < 0 && boost < 0)
		return 0;
	return (mode > 0 || boost > 0) ? 1 : 0;
}
#define QCOM_BATT_FMT	"/sys/class/qcom-battery/%s"
static struct delayed_work wls_tx_work;
static void qcom_write_int(const char *name, int val)
{
	struct file *f;
	char path[128];
	char buf[16];
	int len;
	loff_t pos = 0;
	snprintf(path, sizeof(path), QCOM_BATT_FMT, name);
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f)) {
		pr_warn(DRV_NAME ": open %s failed: %ld\n", path, PTR_ERR(f));
		return;
	}
	len = scnprintf(buf, sizeof(buf), "%d", val);
	if (kernel_write(f, buf, len, &pos) < 0)
		pr_debug(DRV_NAME ": write %s=%d failed\n", name, val);
	filp_close(f, NULL);
}
static void wls_tx_apply(bool on)
{

	qcom_write_int("reverse_chg_mode", on ? 1 : 0);
}
#define WLS_NOTIFY_TIMES	6
#define WLS_NOTIFY_MS		1500
static int wls_notify_left;
static struct delayed_work wls_notify_work;
static void wls_notify_work_fn(struct work_struct *work)
{
	if (battery_psy)
		power_supply_changed(battery_psy);
	if (--wls_notify_left > 0)
		schedule_delayed_work(&wls_notify_work, msecs_to_jiffies(WLS_NOTIFY_MS));
}
static void wls_tx_work_fn(struct work_struct *work)
{
	bool on = READ_ONCE(wls_tx_want);
	int state;
	wls_tx_apply(on);
	state = read_file_int(QCOM_BATT_PATH("reverse_chg_state"), -1);
	pr_info(DRV_NAME ": wpc_tx=%d reverse_chg_state=%d\n", on, state);
	if (battery_psy &&
	    (state == 4 ||
	     ps_read_int(&battery_psy, POWER_SUPPLY_PROP_CURRENT_NOW, 0) > 0)) {
		power_supply_changed(battery_psy);
		wls_notify_left = WLS_NOTIFY_TIMES;
		schedule_delayed_work(&wls_notify_work, msecs_to_jiffies(WLS_NOTIFY_MS));
	}
}
#define WLS_POLL_MS	1000
static struct delayed_work wls_poll_work;
static int wls_last_reported = -1;
static void wls_poll_work_fn(struct work_struct *work)
{
	int state = read_file_int(QCOM_BATT_PATH("reverse_chg_state"), -1);
	int mode = read_file_int(QCOM_BATT_PATH("reverse_chg_mode"), -1);
	int boost = read_file_int(QCOM_BATT_PATH("wireless_boost_en"), -1);
	int val;

	if (state == 4)
		WRITE_ONCE(wls_saw_device, true);

	val = wls_tx_enabled();
	if (val != READ_ONCE(wls_last_reported)) {
		pr_info(DRV_NAME ": wls_otg_status %d -> %d (state=%d mode=%d boost=%d)\n",
			READ_ONCE(wls_last_reported), val, state, mode, boost);
		WRITE_ONCE(wls_last_reported, val);
		if (battery_psy)
			power_supply_changed(battery_psy);
	}

	if (mode > 0 || boost > 0 || state == 4) {
		schedule_delayed_work(&wls_poll_work,
				      msecs_to_jiffies(WLS_POLL_MS));
	} else {
		WRITE_ONCE(wls_saw_device, false);
		WRITE_ONCE(wls_last_reported, 0);
	}
}
static int write_int_file(const char *path, int val)
{
	struct file *f;
	char buf[16];
	int len;
	loff_t pos = 0;
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return (int)PTR_ERR(f);
	len = scnprintf(buf, sizeof(buf), "%d", val);
	if (kernel_write(f, buf, len, &pos) < 0) {
		filp_close(f, NULL);
		return -EIO;
	}
	filp_close(f, NULL);
	return 0;
}
static bool bypass_want;
static int bypass_applied = -1;
static struct delayed_work bypass_work;
static bool usb_is_pd(void)
{
	int t = ps_read_int(&usb_psy, POWER_SUPPLY_PROP_USB_TYPE, -1);

	return t == POWER_SUPPLY_USB_TYPE_PD ||
	       t == POWER_SUPPLY_USB_TYPE_PD_DRP ||
	       t == POWER_SUPPLY_USB_TYPE_PD_PPS;
}

static void bypass_work_fn(struct work_struct *work)
{
	bool want = READ_ONCE(bypass_want);
	bool pd = usb_is_pd();
	bool on = want && pd;

	if ((int)on != READ_ONCE(bypass_applied)) {
		char path[128];
		int rc;

		snprintf(path, sizeof(path), QCOM_BATT_FMT, "restrict_chg");
		rc = write_int_file(path, on ? 1 : 0);
		WRITE_ONCE(bypass_applied, on);
		pr_info(DRV_NAME ": bypass %d (pd=%d want=%d) restrict_chg rc=%d\n",
			on, pd, want, rc);
	}
	if (want)
		pr_info(DRV_NAME ": bypass state on=%d status=%d ibat=%d uA usb=%d uA cap=%d\n",
			READ_ONCE(bypass_applied),
			ps_read_int(&battery_psy, POWER_SUPPLY_PROP_STATUS, -1),
			ps_read_int(&battery_psy, POWER_SUPPLY_PROP_CURRENT_NOW, 0),
			ps_read_int(&usb_psy, POWER_SUPPLY_PROP_CURRENT_NOW, 0),
			ps_read_int(&battery_psy, POWER_SUPPLY_PROP_CAPACITY, -1));
	if (want)
		schedule_delayed_work(&bypass_work, msecs_to_jiffies(5000));
}
static void bypass_apply(bool on)
{
	WRITE_ONCE(bypass_want, on);
	mod_delayed_work(system_wq, &bypass_work, 0);
}
#define SETTINGS_SEC_PATH "/data/system/users/0/settings_secure.xml"
#define SETTINGS_SYS_PATH "/data/system/users/0/settings_system.xml"
#define SETTINGS_KEY "smart_charge_upper_value"
#define SETTINGS_MAX_READ (256 * 1024)
static int abx_lookup_int(const char *path, const char *key)
{
	struct file *f;
	char *buf;
	loff_t pos = 0;
	ssize_t n, i, j;
	size_t klen = strlen(key);
	int ret = -1;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return -1;
	buf = kmalloc(SETTINGS_MAX_READ, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return -1;
	}
	n = kernel_read(f, buf, SETTINGS_MAX_READ, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		goto out;
	ret = -2;
	for (i = 0; i + (ssize_t)klen + 6 < n; i++) {
		if (memcmp(buf + i, key, klen))
			continue;
		for (j = i + klen; j + 6 < n && j < i + klen + 96; j++) {
			unsigned int len, val = 0, d;
			if ((u8)buf[j] != 0x2f || (u8)buf[j + 1] != 0x00 ||
			    (u8)buf[j + 2] != 0x05)
				continue;
			len = ((u8)buf[j + 3] << 8) | (u8)buf[j + 4];
			if (!len || len > 8)
				break;
			for (d = 0; d < len; d++) {
				char c = buf[j + 5 + d];
				if (c < '0' || c > '9')
					break;
				val = val * 10 + (c - '0');
			}
			if (d == len) {
				ret = (int)val;
				goto out;
			}
			break;
		}
	}
out:
	kfree(buf);
	return ret;
}
static int settings_target(void)
{
	int v = abx_lookup_int(SETTINGS_SEC_PATH, SETTINGS_KEY);
	if (v == -1)
		return -1;
	if (v == -2) {
		v = abx_lookup_int(SETTINGS_SYS_PATH, SETTINGS_KEY);
		if (v < 0)
			return -1;
	}
	return v >= 100 ? 0 : v;
}
static struct delayed_work soc_limit_work;
static int soc_limit = -1;
#define SOC_HYST	2
static int limit_mech;
module_param(limit_mech, int, 0644);
static int target_from_settings = 1;
module_param(target_from_settings, int, 0644);
static bool soc_from_settings;
static int soc_limit_applied;
static int soc_override = -2;
static int soc_override_ignore = -3;
static int soc_node_val = -1;
static int soc_limit_param_set(const char *val, const struct kernel_param *kp)
{
	int v = 0;
	int rc = kstrtoint(val, 10, &v);
	if (rc)
		return rc;
	WRITE_ONCE(soc_override, v);
	WRITE_ONCE(soc_override_ignore, READ_ONCE(soc_limit));
	WRITE_ONCE(soc_limit, v);
	mod_delayed_work(system_wq, &soc_limit_work, 0);
	pr_info(DRV_NAME ": charge limit override -> %d%% (ignoring node %d)\n",
		v, READ_ONCE(soc_override_ignore));
	return 0;
}
static const struct kernel_param_ops soc_limit_ops = {
	.set = soc_limit_param_set,
	.get = param_get_int,
};
module_param_cb(soc_limit, &soc_limit_ops, &soc_limit, 0644);
MODULE_PARM_DESC(soc_limit, "charge limit in % (0/negative disables)");
static void soc_limit_work_fn(struct work_struct *work)
{
	int cap = ps_read_int(&battery_psy, POWER_SUPPLY_PROP_CAPACITY, -1);
	int limit = READ_ONCE(soc_limit);
	int mech = READ_ONCE(limit_mech);
	bool on, target_changed = false;
	if (READ_ONCE(target_from_settings)) {
		int st = settings_target();

		if (st >= 0) {
			WRITE_ONCE(soc_from_settings, true);
			if (st != limit) {
				pr_info(DRV_NAME ": target %d%% from settings\n",
					st);
				WRITE_ONCE(soc_limit, st);
				limit = st;
				target_changed = true;
			}
		}
	}
	if (limit > 0 && limit <= 100 && cap >= 0) {
		if (!soc_limit_applied || target_changed)
			on = cap >= limit;
		else
			on = cap > limit - SOC_HYST;
	} else {
		on = false;
	}
	if (on == soc_limit_applied && mech == 0) {
		int cur = read_file_int(QCOM_BATT_PATH("input_suspend"), -1);
		if (cur >= 0 && (cur > 0) != on)
			soc_limit_applied = !on;
	}
	if (on != soc_limit_applied) {
		soc_limit_applied = on;
		if (mech == 1)
			write_int_file(QCOM_BATT_PATH("restrict_chg"),
				       on ? 1 : 0);
		else
			write_int_file(QCOM_BATT_PATH("input_suspend"),
				       on ? 1 : 0);
		pr_info(DRV_NAME ": charge limit %d%% (mech %d): cap=%d -> %s\n",
			limit, mech, cap, on ? "stop charging" : "resume");
	}
	if ((limit > 0 && limit <= 100) ||
	    READ_ONCE(target_from_settings))
		schedule_delayed_work(&soc_limit_work,
				      msecs_to_jiffies(15000));
}
static void soc_limit_set(const char *str)
{
	const char *p = str;
	int v = 0;
	while (*p) {
		if (*p >= '0' && *p <= '9') {
			int n = 0;
			while (*p >= '0' && *p <= '9')
				n = n * 10 + (*p++ - '0');
			v = n;
			continue;
		}
		p++;
	}
	WRITE_ONCE(soc_node_val, v);
	if (READ_ONCE(soc_from_settings)) {
		pr_info(DRV_NAME ": node '%s' (from %s/%d) mirrored, target stays %d%%\n",
			str, current->comm, current->pid,
			READ_ONCE(soc_limit));
		mod_delayed_work(system_wq, &soc_limit_work, msecs_to_jiffies(300));
		return;
	}
	if (READ_ONCE(soc_override) != -2) {
		if (v == READ_ONCE(soc_override_ignore)) {
			pr_info(DRV_NAME ": ignoring node echo %d (override)\n",
				v);
			return;
		}
		WRITE_ONCE(soc_override, -2);
	}
	if (v <= 0) {
		pr_info(DRV_NAME ": ignoring zero from %s/%d, keeping %d%%\n",
			current->comm, current->pid, READ_ONCE(soc_limit));
		return;
	}
	WRITE_ONCE(soc_limit, v);
	pr_info(DRV_NAME ": charge limit %d%% (node '%s', from %s/%d)\n",
		v, str, current->comm, current->pid);
	mod_delayed_work(system_wq, &soc_limit_work, msecs_to_jiffies(2000));
}
static void wls_tx_request(char *val)
{
	int v;
	if (kstrtoint(strim(val), 10, &v))
		v = 0;
	pr_info(DRV_NAME ": wpc_tx '%s' from %s(%d)\n", val,
		current->comm, current->pid);
	WRITE_ONCE(wls_tx_want, v > 0);
	if (v > 0)
		WRITE_ONCE(wls_saw_device, false);
	WRITE_ONCE(wls_tx_req_jiffies, jiffies);
	mod_delayed_work(system_wq, &wls_tx_work, 0);
	mod_delayed_work(system_wq, &wls_poll_work, msecs_to_jiffies(200));
}
static ssize_t cms_wls_otg_status_show(struct class *cls,
		struct class_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", wls_tx_enabled());
}
static struct class_attribute cms_wls_otg_status_attr = {
	.attr = { .name = "wls_otg_status", .mode = 0444 },
	.show = cms_wls_otg_status_show,
};
static ssize_t cms_md_status_show(struct class *cls,
		struct class_attribute *attr, char *buf)
{
	int v = qcom_batt_read_int(QCOM_BATT_PATH("moisture_detection_status"));
	if (v < 0)
		v = 0;
	if (v > 3)
		v = 3;
	return sysfs_emit(buf, "%d\n", v);
}
static struct class_attribute cms_md_status_attr = {
	.attr = { .name = "md_status", .mode = 0444 },
	.show = cms_md_status_show,
};
static ssize_t cms_bat_wpc_tx_chg_state_show(struct class *cls,
		struct class_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", wls_tx_enabled());
}
static struct class_attribute cms_bat_wpc_tx_chg_state_attr = {
	.attr = { .name = "bat_wpc_tx_chg_state", .mode = 0444 },
	.show = cms_bat_wpc_tx_chg_state_show,
};
static ssize_t cms_bat_wpc_tx_to_tx_state_show(struct class *cls,
		struct class_attribute *attr, char *buf)
{
	int mode = qcom_batt_read_int(QCOM_BATT_PATH("reverse_chg_mode"));
	int state = qcom_batt_read_int(QCOM_BATT_PATH("reverse_chg_state"));
	return sysfs_emit(buf, "%d\n", (mode && !state) ? 1 : 0);
}
static struct class_attribute cms_bat_wpc_tx_to_tx_state_attr = {
	.attr = { .name = "bat_wpc_tx_to_tx_state", .mode = 0444 },
	.show = cms_bat_wpc_tx_to_tx_state_show,
};
static struct class_attribute *cms_attrs[] = {
	&cms_engine_attr,
	&cms_adapter_power_attr,
	&cms_batt_last_mode_flag_attr,
	&cms_batt_last_mode_time_attr,
	&cms_batt_empty_notify_attr,
	&cms_wls_otg_status_attr,
	&cms_md_status_attr,
	&cms_bat_wpc_tx_chg_state_attr,
	&cms_bat_wpc_tx_to_tx_state_attr,
};
/*
 * ---------------------------------------------------------------------------
 * Extended cms_class surface.
 *
 * The vivo kernel "cms" (charging management system) driver exposes a large
 * class of nodes consumed by the OriginOS charging stack, the vivo health HAL
 * (health_vivo), com.vivo.fuelsummary, com.vivo.pie/pem and init.vivo.rc.
 *   - values are derived from the real QTI power-supply / qcom-battery state
 *     whenever an equivalent exists (currents, voltages, temperatures, SOH,
 *     backlight, moisture, USB type),
 *   - control nodes that userspace writes (init.vivo.rc properties, FuelSummary
 *     AI charge votes, PEM scenes) are writable storage that echoes the last
 *     write, which is what the real driver does for them.
 * ---------------------------------------------------------------------------
 */
#define BACKLIGHT_BRIGHTNESS_PATH \
	"/sys/class/backlight/panel0-backlight/brightness"
#define BATTERY_MODEL_PATH "/sys/class/power_supply/battery/model_name"
enum cms_kind {
	CMS_K_STORE,
	CMS_K_ROS,
	CMS_K_PS,
	CMS_K_QCOM,
	CMS_K_STRFILE,
	CMS_K_CONST,
	CMS_K_USBTYPE,
	CMS_K_FN,
	CMS_K_STOREFILE,
	CMS_K_STOREBIG,
};
struct cms_node {
	struct class_attribute cattr;
	enum cms_kind kind;
	int uid;
	int gid;
	struct power_supply **psy;
	enum power_supply_property prop;
	const char *path;
	int div;
	int val;
	int (*fn)(char *buf, size_t len);
	char storage[64];
	char *big;
	unsigned int big_size;
};
#define CMS_PS(_name, _psy, _prop, _div)				\
	{ .cattr = { .attr = { .name = _name, .mode = 0444 } }, .kind = CMS_K_PS,	\
	  .psy = (_psy), .prop = (_prop), .div = (_div) }
#define CMS_QCOM(_name, _file, _div, _def)				\
	{ .cattr = { .attr = { .name = _name, .mode = 0444 } }, .kind = CMS_K_QCOM,	\
	  .path = QCOM_BATT_PATH(_file), .div = (_div), .val = (_def) }
#define CMS_CONST(_name, _val)						\
	{ .cattr = { .attr = { .name = _name, .mode = 0444 } }, .kind = CMS_K_CONST, .val = (_val) }
#define CMS_STRFILE(_name, _path)					\
	{ .cattr = { .attr = { .name = _name, .mode = 0444 } }, .kind = CMS_K_STRFILE, .path = (_path) }
#define CMS_USBTYPE(_name)						\
	{ .cattr = { .attr = { .name = _name, .mode = 0444 } }, .kind = CMS_K_USBTYPE }
#define CMS_FN(_name, _fn)						\
	{ .cattr = { .attr = { .name = _name, .mode = 0444 } }, .kind = CMS_K_FN, .fn = (_fn) }
#define CMS_ROS(_name)							\
	{ .cattr = { .attr = { .name = _name, .mode = 0444 } }, .kind = CMS_K_ROS }
#define CMS_RW(_name, _mode, _uid, _gid)				\
	{ .cattr = { .attr = { .name = _name, .mode = (_mode) } }, .kind = CMS_K_STORE, \
	  .uid = (_uid), .gid = (_gid), .storage = "0" }
#define CMS_RW_D(_name, _mode, _uid, _gid, _def)			\
	{ .cattr = { .attr = { .name = _name, .mode = (_mode) } }, .kind = CMS_K_STORE, \
	  .uid = (_uid), .gid = (_gid), .storage = _def }
#define CMS_RWBIG(_name, _buf, _mode, _uid, _gid)			\
	{ .cattr = { .attr = { .name = _name, .mode = (_mode) } }, .kind = CMS_K_STOREBIG, \
	  .uid = (_uid), .gid = (_gid), .big = (_buf), .big_size = sizeof(_buf) }
#define CMS_STOREFILE(_name, _mode, _uid, _gid, _path)			\
	{ .cattr = { .attr = { .name = _name, .mode = (_mode) } }, .kind = CMS_K_STOREFILE, \
	  .uid = (_uid), .gid = (_gid), .path = (_path) }
static int ps_read_int(struct power_supply **psy,
		enum power_supply_property prop, int def)
{
	union power_supply_propval val = { 0 };
	int rc;
	mutex_lock(&compat_lock);
	if (!*psy) {
		mutex_unlock(&compat_lock);
		return def;
	}
	rc = power_supply_get_property(*psy, prop, &val);
	mutex_unlock(&compat_lock);
	return rc ? def : val.intval;
}
static int read_file_int(const char *path, int def)
{
	struct file *f;
	char buf[64];
	ssize_t n;
	loff_t pos = 0;
	int val;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return def;
	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return def;
	buf[n] = '\0';
	if (kstrtoint(strim(buf), 10, &val))
		return def;
	return val;
}
static int read_file_str(const char *path, char *out, size_t len)
{
	struct file *f;
	ssize_t n;
	loff_t pos = 0;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return -ENOENT;
	n = kernel_read(f, out, len - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0)
		return -EIO;
	out[n] = '\0';
	strim(out);
	return 0;
}
static const char *usb_type_str(void)
{
	union power_supply_propval val = { 0 };
	const char *name = "Unknown";
	mutex_lock(&compat_lock);
	if (usb_psy &&
	    !power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_USB_TYPE, &val))
		name = usb_type_name(val.intval);
	mutex_unlock(&compat_lock);
	return name;
}
static int weak_charger_show(char *buf, size_t len)
{
	union power_supply_propval online = { 0 };
	union power_supply_propval max = { 0 };
	int weak = 0;
	mutex_lock(&compat_lock);
	if (usb_psy &&
	    !power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_ONLINE, &online) && online.intval &&
	    !power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_CURRENT_MAX, &max))
		weak = max.intval <= 500000;
	mutex_unlock(&compat_lock);
	return scnprintf(buf, len, "%d\n", weak);
}
static int coulomb_show(char *buf, size_t len)
{
	int v = ps_read_int(&battery_psy, POWER_SUPPLY_PROP_CHARGE_COUNTER, 0);
	if (v >= 100000)
		v /= 1000;
	return scnprintf(buf, len, "%d\n", v);
}
static int ui_soh_show(char *buf, size_t len)
{
	return scnprintf(buf, len, "%d\n", battery_soh());
}
#define VIVO_HEALTH_BATT_OT	0x0002
#define VIVO_HEALTH_BATT_OV	0x0008
#define VIVO_HEALTH_TEMP_LOW 0x0020
static int vivo_health_status(void)
{
	switch (ps_read_int(&battery_psy, POWER_SUPPLY_PROP_HEALTH, -1)) {
	case POWER_SUPPLY_HEALTH_OVERHEAT:
		return VIVO_HEALTH_BATT_OT;
	case POWER_SUPPLY_HEALTH_OVERVOLTAGE:
		return VIVO_HEALTH_BATT_OV;
	case POWER_SUPPLY_HEALTH_COLD:
		return VIVO_HEALTH_TEMP_LOW;
	default:
		return 0;
	}
}
static int health_status_show(char *buf, size_t len)
{
	return scnprintf(buf, len, "%d\n", vivo_health_status());
}
static char soh_loss_table_buf[1024];
static char loss_condition_sum_table_buf[4096];
static struct cms_node cms_nodes[] = {
	CMS_USBTYPE("chg_type"),
	CMS_PS("ibus", &usb_psy, POWER_SUPPLY_PROP_CURRENT_NOW, 1000),
	CMS_PS("vchg", &usb_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, 1000),
	CMS_PS("ibat", &battery_psy, POWER_SUPPLY_PROP_CURRENT_NOW, 1000),
	CMS_QCOM("tboard", "connector_temp", 0, 0),
	CMS_PS("vcell1_voltage", &battery_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, 1000),
	CMS_CONST("vcell2_voltage", 0),	/* fuxi is single-cell */
	CMS_FN("coulomb", coulomb_show),
	CMS_PS("cycle", &battery_psy, POWER_SUPPLY_PROP_CYCLE_COUNT, 0),
	CMS_PS("health", &battery_psy, POWER_SUPPLY_PROP_HEALTH, 0),
	CMS_PS("capacity_mah_from_battery_health", &battery_psy, POWER_SUPPLY_PROP_CHARGE_FULL, 1000),
	CMS_QCOM("fg_raw_soc", "fg1_rsoc", 0, 0),
	CMS_QCOM("aging_soh", "soh", 0, 100),
	CMS_QCOM("soh_from_battery_health", "soh", 0, 100),
	CMS_FN("ui_soh", ui_soh_show),
	CMS_PS("avg_voltage_now", &battery_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, 1000),
	CMS_QCOM("vfloat_voltage", "fg1_vol_max", 1000, 4400),
	CMS_CONST("shutdown_voltage", 3200),
	CMS_FN("weakcharger", weak_charger_show),
	CMS_QCOM("md_scene", "moisture_detection_status", 0, 0),
	CMS_QCOM("md_data", "moisture_detection_status", 0, 0),
	CMS_FN("health_status", health_status_show),
	CMS_FN("health_status_bak", health_status_show),
	CMS_CONST("health_status_errno", 0),
	CMS_STRFILE("batid", BATTERY_MODEL_PATH),
	CMS_STOREFILE("backlight_brightness", 0664, 1000, 1000, BACKLIGHT_BRIGHTNESS_PATH),
	CMS_ROS("predict_chg_time_data"),
	/* ---- telemetry blobs the FuelSummary/HAL read (driver-filled) ---- */
	CMS_ROS("adapter_error_code2_bigdata_info"),
	CMS_ROS("battery_health_info"),
	CMS_ROS("battery_r_bigdata_info"),
	CMS_ROS("dual_battery_r_bigdata_info"),
	CMS_ROS("eis_bigdata_info"),
	CMS_ROS("fg_bigdata_info"),
	CMS_ROS("heating_data"),
	CMS_ROS("isp_bigdata_info"),
	CMS_ROS("plugout_misc_bigdata"),
	CMS_ROS("poweroff_bigdata_info"),
	CMS_ROS("soc_info"),
	CMS_ROS("soh_bigdata"),
	CMS_ROS("vbat_rise_up_bigdata_info"),
	CMS_ROS("date_changed_misc_bigdata"),
	CMS_RWBIG("loss_condition_sum_table", loss_condition_sum_table_buf, 0666, 1000, 1000),
	CMS_RWBIG("soh_loss_table", soh_loss_table_buf, 0666, 1000, 1000),
	CMS_RW_D("soh_formula", 0444, 1000, 1000, "no formula"),
	CMS_RW_D("soh_loss_gamma_param", 0666, 1000, 1000, "0"),
	CMS_RW_D("soh_loss_theta_param", 0666, 1000, 1000, "0"),
	CMS_RW_D("battery_health_learning_capacity_state", 0666, 1000, 1000, "0"),
	CMS_QCOM("battery_health_learning_soh", "soh", 0, 0),
	CMS_RW("cms_enable", 0664, 1000, 0),
	CMS_RW("fixed_freq", 0777, 1000, 0),
	CMS_RW("scene_table", 0666, 1000, 0),
	CMS_RW("scene_current", 0666, 1000, 0),
	CMS_RW("keep_soc", 0664, 1000, 1000),
	CMS_RW("custom_dis_otg", 0664, 1000, 1000),
	CMS_RW("ai_charge_rate_mode", 0664, 1000, 1000),
	CMS_RW("last_mode_notify_flag", 0664, 1000, 1000),
	CMS_RW("game_bypass_charge_flag", 0664, 1000, 1000),
	CMS_RW("magnetic_hall_mode", 0664, 1000, 1000),
	CMS_RW("camera_open_state", 0664, 1000, 1000),
	CMS_RW("otg_silicon_batt_low_thr", 0664, 1000, 1000),
	CMS_RW_D("soh_life_loss", 0664, 1000, 1000, "0 0"),
	CMS_RW_D("soh_ver_transition", 0664, 1000, 1000, "3"),
	CMS_RW("battery_health_framework_flag", 0664, 1000, 1000),
	CMS_RW("soh_test", 0664, 1000, 1000),
	CMS_RW("soh_test_ocv_soc", 0664, 1000, 1000),
	CMS_RW("soh_loss_event_info", 0664, 1000, 1000),
	CMS_RW("autotest", 0664, 1000, 1000),
	CMS_RW("bbklog_enabled", 0664, 1000, 1000),
	CMS_RW("calling_state", 0664, 1000, 1000),
	CMS_RW("country_region", 0664, 1000, 1000),
	CMS_RW("disable_charge", 0664, 1000, 1000),
	CMS_RW("exhibition_mode", 0664, 1000, 1000),
	CMS_RW("gsm", 0664, 1000, 1000),
	CMS_RW("keep_chg_soc", 0664, 1000, 1000),
	CMS_RW("oem_model", 0664, 1000, 1000),
	CMS_RW("panlevel", 0664, 1000, 1000),
	CMS_RW("pkg_state", 0664, 1000, 1000),
	CMS_RW("switch_state", 0664, 1000, 1000),
	CMS_RW("system_update", 0664, 1000, 1000),
	CMS_RW("weixin_calling_state", 0664, 1000, 1000),
	CMS_RW("wls_hall", 0664, 1000, 1000),
	CMS_RW("wpc_tx", 0664, 1000, 1000),
	CMS_RW("scene_mode", 0664, 1000, 1000),
	CMS_RW("scene", 0664, 1000, 1000),
	CMS_RW("scene_level", 0664, 1000, 1000),
	CMS_RW("scene_disable", 0664, 1000, 1000),
};
static ssize_t cms_node_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	struct cms_node *n = container_of(attr, struct cms_node, cattr);
	int v;
	switch (n->kind) {
	case CMS_K_STORE:
	case CMS_K_ROS:
		return sysfs_emit(buf, "%s\n", n->storage);
	case CMS_K_STOREBIG:
		return sysfs_emit(buf, "%s\n", n->big);
	case CMS_K_STOREFILE:
		if (n->storage[0])
			return sysfs_emit(buf, "%s\n", n->storage);
		v = read_file_int(n->path, n->val);
		break;
	case CMS_K_PS:
		v = ps_read_int(n->psy, n->prop, n->val);
		break;
	case CMS_K_QCOM:
		v = read_file_int(n->path, n->val);
		break;
	case CMS_K_CONST:
		v = n->val;
		break;
	case CMS_K_USBTYPE:
		return sysfs_emit(buf, "%s\n", usb_type_str());
	case CMS_K_STRFILE: {
		char tmp[64];
		if (read_file_str(n->path, tmp, sizeof(tmp)))
			tmp[0] = '\0';
		return sysfs_emit(buf, "%s\n", tmp);
	}
	case CMS_K_FN:
		return n->fn(buf, PAGE_SIZE);
	default:
		return -EINVAL;
	}
	if (n->div)
		v /= n->div;
	return sysfs_emit(buf, "%d\n", v);
}
static ssize_t cms_node_store(struct class *cls, struct class_attribute *attr, const char *buf, size_t count)
{
	struct cms_node *n = container_of(attr, struct cms_node, cattr);
	size_t len;
	if (n->kind == CMS_K_STOREBIG) {
		len = min(count, (size_t)n->big_size - 1);
		memcpy(n->big, buf, len);
		if (len && n->big[len - 1] == '\n')
			len--;
		n->big[len] = '\0';
		return count;
	}
	if (n->kind != CMS_K_STORE && n->kind != CMS_K_STOREFILE)
		return count;
	len = min(count, sizeof(n->storage) - 1);
	memcpy(n->storage, buf, len);
	if (len && n->storage[len - 1] == '\n')
		len--;
	n->storage[len] = '\0';
	if (!strcmp(n->cattr.attr.name, "wpc_tx"))
		wls_tx_request(n->storage);
	if (!strcmp(n->cattr.attr.name, "game_bypass_charge_flag")) {
		int v = 0;

		if (kstrtoint(n->storage, 10, &v))
			v = 0;
		bypass_apply(v > 0);
	}
	if (!strcmp(n->cattr.attr.name, "keep_chg_soc") ||
	    !strcmp(n->cattr.attr.name, "keep_soc"))
		soc_limit_set(n->storage);
	return count;
}
static void cms_chown(const char *name, int uid, int gid)
{
	struct path path;
	struct iattr newattrs = {
		.ia_valid = ATTR_UID | ATTR_GID,
		.ia_uid = KUIDT_INIT(uid),
		.ia_gid = KGIDT_INIT(gid),
	};
	char full[128];
	if (snprintf(full, sizeof(full), "/sys/class/cms_class/%s", name) >=
	    sizeof(full))
		return;
	if (kern_path(full, LOOKUP_FOLLOW, &path))
		return;
	notify_change(&init_user_ns, path.dentry, &newattrs, NULL);
	path_put(&path);
}
static void cms_nodes_create(void)
{
	int i;
	if (!cms_class)
		return;
	for (i = 0; i < ARRAY_SIZE(cms_nodes); i++) {
		struct cms_node *n = &cms_nodes[i];
		int rc;
		n->cattr.show = cms_node_show;
		n->cattr.store = cms_node_store;
		rc = class_create_file(cms_class, &n->cattr);
		if (rc) {
			pr_warn(DRV_NAME ": create cms %s failed: %d\n",
				n->cattr.attr.name, rc);
			continue;
		}
		if (n->uid || n->gid)
			cms_chown(n->cattr.attr.name, n->uid, n->gid);
	}
}
static void cms_class_create(void)
{
	int i;
	if (cms_class)
		return;
	cms_class = class_create(THIS_MODULE, "cms_class");
	if (IS_ERR(cms_class)) {
		cms_class = NULL;
		pr_warn(DRV_NAME ": cms_class unavailable\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(cms_attrs); i++) {
		int rc = class_create_file(cms_class, cms_attrs[i]);
		if (rc)
			pr_warn(DRV_NAME ": failed to create cms %s: %d\n",
				cms_attrs[i]->attr.name, rc);
	}
	cms_nodes_create();
}
static int battery_read_prop(enum power_supply_property prop, int *val)
{
	union power_supply_propval v = { 0 };
	int rc;
	mutex_lock(&compat_lock);
	if (!battery_psy) {
		mutex_unlock(&compat_lock);
		return -ENODEV;
	}
	rc = power_supply_get_property(battery_psy, prop, &v);
	mutex_unlock(&compat_lock);
	if (rc)
		return rc;
	*val = v.intval;
	return 0;
}
static ssize_t fuelsummary_amount_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	int counter = 0;
	int full = 0;
	int cap = 0;
	long long amount = 0;
	if (!battery_read_prop(POWER_SUPPLY_PROP_CHARGE_COUNTER, &counter)) {
		amount = (counter >= 100000) ? counter / 1000 : counter;
	} else if (!battery_read_prop(POWER_SUPPLY_PROP_CHARGE_FULL, &full) &&
		   !battery_read_prop(POWER_SUPPLY_PROP_CAPACITY, &cap)) {
		amount = (long long)full * cap / 100000;
	}
	return sysfs_emit(buf, "%lld\n", amount);
}
static ssize_t fuelsummary_age_amount_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	int full = 0;
	if (!battery_read_prop(POWER_SUPPLY_PROP_CHARGE_FULL, &full))
		full /= 1000;
	return sysfs_emit(buf, "%d\n", full);
}
static ssize_t fuelsummary_amount_total_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	int design = 0;
	if (!battery_read_prop(POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &design))
		design /= 1000;
	return sysfs_emit(buf, "%d\n", design);
}
static ssize_t fuelsummary_cycle_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	int cycle = 0;
	battery_read_prop(POWER_SUPPLY_PROP_CYCLE_COUNT, &cycle);
	return sysfs_emit(buf, "%d\n", cycle);
}
static ssize_t fuelsummary_roll_current_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	int cur = 0;
	if (!battery_read_prop(POWER_SUPPLY_PROP_CURRENT_NOW, &cur))
		cur /= 1000;
	return sysfs_emit(buf, "%d\n", cur);
}
static ssize_t fuelsummary_soh_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", battery_soh());
}

#define FUELSUM_ATTR_RO(_name)						\
static struct class_attribute fuelsummary_##_name##_attr = {		\
	.attr = { .name = __stringify(_name), .mode = 0444 },		\
	.show = fuelsummary_##_name##_show,				\
}
FUELSUM_ATTR_RO(amount);
FUELSUM_ATTR_RO(age_amount);
FUELSUM_ATTR_RO(amount_total);
FUELSUM_ATTR_RO(cycle);
FUELSUM_ATTR_RO(roll_current);
FUELSUM_ATTR_RO(soh);
static struct class_attribute *fuelsummary_attrs[] = {
	&fuelsummary_amount_attr,
	&fuelsummary_age_amount_attr,
	&fuelsummary_amount_total_attr,
	&fuelsummary_cycle_attr,
	&fuelsummary_roll_current_attr,
	&fuelsummary_soh_attr,
};
static void fuelsummary_class_create(void)
{
	int i;
	if (fuelsummary_class)
		return;
	fuelsummary_class = class_create(THIS_MODULE, "fuelsummary");
	if (IS_ERR(fuelsummary_class)) {
		fuelsummary_class = NULL;
		pr_warn(DRV_NAME ": fuelsummary class unavailable\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(fuelsummary_attrs); i++) {
		int rc = class_create_file(fuelsummary_class,
				fuelsummary_attrs[i]);
		if (rc)
			pr_warn(DRV_NAME ": failed to create fuelsummary %s: %d\n",
				fuelsummary_attrs[i]->attr.name, rc);
	}
}
static int bsts_add_file(struct device *dev, struct device_attribute *attr,
		bool *created)
{
	int rc = device_create_file(dev, attr);
	if (rc == -EEXIST) {
		pr_info(DRV_NAME ": %s already provided, skipping\n",
			attr->attr.name);
		return 0;
	}
	if (rc)
		return rc;
	*created = true;
	return 0;
}
static void remove_attrs(void);
static int add_attrs(void)
{
	int rc;
	battery_psy = power_supply_get_by_name("battery");
	usb_psy = power_supply_get_by_name("usb");
	if (!battery_psy || !usb_psy) {
		if (battery_psy) {
			power_supply_put(battery_psy);
			battery_psy = NULL;
		}
		if (usb_psy) {
			power_supply_put(usb_psy);
			usb_psy = NULL;
		}
		return -ENODEV;
	}
	rc = bsts_add_file(&battery_psy->dev, &dev_attr_chg_type,
			&have_chg_type);
	if (rc)
		goto err;
	rc = bsts_add_file(&battery_psy->dev, &dev_attr_fg_passed_charge,
			&have_fg_passed_charge);
	if (rc)
		goto err;
	rc = bsts_add_file(&battery_psy->dev, &dev_attr_soc_decimal,
			&have_soc_decimal);
	if (rc)
		goto err;
	rc = bsts_add_file(&battery_psy->dev, &dev_attr_engine,
			&have_engine);
	if (rc)
		goto err;
	rc = bsts_add_file(&battery_psy->dev, &dev_attr_charging_speed,
			&have_charging_speed);
	if (rc)
		goto err;
	rc = bsts_add_file(&battery_psy->dev, &dev_attr_board_temp_state,
			&have_board_temp_state);
	if (rc)
		goto err;
	rc = bsts_add_file(&usb_psy->dev, &dev_attr_present,
			&have_usb_present);
	if (rc)
		goto err;
	pr_info(DRV_NAME ": added compatibility attributes\n");
	return 0;
err:
	remove_attrs();
	return rc;
}
static void remove_attrs(void)
{
	if (usb_psy) {
		if (have_usb_present)
			device_remove_file(&usb_psy->dev, &dev_attr_present);
		power_supply_put(usb_psy);
		usb_psy = NULL;
	}
	if (battery_psy) {
		if (have_board_temp_state)
			device_remove_file(&battery_psy->dev,
				&dev_attr_board_temp_state);
		if (have_charging_speed)
			device_remove_file(&battery_psy->dev,
				&dev_attr_charging_speed);
		if (have_engine)
			device_remove_file(&battery_psy->dev,
				&dev_attr_engine);
		if (have_soc_decimal)
			device_remove_file(&battery_psy->dev,
				&dev_attr_soc_decimal);
		if (have_fg_passed_charge)
			device_remove_file(&battery_psy->dev,
				&dev_attr_fg_passed_charge);
		if (have_chg_type)
			device_remove_file(&battery_psy->dev,
				&dev_attr_chg_type);
		power_supply_put(battery_psy);
		battery_psy = NULL;
	}
	if (cms_class) {
		class_destroy(cms_class);
		cms_class = NULL;
	}
	if (fuelsummary_class) {
		class_destroy(fuelsummary_class);
		fuelsummary_class = NULL;
	}
	have_chg_type = false;
	have_fg_passed_charge = false;
	have_soc_decimal = false;
	have_engine = false;
	have_charging_speed = false;
	have_board_temp_state = false;
	have_usb_present = false;
}
static void bsts_probe_work(struct work_struct *work)
{
	int rc;
	mutex_lock(&compat_lock);
	if (bsts_exiting) {
		mutex_unlock(&compat_lock);
		return;
	}
	rc = add_attrs();
	if (rc == -ENODEV) {
		if (!bsts_waiting) {
			bsts_waiting = true;
			pr_info(DRV_NAME ": battery/usb not ready, waiting for probe\n");
		}
		schedule_delayed_work(&bsts_work,
			msecs_to_jiffies(BSTS_RETRY_MS));
	} else if (rc) {
		pr_err(DRV_NAME ": failed to add attributes: %d\n", rc);
	}
	mutex_unlock(&compat_lock);
}
static int __init bsts_init(void)
{
	cms_class_create();
	fuelsummary_class_create();
	INIT_DELAYED_WORK(&bsts_work, bsts_probe_work);
	schedule_delayed_work(&bsts_work, 0);
	INIT_DELAYED_WORK(&wls_tx_work, wls_tx_work_fn);
	INIT_DELAYED_WORK(&wls_poll_work, wls_poll_work_fn);
	INIT_DELAYED_WORK(&soc_limit_work, soc_limit_work_fn);
	INIT_DELAYED_WORK(&bypass_work, bypass_work_fn);
	schedule_delayed_work(&soc_limit_work, msecs_to_jiffies(5000));
	INIT_DELAYED_WORK(&wls_notify_work, wls_notify_work_fn);
	return 0;
}
static void __exit bsts_exit(void)
{
	mutex_lock(&compat_lock);
	bsts_exiting = true;
	mutex_unlock(&compat_lock);
	cancel_delayed_work_sync(&bsts_work);
	cancel_delayed_work_sync(&wls_tx_work);
	cancel_delayed_work_sync(&wls_poll_work);
	cancel_delayed_work_sync(&wls_notify_work);
	cancel_delayed_work_sync(&bypass_work);
	if (READ_ONCE(bypass_applied) > 0) {
		char path[128];

		snprintf(path, sizeof(path), QCOM_BATT_FMT, "restrict_chg");
		write_int_file(path, 0);
	}
	cancel_delayed_work_sync(&soc_limit_work);
	if (soc_limit_applied) {
		write_int_file(QCOM_BATT_PATH("input_suspend"), 0);
		write_int_file(QCOM_BATT_PATH("restrict_chg"), 0);
	}
	remove_attrs();
	pr_info(DRV_NAME ": removed\n");
}
module_init(bsts_init);
module_exit(bsts_exit);
MODULE_DESCRIPTION("Xiaomi/QTI to Vivo power-supply compatibility layer");
MODULE_AUTHOR("~jkoo");
MODULE_LICENSE("GPL");
