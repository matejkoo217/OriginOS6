// SPDX-License-Identifier: GPL-2.0-only
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uidgid.h>
#include <linux/workqueue.h>
/*
 * Vivo drives its sunlight/peak (global HBM) brightness by writing an
 * integer mode to /sys/lcm/oled_hbm (VivoPeakController):
 *
 *   24 PEAK_V1_ON   25 PEAK_V1_OFF
 *   26 PEAK_V2_ON   27 PEAK_V2_OFF
 *   0x20000000|level  peak V2 AP-dimming ramp (0 = off, 255 = max)
 *
 * fuxi's panel knows nothing about these; its HBM is mi_display feature
 * 1 (hbm, 0/1) exposed through the disp_param sysfs node.  Forward the
 * modes so the panel actually switches into HBM.
 */
#define MI_DISP_DIR	"/sys/class/mi_display/disp-DSI-0"
#define MI_DISP_PARAM_PATH	MI_DISP_DIR "/disp_param"
#define MI_DISP_FPS_PATH MI_DISP_DIR "/dynamic_fps"
#define MI_DISP_BUILD_ID_PATH MI_DISP_DIR "/panel_build_id"
#define MI_DISP_PANEL_INFO_PATH	MI_DISP_DIR "/panel_info"
#define MI_DISP_VSYNC_PATH	MI_DISP_DIR "/hw_vsync_info"
#define DRM_DSI_MODES_PATH	"/sys/class/drm/card0-DSI-1/modes"
#define MI_BACKLIGHT_DIR	"/sys/class/backlight/panel0-backlight"
#define MI_BACKLIGHT_BRIGHTNESS	MI_BACKLIGHT_DIR "/brightness"
#define MI_BACKLIGHT_MAX	MI_BACKLIGHT_DIR "/max_brightness"
static bool bl_bridge = true;
module_param(bl_bridge, bool, 0644);
MODULE_PARM_DESC(bl_bridge, "forward /sys/lcm/bl_level to the real panel backlight (default: 1)");
static bool fps_bridge = true;
module_param(fps_bridge, bool, 0644);
MODULE_PARM_DESC(fps_bridge, "derive ltpo_mode's ltpo_min_fps from the panel refresh-rate list (default: 1)");
static int high_te = 240;
module_param(high_te, int, 0644);
MODULE_PARM_DESC(high_te, "max TE rate reported in ltpo_mode (240 = vivo contract, 0 = derive from the measured TE, ~121 on fuxi)");
static bool dimming_bridge = true;
module_param(dimming_bridge, bool, 0644);
MODULE_PARM_DESC(dimming_bridge, "forward /sys/lcm/dimming_speed to mi_disp feature 0 (dimming on/off) (default: 1)");
static bool aod_carry = true;
module_param(aod_carry, bool, 0644);
MODULE_PARM_DESC(aod_carry, "carry the last awake backlight into the AOD after screen-off (default: 1)");
static int aod_carry_max = 2047;
module_param(aod_carry_max, int, 0644);
MODULE_PARM_DESC(aod_carry_max, "upper clamp for the carried AOD backlight (default: 2047)");
#define AOD_LEVEL_MIN 21
#define AOD_LEVEL_MAX 30
#define AOD_CARRY_POLL_MS 250
#define AOD_CARRY_ARM_MS 10000
static bool aod_carry_active;
static int aod_last_awake;
static int aod_carried;
static int aod_low_cnt;
static unsigned long aod_arm_until;
static void aod_carry_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(aod_carry_work, aod_carry_work_fn);
/*
 * AOD bridge (vivo contract -> mi_disp).
 *
 * vivo's AOD state machine (donor vivo_display.ko) is sysfs driven: it turns
 * /sys/lcm/aod_disp_info tokens into panel AOD command sets.  The fuxi panel
 * has no such command sets; its AOD lives in mi_disp's doze machinery, which
 * is reachable from the kernel only through the disp_param feature interface
 * (same mechanism this module already uses for HBM):
 *
 *   feature 3  doze_brightness : 0 = normal/off, 1 = doze HBM, 2 = doze LBM
 *                                (accepted only while the panel is in AOD)
 *   feature 16 aod_to_normal   : 1 = leave AOD/low-power (dsi_panel_set_nolp,
 *                                i.e. DSI_CMD_SET_NOLP) - fuxi's equivalent of
 *                                vivo's AOD exit.
 *
 * Mapping used by this bridge (values are logged so they can be verified
 * against the panel; nothing is invented):
 *   partial_aod_show=1                   -> feature 3 = 2 (LBM, dim partial AOD)
 *   partial_aod_show=0                   -> feature 3 = 1 (HBM)
 *   aod_bl_level=0                       -> feature 3 = 0 (AOD brightness off)
 *   aod_bl_level>0                       -> feature 3 = 2 (LBM)
 *
 * NOT translated on purpose (v1):
 *   keyguard_aod_show / full_aod_on.  Driving feature 16 (aod_to_normal) from
 *   here was measured to break the framework's awake->AOD transition animation
 *   and to drop the display straight into doze-suspend, so the AOD enter/exit
 *   transition is left entirely to the framework; these tokens are logged.
 *   aod_1hz_enable / fp_1hz_enable / full_screen_enable have no verified fuxi
 *   equivalent and are only recorded.
 */
#define MI_FEAT_DIMMING 0
#define MI_FEAT_DOZE_BRIGHTNESS 3
#define MI_FEAT_AOD_TO_NORMAL 16
#define MI_DOZE_OFF 0
#define MI_DOZE_HBM 1
#define MI_DOZE_LBM 2
#define MI_DOZE_HBM_DBV_DEFAULT 243
#define VIVO_PEAK_V1_ON 24
#define VIVO_PEAK_V1_OFF 25
#define VIVO_PEAK_V2_ON 26
#define VIVO_PEAK_V2_OFF 27
#define VIVO_PEAK_V2_DIMMING_BASE 0x20000000L
#define LCM_BUF_MAX 256
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
static struct kset *lcm_kset;
static struct kset *lcm1_kset;
static struct kset *fingerprint_kset;
static struct kobject *lcm_event_kobj;
static DEFINE_MUTEX(lcm_lock);
static int mi_disp_read_int(const char *path, int def);
static int mi_disp_read_str(const char *path, char *out, size_t len);
static int mi_write_int_file(const char *path, int val);
static int drm_mode_fps(int *lo, int *hi);
static int measured_max_te(void);
static void lcm_apply_bl(char *str);
static void lcm_apply_dimming(char *str);

/*
 * Vivo's display stack (VivoLcmUtils / SmoothBrightnessManager /
 * HighBrightnessManager) identifies the panel by reading this node and then
 * matching it against the "panel_name" entries in the /system/etc/LcmConfig
 * json files.  Report the panel mi_disp actually has (m3_38_0c_0a on fuxi)
 * instead of pretending to be the vivo PD2243 - a matching LcmConfig has to be
 * shipped for the real panel; the PD2243 file does not describe it.
 *
 * panel_name_val is a userspace override only: empty means "read mi_disp now".
 */
static char panel_name_val[LCM_BUF_MAX] = "";
static char ltpo_mode_val[LCM_BUF_MAX] = "high_te=240,ltpo_min_fps=60";
static char ltpo_cmd_val[LCM_BUF_MAX] = "";
static char hbm_max_brightness_val[LCM_BUF_MAX] = "";
static char panel_info_val[LCM_BUF_MAX] = "local_hbm=true";
// having 2x support_full_screen isnt bug, its deliberate fix, not sure how its done on origin os 7,
// but here nightpearl only accepts 1-3 as enabled, but SF uses 4 for ltpo refresh change that makes 
// aod refresh rate be 60 becouse we set ltpo_min_fps instead of 120 on 1-3.
static char aod_disp_info_val[LCM_BUF_MAX] =
	"support_1hz=0,aod_brightness=21,support_video_aod=0,"
	"support_full_screen=3,support_full_screen=4,fp_1hz_enable=0," 
	"full_screen_enable=0";
static char oled_hbm_val[LCM_BUF_MAX] = "0";
static char bl_level_val[LCM_BUF_MAX] = "0";
static char aod_bl_level_val[LCM_BUF_MAX] = "0";
static char dimming_speed_val[LCM_BUF_MAX] = "60";
static char colour_gamut_val[LCM_BUF_MAX] = "0";
static char hdr10p_bl_remap_val[LCM_BUF_MAX] = "0";
static char error_info_val[LCM_BUF_MAX] = "fp_bl_errcheck=0";
static const char lcm_id_val[] = "0x00000000";
static char hbm_notify_enable_val[LCM_BUF_MAX] = "0";
static void lcm_event_release(struct kobject *kobj)
{
	kfree(kobj);
}
static struct kobj_type lcm_event_ktype = {
	.release = lcm_event_release,
};
static ssize_t lcm_show(char *buf, const char *val)
{
	ssize_t ret;
	mutex_lock(&lcm_lock);
	ret = sysfs_emit(buf, "%s\n", val);
	mutex_unlock(&lcm_lock);
	return ret;
}
static ssize_t lcm_store(const char *buf, size_t count, char *val, size_t size)
{
	size_t len;
	mutex_lock(&lcm_lock);
	len = min(count, size - 1);
	memcpy(val, buf, len);
	if (len && val[len - 1] == '\n')
		len--;
	val[len] = '\0';
	mutex_unlock(&lcm_lock);
	return count;
}
static void kv_set(char *dst, size_t size, const char *key, size_t klen,
		   const char *val, size_t vlen)
{
	size_t dlen;
	char *p = dst;
	while (p && *p) {
		int at_start = (p == dst) || p[-1] == ',' || p[-1] == '\n';
		if (at_start && strncmp(p, key, klen) == 0 && p[klen] == '=') {
			char *vstart = p + klen + 1;
			char *vend = vstart;
			while (*vend && *vend != ',' && *vend != '\n')
				vend++;
			if ((size_t)(vstart - dst) + vlen + strlen(vend) >= size)
				return;
			memmove(vstart + vlen, vend, strlen(vend) + 1);
			memcpy(vstart, val, vlen);
			return;
		}
		p = strpbrk(p, ",\n");
		if (p)
			p++;
	}
	dlen = strlen(dst);
	if (dlen + 1 + klen + 1 + vlen + 1 > size)
		return;
	if (dlen)
		dst[dlen++] = ',';
	memcpy(dst + dlen, key, klen);
	dst[dlen + klen] = '=';
	memcpy(dst + dlen + klen + 1, val, vlen);
	dst[dlen + klen + 1 + vlen] = '\0';
}
static ssize_t lcm_store_kv(const char *buf, size_t count, char *val, size_t size)
{
	const char *p = buf;
	mutex_lock(&lcm_lock);
	while (p && *p) {
		const char *end = p;
		while (*end && *end != ',' && *end != '\n' && *end != '\r' &&
		       *end != ' ' && *end != '\t')
			end++;
		if (end > p) {
			const char *eq = memchr(p, '=', end - p);
			if (eq && eq > p)
				kv_set(val, size, p, eq - p, eq + 1,
				       end - (eq + 1));
		}
		p = *end ? end + 1 : end;
		while (*p == ',' || *p == '\n' || *p == '\r' || *p == ' ' ||
		       *p == '\t')
			p++;
	}
	mutex_unlock(&lcm_lock);
	return count;
}
#define LCM_RW_ATTR(_pfx, _name, _val)						\
static ssize_t _pfx##_show(struct kobject *kobj,				\
			   struct kobj_attribute *attr, char *buf)		\
{										\
	return lcm_show(buf, _val);						\
}										\
static ssize_t _pfx##_store(struct kobject *kobj,				\
			    struct kobj_attribute *attr,			\
			    const char *buf, size_t count)			\
{										\
	return lcm_store(buf, count, _val, sizeof(_val));			\
}										\
static struct kobj_attribute _pfx##_attr =					\
	__ATTR(_name, 0660, _pfx##_show, _pfx##_store)

#define LCM_RW_KV_ATTR(_pfx, _name, _val)					\
static ssize_t _pfx##_show(struct kobject *kobj,				\
			   struct kobj_attribute *attr, char *buf)		\
{										\
	return lcm_show(buf, _val);						\
}										\
static ssize_t _pfx##_store(struct kobject *kobj,				\
			    struct kobj_attribute *attr,			\
			    const char *buf, size_t count)			\
{										\
	return lcm_store_kv(buf, count, _val, sizeof(_val));			\
}										\
static struct kobj_attribute _pfx##_attr =					\
	__ATTR(_name, 0660, _pfx##_show, _pfx##_store)

#define LCM_RO_ATTR(_pfx, _name, _val)						\
static ssize_t _pfx##_show(struct kobject *kobj,				\
			   struct kobj_attribute *attr, char *buf)		\
{										\
	return lcm_show(buf, _val);						\
}										\
static struct kobj_attribute _pfx##_attr =					\
	__ATTR(_name, 0444, _pfx##_show, NULL)

static ssize_t panel_name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	char info[256];
	char val[LCM_BUF_MAX];
	const char *p, *end;
	mutex_lock(&lcm_lock);
	if (panel_name_val[0]) {
		ssize_t ret = sysfs_emit(buf, "%s\n", panel_name_val);
		mutex_unlock(&lcm_lock);
		return ret;
	}
	mutex_unlock(&lcm_lock);
	if (!mi_disp_read_str(MI_DISP_PANEL_INFO_PATH, info, sizeof(info))) {
		p = strstr(info, "panel_name=");
		if (p) {
			p += strlen("panel_name=");
			end = p;
			while (*end && *end != ',' && *end != '\n' &&
			       *end != ' ')
				end++;
			if (end > p) {
				scnprintf(val, sizeof(val), "%.*s panel",
					  (int)(end - p), p);
				return lcm_show(buf, val);
			}
		}
	}
	return lcm_show(buf, NULL);
}
static ssize_t panel_name_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return lcm_store(buf, count, panel_name_val, sizeof(panel_name_val));
}
static struct kobj_attribute panel_name_attr = __ATTR(panel_name, 0660, panel_name_show, panel_name_store);
static ssize_t hbm_max_brightness_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int max;
	ssize_t ret;
	mutex_lock(&lcm_lock);
	if (hbm_max_brightness_val[0]) {
		ret = sysfs_emit(buf, "%s\n", hbm_max_brightness_val);
		mutex_unlock(&lcm_lock);
		return ret;
	}
	mutex_unlock(&lcm_lock);
	max = mi_disp_read_int(MI_BACKLIGHT_MAX, 0);
	return sysfs_emit(buf, "%d\n", max > 0 ? max : 0);
}
static ssize_t hbm_max_brightness_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return lcm_store(buf, count, hbm_max_brightness_val,
			 sizeof(hbm_max_brightness_val));
}
static struct kobj_attribute hbm_max_brightness_attr = __ATTR(hbm_max_brightness, 0660, hbm_max_brightness_show, hbm_max_brightness_store);
static ssize_t ltpo_mode_report(char *buf, const char *fallback)
{
	char val[LCM_BUF_MAX];
	int lo, hi, te;
	if (!fps_bridge || drm_mode_fps(&lo, &hi))
		return lcm_show(buf, fallback);
	te = high_te;
	if (te <= 0) {
		te = measured_max_te();
		if (!te)
			te = hi;
	}
	scnprintf(val, sizeof(val), "high_te=%d,ltpo_min_fps=%d", te, lo);
	return lcm_show(buf, val);
}
static ssize_t ltpo_mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return ltpo_mode_report(buf, ltpo_mode_val);
}
static ssize_t ltpo_mode_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = lcm_store(buf, count, ltpo_cmd_val, sizeof(ltpo_cmd_val));
	char req[LCM_BUF_MAX];
	mutex_lock(&lcm_lock);
	strscpy(req, ltpo_cmd_val, sizeof(req));
	mutex_unlock(&lcm_lock);
	pr_info("lcm_bridge: ltpo_mode request '%s' recorded (panel rates are read-only)\n",
		req);
	return ret;
}
static struct kobj_attribute ltpo_mode_attr = __ATTR(ltpo_mode, 0660, ltpo_mode_show, ltpo_mode_store);
static void mi_disp_set_feature(int id, int val);
static void lcm_apply_aod_info(const char *merged);
static void lcm_apply_aod_bl(char *str);
static void lcm_apply_doze_class(int cls);
LCM_RW_KV_ATTR(panel_info, panel_info, panel_info_val);
static ssize_t bl_level_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lcm_show(buf, bl_level_val);
}

static ssize_t bl_level_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = lcm_store(buf, count, bl_level_val, sizeof(bl_level_val));
	lcm_apply_bl(bl_level_val);
	return ret;
}

static struct kobj_attribute bl_level_attr = __ATTR(bl_level, 0660, bl_level_show, bl_level_store);
static ssize_t aod_disp_info_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lcm_show(buf, aod_disp_info_val);
}
static ssize_t aod_disp_info_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	lcm_store_kv(buf, count, aod_disp_info_val, sizeof(aod_disp_info_val));
	lcm_apply_aod_info(aod_disp_info_val);
	return count;
}

static struct kobj_attribute aod_disp_info_attr = __ATTR(aod_disp_info, 0660, aod_disp_info_show, aod_disp_info_store);

static ssize_t aod_bl_level_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lcm_show(buf, aod_bl_level_val);
}

static ssize_t aod_bl_level_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = lcm_store(buf, count, aod_bl_level_val,
				sizeof(aod_bl_level_val));

	lcm_apply_aod_bl(aod_bl_level_val);
	return ret;
}
static struct kobj_attribute aod_bl_level_attr = __ATTR(aod_bl_level, 0660, aod_bl_level_show, aod_bl_level_store);
static ssize_t dimming_speed_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lcm_show(buf, dimming_speed_val);
}
static ssize_t dimming_speed_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = lcm_store(buf, count, dimming_speed_val,
				sizeof(dimming_speed_val));
	lcm_apply_dimming(dimming_speed_val);
	return ret;
}
//lazy to hide more easter eggs
static struct kobj_attribute dimming_speed_attr =
	__ATTR(dimming_speed, 0660, dimming_speed_show, dimming_speed_store);
LCM_RW_ATTR(colour_gamut, colour_gamut, colour_gamut_val);
LCM_RW_ATTR(hdr10p_bl_remap, hdr10p_bl_remap, hdr10p_bl_remap_val);
LCM_RW_KV_ATTR(error_info, error_info, error_info_val);
static ssize_t lcm_id_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	char id[64];
	if (!mi_disp_read_str(MI_DISP_BUILD_ID_PATH, id, sizeof(id)) &&
	    strlen(id) >= 10 && strncmp(id, "Unsupported", 11))
		return lcm_show(buf, id);
	return lcm_show(buf, lcm_id_val);
}
static struct kobj_attribute lcm_id_attr = __ATTR(lcm_id, 0444, lcm_id_show, NULL);
static void lcm_emit_event(const char *node, const char *value)
{
	char node_env[64];
	char value_env[LCM_BUF_MAX + 16];
	char *envp[] = { node_env, value_env, NULL };
	snprintf(node_env, sizeof(node_env), "LCM_NODE=%s", node);
	snprintf(value_env, sizeof(value_env), "LCM_VALUE=%s", value);
	if (lcm_event_kobj) {
		int ret = kobject_uevent_env(lcm_event_kobj, KOBJ_CHANGE, envp);
		if (ret)
			pr_warn("lcm_bridge: %s uevent failed: %d\n", node,
				ret);
	}
}
static void mi_disp_write(const char *value)
{
	struct file *f;
	loff_t pos = 0;
	f = filp_open(MI_DISP_PARAM_PATH, O_WRONLY, 0);
	if (IS_ERR(f)) {
		pr_warn("lcm_bridge: open %s failed: %ld\n", MI_DISP_PARAM_PATH,
			PTR_ERR(f));
		return;
	}
	kernel_write(f, value, strlen(value), &pos);
	filp_close(f, NULL);
}
static int mi_write_int_file(const char *path, int val)
{
	struct file *f;
	char buf[16];
	int len;
	loff_t pos = 0;
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f)) {
		pr_warn("lcm_bridge: open %s failed: %ld\n", path, PTR_ERR(f));
		return (int)PTR_ERR(f);
	}
	len = scnprintf(buf, sizeof(buf), "%d", val);
	kernel_write(f, buf, len, &pos);
	filp_close(f, NULL);

	return 0;
}
static void lcm_apply_bl(char *str)
{
	int level, max;
	if (!bl_bridge)
		return;
	if (kstrtoint(strim(str), 10, &level))
		return;
	if (level <= 0) {
		pr_info("lcm_bridge: bl_level 0 not forwarded (framework owns off)\n");
		return;
	}
	max = mi_disp_read_int(MI_BACKLIGHT_MAX, 0);
	if (max > 0 && level > max) {
		pr_info("lcm_bridge: bl_level %d clamped to panel max %d\n",
			level, max);
		level = max;
	}
	pr_info("lcm_bridge: bl_level -> backlight %d\n", level);
	mi_write_int_file(MI_BACKLIGHT_BRIGHTNESS, level);
}
static int mi_disp_read_int(const char *path, int def)
{
	struct file *f;
	char buf[32];
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
static int mi_disp_read_str(const char *path, char *out, size_t len)
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
static void aod_carry_work_fn(struct work_struct *work)
{
	int bl, target;
	if (!aod_carry_active)
		return;
	if (!aod_carry)
		goto rearm;
	bl = mi_disp_read_int(MI_BACKLIGHT_BRIGHTNESS, -1);
	if (bl < 0)
		goto rearm;
	if (bl > AOD_LEVEL_MAX) {
		if (bl != aod_carried && !time_before(jiffies, aod_arm_until)) {
			aod_last_awake = bl;
			aod_arm_until = 0;
		}
		aod_low_cnt = 0;
		goto rearm;
	}
	if (aod_last_awake <= AOD_LEVEL_MAX) {
		aod_low_cnt = 0;
		goto rearm;
	}
	if (!time_before(jiffies, aod_arm_until)) {
		aod_arm_until = jiffies + msecs_to_jiffies(AOD_CARRY_ARM_MS);
		aod_low_cnt = 0;
		pr_info("lcm_bridge: aod carry armed (awake %d)\n",
			aod_last_awake);
	}
	if (++aod_low_cnt < 2)
		goto rearm;
	aod_low_cnt = 0;
	target = clamp(aod_last_awake, AOD_LEVEL_MIN, aod_carry_max);
	if (bl != target) {
		pr_info("lcm_bridge: aod carry %d -> %d (awake %d)\n",
			bl, target, aod_last_awake);
		mi_write_int_file(MI_BACKLIGHT_BRIGHTNESS, target);
		aod_carried = target;
		aod_arm_until = jiffies + msecs_to_jiffies(AOD_CARRY_ARM_MS);
	}
rearm:
	schedule_delayed_work(&aod_carry_work, msecs_to_jiffies(AOD_CARRY_POLL_MS));
}
static int drm_mode_fps(int *lo, int *hi)
{
	struct file *f;
	char *buf;
	ssize_t n;
	loff_t pos = 0;
	size_t i;
	int min = 0, max = 0;
	f = filp_open(DRM_DSI_MODES_PATH, O_RDONLY, 0);
	if (IS_ERR(f))
		return (int)PTR_ERR(f);
	buf = kmalloc(4096, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return -ENOMEM;
	}
	n = kernel_read(f, buf, 4095, &pos);
	filp_close(f, NULL);
	if (n <= 0) {
		kfree(buf);
		return -EIO;
	}
	buf[n] = '\0';
	for (i = 1; i < (size_t)n; i++) {
		size_t j;
		unsigned int v = 0;
		if (buf[i] != 'x' || buf[i - 1] < '0' || buf[i - 1] > '9')
			continue;
		j = i + 1;
		while (j < (size_t)n && buf[j] >= '0' && buf[j] <= '9') {
			v = v * 10 + (buf[j] - '0');
			j++;
		}
		if (j == i + 1 || v < 1 || v > 480)
			continue;
		if (!min || (int)v < min)
			min = (int)v;
		if ((int)v > max)
			max = (int)v;
	}
	kfree(buf);
	if (!max)
		return -ENOENT;
	*lo = min;
	*hi = max;
	return 0;
}
static int measured_max_te(void)
{
	char buf[160];
	const char *p;
	int period = 0;
	int te;
	if (mi_disp_read_str(MI_DISP_VSYNC_PATH, buf, sizeof(buf)))
		return 0;
	p = strstr(buf, "vsync_period_ns:");
	if (!p)
		return 0;
	p += strlen("vsync_period_ns:");
	if (kstrtoint(p, 10, &period) || period <= 0)
		return 0;
	te = (int)(1000000000ULL / (u64)period);
	return ((te + 5) / 10) * 10;
}
static void lcm_apply_peak(const char *str)
{
	long mode;
	int hbm;
	if (kstrtol(str, 10, &mode))
		return;
	if (mode == VIVO_PEAK_V1_ON || mode == VIVO_PEAK_V2_ON)
		hbm = 1;
	else if (mode == VIVO_PEAK_V1_OFF || mode == VIVO_PEAK_V2_OFF)
		hbm = 0;
	else if ((mode & VIVO_PEAK_V2_DIMMING_BASE) == VIVO_PEAK_V2_DIMMING_BASE)
		hbm = (mode & 0x1ff) != 0;
	else
		return;
	pr_info("lcm_bridge: oled_hbm %ld -> hbm %d\n", mode, hbm);
	mi_disp_write(hbm ? "1 1" : "1 0");
}
static void mi_disp_set_feature(int id, int val)
{
	char cmd[32];
	snprintf(cmd, sizeof(cmd), "%d %d", id, val);
	pr_info("lcm_bridge: aod bridge -> feature %d = %d\n", id, val);
	mi_disp_write(cmd);
}
static void lcm_apply_dimming(char *str)
{
	int speed;
	if (!dimming_bridge)
		return;
	if (kstrtoint(strim(str), 10, &speed))
		return;
	pr_info("lcm_bridge: dimming_speed %d -> mi_disp dimming %d\n",
		speed, speed > 0);
	mi_disp_set_feature(MI_FEAT_DIMMING, speed > 0 ? 1 : 0);
}
static int last_doze_class = -1;
static void lcm_apply_doze_class(int cls)
{
	if (cls == last_doze_class)
		return;
	last_doze_class = cls;
	mi_disp_set_feature(MI_FEAT_DOZE_BRIGHTNESS, cls);
}
static int kv_get_int(const char *buf, const char *key, int *out)
{
	size_t klen = strlen(key);
	const char *p = buf;
	while (p && *p) {
		const char *end = p;
		while (*end && *end != ',' && *end != '\n' && *end != '\r' &&
		       *end != ' ' && *end != '	')
			end++;
		if ((size_t)(end - p) > klen && !strncmp(p, key, klen) &&
		    p[klen] == '=') {
			char tmp[16];
			size_t vlen = end - (p + klen + 1);
			if (!vlen || vlen >= sizeof(tmp))
				return -EINVAL;
			memcpy(tmp, p + klen + 1, vlen);
			tmp[vlen] = '\0';
			return kstrtoint(tmp, 10, out);
		}
		p = *end ? end + 1 : end;
		while (*p == ',' || *p == '\n' || *p == '\r' || *p == ' ' ||
		       *p == '	')
			p++;
	}
	return -ENOENT;
}
static void lcm_apply_aod_info(const char *merged)
{
	int show, full, partial, tmp;
	if (!kv_get_int(merged, "keyguard_aod_show", &show))
		pr_info("lcm_bridge: keyguard_aod_show = %d (logged only)\n", show);
	if (!kv_get_int(merged, "full_aod_on", &full))
		pr_info("lcm_bridge: full_aod_on = %d (logged only)\n", full);
	if (!kv_get_int(merged, "partial_aod_show", &partial)) {
		pr_info("lcm_bridge: partial aod = %d\n", partial);
		lcm_apply_doze_class(partial ? MI_DOZE_LBM : MI_DOZE_HBM);
	}
	if (!kv_get_int(merged, "aod_1hz_enable", &tmp))
		pr_info("lcm_bridge: aod_1hz_enable = %d (recorded only)\n", tmp);
	if (!kv_get_int(merged, "fp_1hz_enable", &tmp))
		pr_info("lcm_bridge: fp_1hz_enable = %d (recorded only)\n", tmp);
	if (!kv_get_int(merged, "full_screen_enable", &tmp))
		pr_info("lcm_bridge: full_screen_enable = %d (recorded only)\n",
			tmp);
}
static void lcm_apply_aod_bl(char *str)
{
	char info[LCM_BUF_MAX];
	int level, high, cls;
	if (kstrtoint(strim(str), 10, &level) || level < 0)
		return;
	if (aod_carry) {
		pr_info("lcm_bridge: aod_bl_level %d recorded (aod_carry owns the backlight)\n",
			level);
		return;
	}
	mutex_lock(&lcm_lock);
	strncpy(info, aod_disp_info_val, sizeof(info) - 1);
	info[sizeof(info) - 1] = '\0';
	mutex_unlock(&lcm_lock);
	if (kv_get_int(info, "aod_high_brightness", &high) || high <= 0)
		high = MI_DOZE_HBM_DBV_DEFAULT;
	if (level == 0)
		cls = MI_DOZE_OFF;
	else if (level >= high)
		cls = MI_DOZE_HBM;
	else
		cls = MI_DOZE_LBM;
	pr_info("lcm_bridge: aod_bl_level %d -> doze class %d (high=%d)\n",
		level, cls, high);
	lcm_apply_doze_class(cls);
}
static ssize_t oled_hbm_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lcm_show(buf, oled_hbm_val);
}

static ssize_t oled_hbm_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	char value[LCM_BUF_MAX];
	ssize_t ret;
	ret = lcm_store(buf, count, oled_hbm_val, sizeof(oled_hbm_val));
	mutex_lock(&lcm_lock);
	strncpy(value, oled_hbm_val, sizeof(value) - 1);
	value[sizeof(value) - 1] = '\0';
	mutex_unlock(&lcm_lock);
	lcm_emit_event("oled_hbm", value);
	lcm_apply_peak(value);
	return ret;
}

static struct kobj_attribute oled_hbm_attr = __ATTR(oled_hbm, 0660, oled_hbm_show, oled_hbm_store);

static ssize_t hbm_notify_enable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lcm_show(buf, hbm_notify_enable_val);
}

static ssize_t hbm_notify_enable_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	char value[LCM_BUF_MAX];
	ssize_t ret;
	ret = lcm_store(buf, count, hbm_notify_enable_val, sizeof(hbm_notify_enable_val));
	mutex_lock(&lcm_lock);
	strncpy(value, hbm_notify_enable_val, sizeof(value) - 1);
	value[sizeof(value) - 1] = '\0';
	mutex_unlock(&lcm_lock);
	lcm_emit_event("hbm_notify_enable", value);
	return ret;
}
static struct kobj_attribute hbm_notify_enable_attr = __ATTR(hbm_notify_enable, 0660, hbm_notify_enable_show, hbm_notify_enable_store);
#define LCM_FPS_ATTR(_pfx)							\
static ssize_t _pfx##_show(struct kobject *kobj,				\
			   struct kobj_attribute *attr, char *buf)		\
{										\
	return sysfs_emit(buf, "%d\n",					\
			  mi_disp_read_int(MI_DISP_FPS_PATH, 120));		\
}										\
static struct kobj_attribute _pfx##_attr =					\
	__ATTR(refresh_monitor, 0444, _pfx##_show, NULL)
LCM_FPS_ATTR(refresh_monitor);
static ssize_t lcm_fps_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "fps=%dHz\n",
			  mi_disp_read_int(MI_DISP_FPS_PATH, 120));
}
static struct kobj_attribute lcm_fps_attr = __ATTR(lcm_fps, 0444, lcm_fps_show, NULL);
#define lcm1_attrs lcm_attrs
static struct kobj_attribute *lcm_attrs[] = {
	&panel_info_attr,
	&panel_name_attr,
	&hbm_max_brightness_attr,
	&aod_disp_info_attr,
	&oled_hbm_attr,
	&bl_level_attr,
	&aod_bl_level_attr,
	&dimming_speed_attr,
	&error_info_attr,
	&lcm_id_attr,
	&refresh_monitor_attr,
	&colour_gamut_attr,
	&ltpo_mode_attr,
	&hdr10p_bl_remap_attr,
	&lcm_fps_attr,
};
static struct kobj_attribute *fp_attrs[] = {
	&hbm_notify_enable_attr,
};
#define LCM_GID_GRAPHICS 1003
static int lcm_add_attrs(struct kset *kset, struct kobj_attribute **attrs, int n)
{
	int i, ret;
	for (i = 0; i < n; i++) {
		kgid_t gid = KGIDT_INIT(1000);
		ret = sysfs_create_file(&kset->kobj, &attrs[i]->attr);
		if (ret) {
			pr_err("lcm_bridge: create %s failed: %d\n",
			       attrs[i]->attr.name, ret);
			while (--i >= 0)
				sysfs_remove_file(&kset->kobj,
						  &attrs[i]->attr);
			return ret;
		}
		if (!strcmp(attrs[i]->attr.name, "colour_gamut") ||
		    !strcmp(attrs[i]->attr.name, "hdr10p_bl_remap"))
			gid = KGIDT_INIT(LCM_GID_GRAPHICS);
		ret = sysfs_file_change_owner(&kset->kobj,
					      attrs[i]->attr.name,
					      GLOBAL_ROOT_UID, gid);
		if (ret)
			pr_warn("lcm_bridge: chown %s failed: %d\n",
				attrs[i]->attr.name, ret);
	}
	return 0;
}
static void lcm_del_attrs(struct kset *kset, struct kobj_attribute **attrs, int n)
{
	int i;
	for (i = 0; i < n; i++)
		sysfs_remove_file(&kset->kobj, &attrs[i]->attr);
}

static void lcm_bridge_teardown(void)
{
	aod_carry_active = false;
	cancel_delayed_work_sync(&aod_carry_work);
	if (lcm_event_kobj) {
		kobject_del(lcm_event_kobj);
		kobject_put(lcm_event_kobj);
		lcm_event_kobj = NULL;
	}
	if (fingerprint_kset) {
		lcm_del_attrs(fingerprint_kset, fp_attrs, ARRAY_SIZE(fp_attrs));
		kset_unregister(fingerprint_kset);
		fingerprint_kset = NULL;
	}
	if (lcm1_kset) {
		lcm_del_attrs(lcm1_kset, lcm1_attrs, ARRAY_SIZE(lcm1_attrs));
		kset_unregister(lcm1_kset);
		lcm1_kset = NULL;
	}
	if (lcm_kset) {
		lcm_del_attrs(lcm_kset, lcm_attrs, ARRAY_SIZE(lcm_attrs));
		kset_unregister(lcm_kset);
		lcm_kset = NULL;
	}
}

static int __init lcm_bridge_init(void)
{
	int ret;
	lcm_kset = kset_create_and_add("lcm", NULL, NULL);
	if (!lcm_kset) {
		pr_err("lcm_bridge: kset lcm failed\n");
		return -ENOMEM;
	}
	lcm_event_kobj = kzalloc(sizeof(*lcm_event_kobj), GFP_KERNEL);
	if (!lcm_event_kobj) {
		pr_err("lcm_bridge: event kobject alloc failed\n");
		ret = -ENOMEM;
		goto err;
	}
	kobject_init(lcm_event_kobj, &lcm_event_ktype);
	lcm_event_kobj->kset = lcm_kset;
	ret = kobject_add(lcm_event_kobj, &lcm_kset->kobj, ".events");
	if (ret) {
		pr_err("lcm_bridge: event kobject add failed: %d\n", ret);
		kobject_put(lcm_event_kobj);
		lcm_event_kobj = NULL;
		goto err;
	}
	lcm1_kset = kset_create_and_add("lcm1", NULL, NULL);
	if (!lcm1_kset) {
		pr_err("lcm_bridge: kset lcm1 failed\n");
		ret = -ENOMEM;
		goto err;
	}
	fingerprint_kset = kset_create_and_add("fingerprint", NULL, NULL);
	if (!fingerprint_kset) {
		pr_err("lcm_bridge: kset fingerprint failed\n");
		ret = -ENOMEM;
		goto err;
	}
	ret = lcm_add_attrs(lcm_kset, lcm_attrs, ARRAY_SIZE(lcm_attrs));
	if (ret)
		goto err;
	ret = lcm_add_attrs(lcm1_kset, lcm1_attrs, ARRAY_SIZE(lcm1_attrs));
	if (ret)
		goto err;
	ret = lcm_add_attrs(fingerprint_kset, fp_attrs, ARRAY_SIZE(fp_attrs));
	if (ret)
		goto err;
	aod_carry_active = true;
	schedule_delayed_work(&aod_carry_work,
			      msecs_to_jiffies(AOD_CARRY_POLL_MS));
	pr_debug("lcm_bridge: /sys/lcm, /sys/lcm1, /sys/fingerprint ready\n");
	return 0;
err:
	lcm_bridge_teardown();
	return ret;
}
static void __exit lcm_bridge_exit(void)
{
	lcm_bridge_teardown();
}
module_init(lcm_bridge_init);
module_exit(lcm_bridge_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("/sys/lcm{,1} nodes for OriginOS");
MODULE_AUTHOR("~jkoo");
