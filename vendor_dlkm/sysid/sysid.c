// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysid - UFS/eMMC device-id and storage-info nodes for the OriginOS 6
 *   /sys/ufs/ufsid                  UFS device unique id
 *   /sys/ufs/io_sum                 cumulative UFS I/O (sda + dm layers), "<sectors read> <sectors written>"
 *   /sys/ufs/nand_info_dec          decoded UFS device descriptor
 *   /sys/class/block/sda/device/hr  storage health 0-100 (UFS life time estimate)
 *   /sys/block/mmcblk0/device/cid   eMMC CID (fallback)
 *   /sys/block/mmcblk0/queue/max_bg_depth
 *   /sys/block/mmcblk0/queue/wbt_lat_usec
 *                                   sda block-queue tunables, mirrored
 * (com.vivo.ic.SystemUtils.getUfsid() -> getEmmcId(), the vivo bsp
 * service's "emmcid", vold, com.vivo.ic, and vivo's "pie_ufs" storage
 * code's "read io_sum failed! ufs init failed").
 */
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kernfs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
//now it looks better 🐥
#define DRV_NAME "sysid"
#define UFS_VPD80_PATH	"/sys/block/sda/device/vpd_pg80"
#define SOC_SERIAL_PATH	"/sys/devices/soc0/serial_number"
#define UFS_HBA_DIR	"/sys/devices/platform/soc/1d84000.ufshc"
#define UFS_DEV_DIR	UFS_HBA_DIR "/device_descriptor"
#define UFS_HEALTH_DIR	UFS_HBA_DIR "/health_descriptor"
#define UFS_MFG_PATH UFS_DEV_DIR "/manufacturing_date"
#define UFS_MANFID_PATH	UFS_DEV_DIR "/manufacturer_id"
#define UFS_MODEL_PATH	"/sys/block/sda/device/model"
#define SDA_NAME	"sda"
#define DISKSTATS_PATH	"/proc/diskstats"
#define DISKSTATS_CHUNK	16384
#define DISKSTATS_MAX	(1 << 20)
#define SDA_QUEUE_MAX_BG_DEPTH	"/sys/block/sda/queue/max_bg_depth"
#define SDA_QUEUE_WBT_LAT_USEC	"/sys/block/sda/queue/wbt_lat_usec"

static struct kobject *ufs_kobj;
static struct kobject *mmcblk0_kobj;
static struct kobject *mmc_dev_kobj;
static struct kobject *mmc_queue_kobj;
static struct kobject *sda_dev_kobj;
static struct kobject *cls_mmcblk0_kobj;
static struct kobject *cls_mmc_dev_kobj;

static int read_node(const char *path, u8 *buf, size_t len)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return -ENOENT;
	n = kernel_read(f, buf, len, &pos);
	filp_close(f, NULL);
	return n < 0 ? (int)n : (int)n;
}
static int read_node_all(const char *path, char **out, size_t *out_len)
{
	struct file *f;
	loff_t pos = 0;
	char *buf, *nb, *nl;
	size_t len = 0, cap = DISKSTATS_CHUNK;
	bool eof = false;
	ssize_t n;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return -ENOENT;
	buf = kmalloc(cap, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return -ENOMEM;
	}
	for (;;) {
		if (len >= cap - 1) {
			if (cap >= DISKSTATS_MAX)
				break;
			nb = krealloc(buf, cap * 2, GFP_KERNEL);
			if (!nb) {
				kfree(buf);
				filp_close(f, NULL);
				return -ENOMEM;
			}
			buf = nb;
			cap *= 2;
		}
		n = kernel_read(f, buf + len, cap - 1 - len, &pos);
		if (n < 0) {
			if (!len) {
				kfree(buf);
				filp_close(f, NULL);
				return (int)n;
			}
			break;
		}
		if (!n) {
			eof = true;
			break;
		}
		len += n;
	}
	filp_close(f, NULL);
	buf[len] = '\0';
	if (!eof) {
		nl = strrchr(buf, '\n');
		len = nl ? (size_t)(nl - buf) + 1 : 0;
		buf[len] = '\0';
	}
	*out = buf;
	*out_len = len;
	return 0;
}
static int node_str(const char *path, char *out, size_t len)
{
	u8 raw[128];
	int n = read_node(path, raw, sizeof(raw) - 1);
	if (n <= 0)
		return -ENODATA;
	raw[n] = '\0';
	strscpy(out, strim(raw), len);
	return out[0] ? 0 : -ENODATA;
}
static int node_uint(const char *path, unsigned int base, unsigned int *val)
{
	u8 raw[32];
	int n = read_node(path, raw, sizeof(raw) - 1);
	if (n <= 0)
		return -ENODATA;
	raw[n] = '\0';
	return kstrtouint(strim(raw), base, val) ? -EINVAL : 0;
}
static int ufsid_value(char *out, size_t len)
{
	u8 raw[64];
	int n, i, j;
	n = read_node(UFS_VPD80_PATH, raw, sizeof(raw));
	if (n >= 4) {
		int plen = (raw[2] << 8) | raw[3];
		if (plen > 0 && plen <= n - 4) {
			for (i = 0, j = 0; i < plen && j < len - 1; i++) {
				if (!raw[4 + i])
					break;
				out[j++] = raw[4 + i];
			}
			if (j) {
				out[j] = '\0';
				return 0;
			}
		}
	}
	n = read_node(SOC_SERIAL_PATH, raw, sizeof(raw) - 1);
	if (n > 0) {
		raw[n] = '\0';
		for (i = 0, j = 0; i < n && raw[i] && raw[i] != '\n' &&
		     j < len - 1; i++)
			out[j++] = raw[i];
		out[j] = '\0';
		if (j)
			return 0;
	}
	return -ENODEV;
}
static struct kernfs_node *kernfs_walk(struct kernfs_node *root, const char *path)
{
	struct kernfs_node *kn, *next;
	const char *p = path;
	size_t len;
	char name[64];
	if (!root)
		return NULL;
	kn = root;
	kernfs_get(kn);
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		for (len = 0; p[len] && p[len] != '/'; len++)
			;
		if (!len || len >= sizeof(name))
			goto err;
		memcpy(name, p, len);
		name[len] = '\0';
		p += len;
		next = kernfs_find_and_get_ns(kn, name, NULL);
		if (!next)
			goto err;
		kernfs_put(kn);
		kn = next;
		while (kernfs_type(kn) == KERNFS_LINK) {
			next = kn->symlink.target_kn;
			if (!next)
				goto err;
			kernfs_get(next);
			kernfs_put(kn);
			kn = next;
		}
	}
	return kn;
err:
	kernfs_put(kn);
	return NULL;
}
static struct kobject *kobj_of_node(struct kernfs_node *kn)
{
	struct kobject *kobj;
	if (!kn || kernfs_type(kn) != KERNFS_DIR || !kn->priv)
		return NULL;
	kobj = kn->priv;
	if (kobj->sd != kn)
		return NULL;
	kobject_get(kobj);
	return kobj;
}
static struct kobject *sysfs_kobject(const char *path)
{
	struct kernfs_node *sys, *kn;
	struct kobject *kobj;
	if (!kernel_kobj || !kernel_kobj->sd)
		return NULL;
	sys = kernel_kobj->sd->parent;
	if (!sys)
		return NULL;
	kn = kernfs_walk(sys, path);
	if (!kn)
		return NULL;
	kobj = kobj_of_node(kn);
	kernfs_put(kn);
	return kobj;
}
static struct kobject *sda_device_kobject(void)
{
	struct kernfs_node *sys, *kn, *vpd;
	struct kobject *kobj;
	if (!kernel_kobj || !kernel_kobj->sd)
		return NULL;
	sys = kernel_kobj->sd->parent;
	if (!sys)
		return NULL;
	kn = kernfs_walk(sys, "class/block/" SDA_NAME "/device");
	if (!kn)
		return NULL;
	vpd = kernfs_find_and_get_ns(kn, "vpd_pg80", NULL);
	if (!vpd) {
		pr_warn(DRV_NAME ": %s/device is not the sda device dir\n",
			SDA_NAME);
		kernfs_put(kn);
		return NULL;
	}
	kernfs_put(vpd);
	kobj = kobj_of_node(kn);
	kernfs_put(kn);
	return kobj;
}
static struct kobject *class_block_kobject(void)
{
	return sysfs_kobject("class/block");
}
static struct kobject *block_kobject(void)
{
	return sysfs_kobject("block");
}
static char *next_token(char **pp)
{
	char *s = *pp, *tok;
	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s || *s == '\n')
		return NULL;
	tok = s;
	while (*s && *s != ' ' && *s != '\t' && *s != '\n')
		s++;
	if (*s)
		*s++ = '\0';
	*pp = s;
	return tok;
}
static bool disk_io_name(const char *name)
{
	size_t i;
	if (!strcmp(name, SDA_NAME) || !strncmp(name, "dm-", 3))
		return true;
	if (strncmp(name, SDA_NAME, sizeof(SDA_NAME) - 1))
		return false;
	for (i = sizeof(SDA_NAME) - 1; name[i]; i++)
		if (!isdigit(name[i]))
			return false;
	return i > sizeof(SDA_NAME) - 1;
}
static int diskstats_line_add(char *line, u64 *sect_read, u64 *sect_written)
{
	char *p = line, *tok;
	u64 v[7];
	size_t i;
	if (!next_token(&p) || !next_token(&p))
		return -EINVAL;
	tok = next_token(&p);
	if (!tok)
		return -EINVAL;
	if (!disk_io_name(tok))
		return -ENOENT;
	for (i = 0; i < ARRAY_SIZE(v); i++) {
		tok = next_token(&p);
		if (!tok || kstrtoull(tok, 10, &v[i]))
			return -EINVAL;
	}
	*sect_read += v[2];
	*sect_written += v[6];
	return 0;
}
static int diskstats_io_sum(u64 *sect_read, u64 *sect_written)
{
	char *buf, *p;
	size_t len;
	int ret, lines = 0;
	*sect_read = 0;
	*sect_written = 0;
	ret = read_node_all(DISKSTATS_PATH, &buf, &len);
	if (ret)
		return ret;
	for (p = buf; *p;) {
		char *nl = strchrnul(p, '\n');
		bool last = !*nl;
		*nl = '\0';
		if (!diskstats_line_add(p, sect_read, sect_written))
			lines++;
		if (last)
			break;
		p = nl + 1;
	}
	kfree(buf);
	return lines ? 0 : -ENODATA;
}
static int ufs_health_pct(void)
{
	unsigned int a = 0, b = 0, worst;
	if (node_uint(UFS_HEALTH_DIR "/life_time_estimation_a", 16, &a))
		a = 0;
	if (node_uint(UFS_HEALTH_DIR "/life_time_estimation_b", 16, &b))
		b = 0;
	worst = max(a, b);
	if (!worst)
		return 100;
	return clamp(105 - 10 * (int)worst, 0, 100);
}
static void ufs_spec_version_str(unsigned int v, char *out, size_t len)
{
	unsigned int major = (v >> 8) & 0xff;
	unsigned int minor1 = (v >> 4) & 0xf;
	unsigned int minor2 = v & 0xf;
	if (!v || (v & ~0xfff)) {
		scnprintf(out, len, "0x%04x", v);
		return;
	}
	if (minor2)
		scnprintf(out, len, "%x.%x.%x (0x%04x)", major, minor1, minor2,
			  v);
	else
		scnprintf(out, len, "%x.%x (0x%04x)", major, minor1, v);
}
static ssize_t ufsid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	char id[48];
	if (ufsid_value(id, sizeof(id)))
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%s\n", id);
}
static struct kobj_attribute ufsid_attr = __ATTR_RO(ufsid);
static ssize_t io_sum_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u64 rd = 0, wr = 0;
	if (diskstats_io_sum(&rd, &wr))
		return sysfs_emit(buf, "0 0\n");
	return sysfs_emit(buf, "%llu %llu\n", rd, wr);
}
static struct kobj_attribute io_sum_attr = __ATTR_RO(io_sum);
static ssize_t nand_info_dec_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	char model[64] = "", serial[64] = "", spec[32];
	unsigned int manfid = 0, date = 0, specv = 0, luns = 0, qd = 0;
	unsigned int devver = 0, eol = 0, ela = 0, elb = 0;
	size_t pos = 0;
	node_str(UFS_MODEL_PATH, model, sizeof(model));
	if (ufsid_value(serial, sizeof(serial)))
		serial[0] = '\0';
	node_uint(UFS_DEV_DIR "/manufacturer_id", 16, &manfid);
	node_uint(UFS_DEV_DIR "/manufacturing_date", 16, &date);
	node_uint(UFS_DEV_DIR "/specification_version", 16, &specv);
	node_uint(UFS_DEV_DIR "/number_of_luns", 16, &luns);
	node_uint(UFS_DEV_DIR "/queue_depth", 16, &qd);
	node_uint(UFS_DEV_DIR "/device_version", 16, &devver);
	node_uint(UFS_HEALTH_DIR "/eol_info", 16, &eol);
	node_uint(UFS_HEALTH_DIR "/life_time_estimation_a", 16, &ela);
	node_uint(UFS_HEALTH_DIR "/life_time_estimation_b", 16, &elb);
	ufs_spec_version_str(specv, spec, sizeof(spec));
	pos += scnprintf(buf + pos, PAGE_SIZE - pos,
			 "model: %s\n"
			 "serial: %s\n"
			 "manufacturer_id: 0x%04x\n"
			 "mfg_date: 0x%04x\n"
			 "spec_version: %s\n"
			 "luns: %u\n"
			 "queue_depth: %u\n"
			 "device_version: 0x%04x\n"
			 "eol_info: 0x%02x\n"
			 "life_time_estimation_a: 0x%02x\n"
			 "life_time_estimation_b: 0x%02x\n"
			 "health: %d\n",
			 model, serial, manfid, date, spec, luns, qd, devver,
			 eol, ela, elb, ufs_health_pct());
	return pos;
}
static struct kobj_attribute nand_info_dec_attr = __ATTR_RO(nand_info_dec);
static ssize_t hr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", ufs_health_pct());
}
static struct device_attribute hr_attr = __ATTR_RO(hr);
static struct kobject *mi_mem_kobj;
static struct kobject *mi_dev_kobj;
static ssize_t hr_kobj_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", ufs_health_pct());
}
static struct kobj_attribute hr_kobj_attr = __ATTR(hr, 0444, hr_kobj_show, NULL);
static ssize_t max_bg_depth_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int v = 0;
	node_uint(SDA_QUEUE_MAX_BG_DEPTH, 10, &v);
	return sysfs_emit(buf, "%u\n", v);
}
static struct kobj_attribute max_bg_depth_attr = __ATTR_RO(max_bg_depth);
static ssize_t wbt_lat_usec_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int v = 0;
	node_uint(SDA_QUEUE_WBT_LAT_USEC, 10, &v);
	return sysfs_emit(buf, "%u\n", v);
}
static ssize_t wbt_lat_usec_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	f = filp_open(SDA_QUEUE_WBT_LAT_USEC, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, buf, count, &pos);
	filp_close(f, NULL);
	return n;
}
static struct kobj_attribute wbt_lat_usec_attr = __ATTR_RW(wbt_lat_usec);
static ssize_t cid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u8 cid[16] = { 0x15, 0x01, 0x00, 'M', '3', '1', '8', 0x00 };
	unsigned long long serial = 0;
	unsigned int date = 0;
	u8 raw[64];
	char hex[40];
	int n, i;
	n = read_node(SOC_SERIAL_PATH, raw, sizeof(raw) - 1);
	if (n > 0) {
		raw[n] = '\0';
		if (kstrtoull(strim(raw), 16, &serial))
			serial = 0;
	}
	n = read_node(UFS_MFG_PATH, raw, sizeof(raw) - 1);
	if (n > 0) {
		raw[n] = '\0';
		if (kstrtouint(strim(raw), 16, &date))
			date = 0;
	}
	cid[8] = (serial >> 24) & 0xff;
	cid[9] = (serial >> 16) & 0xff;
	cid[10] = (serial >> 8) & 0xff;
	cid[11] = serial & 0xff;
	cid[12] = (date >> 8) & 0xff;
	cid[13] = date & 0xff;
	for (i = 0; i < 16; i++)
		scnprintf(hex + i * 2, 3, "%02x", cid[i]);
	return sysfs_emit(buf, "%s\n", hex);
}
static struct kobj_attribute cid_attr = __ATTR_RO(cid);
static ssize_t name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u8 raw[64];
	int n = read_node(UFS_MODEL_PATH, raw, sizeof(raw) - 1);
	if (n <= 0)
		return sysfs_emit(buf, "\n");
	raw[n] = '\0';
	return sysfs_emit(buf, "%s\n", strim(raw));
}
static struct kobj_attribute name_attr = __ATTR_RO(name);
static ssize_t serial_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	char id[48];
	if (ufsid_value(id, sizeof(id)))
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%s\n", id);
}
static struct kobj_attribute serial_attr = __ATTR_RO(serial);
static ssize_t manfid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u8 raw[32];
	unsigned int v = 0;
	int n = read_node(UFS_MANFID_PATH, raw, sizeof(raw) - 1);
	if (n > 0) {
		raw[n] = '\0';
		if (kstrtouint(strim(raw), 16, &v))
			v = 0;
	}
	return sysfs_emit(buf, "0x%06x\n", v);
}
static struct kobj_attribute manfid_attr = __ATTR_RO(manfid);
static struct kobj_attribute *ufs_attrs[] = {
	&ufsid_attr, &io_sum_attr, &nand_info_dec_attr,
};
static struct kobj_attribute *mmc_dev_attrs[] = {
	&cid_attr, &name_attr, &serial_attr, &manfid_attr,
};
static struct kobj_attribute *mmc_queue_attrs[] = {
	&max_bg_depth_attr, &wbt_lat_usec_attr,
};
static int add_attrs(struct kobject *kobj, struct kobj_attribute **attrs, size_t n)
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
	}
	return 0;
}
static void del_attrs(struct kobject *kobj, struct kobj_attribute **attrs, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		sysfs_remove_file(kobj, &attrs[i]->attr);
}
static void sysid_remove_nodes(void)
{
	if (cls_mmc_dev_kobj) {
		del_attrs(cls_mmc_dev_kobj, mmc_dev_attrs,
			  ARRAY_SIZE(mmc_dev_attrs));
		kobject_put(cls_mmc_dev_kobj);
		cls_mmc_dev_kobj = NULL;
	}
	if (cls_mmcblk0_kobj) {
		kobject_put(cls_mmcblk0_kobj);
		cls_mmcblk0_kobj = NULL;
	}
	if (sda_dev_kobj) {
		sysfs_remove_file(sda_dev_kobj, &hr_attr.attr);
		kobject_put(sda_dev_kobj);
		sda_dev_kobj = NULL;
	}
	if (mmc_queue_kobj) {
		del_attrs(mmc_queue_kobj, mmc_queue_attrs,
			  ARRAY_SIZE(mmc_queue_attrs));
		kobject_put(mmc_queue_kobj);
		mmc_queue_kobj = NULL;
	}
	if (mmc_dev_kobj) {
		del_attrs(mmc_dev_kobj, mmc_dev_attrs,
			  ARRAY_SIZE(mmc_dev_attrs));
		kobject_put(mmc_dev_kobj);
		mmc_dev_kobj = NULL;
	}
	if (mmcblk0_kobj) {
		kobject_put(mmcblk0_kobj);
		mmcblk0_kobj = NULL;
	}
	if (ufs_kobj) {
		del_attrs(ufs_kobj, ufs_attrs, ARRAY_SIZE(ufs_attrs));
		kobject_put(ufs_kobj);
		ufs_kobj = NULL;
	}
}
static void sda_hr_create(void)
{
	int ret;
	sda_dev_kobj = sda_device_kobject();
	if (!sda_dev_kobj) {
		pr_warn(DRV_NAME ": sda device dir not found, no hr node\n");
		return;
	}
	ret = sysfs_create_file(sda_dev_kobj, &hr_attr.attr);
	if (ret) {
		pr_warn(DRV_NAME ": create sda/device/hr failed: %d\n", ret);
		kobject_put(sda_dev_kobj);
		sda_dev_kobj = NULL;
	}
}
static int __init sysid_init(void)
{
	struct kobject *block_kobj, *cls_block;
	int ret;
	ufs_kobj = kobject_create_and_add("ufs", NULL);
	if (!ufs_kobj)
		return -ENOMEM;
	ret = add_attrs(ufs_kobj, ufs_attrs, ARRAY_SIZE(ufs_attrs));
	if (ret) {
		pr_err(DRV_NAME ": create /sys/ufs nodes failed: %d\n", ret);
		goto err;
	}
	block_kobj = block_kobject();
	if (!block_kobj) {
		pr_warn(DRV_NAME ": /sys/block kobject not found, no eMMC nodes\n");
		goto out;
	}
	mmcblk0_kobj = kobject_create_and_add("mmcblk0", block_kobj);
	if (!mmcblk0_kobj) {
		ret = -ENOMEM;
		kobject_put(block_kobj);
		goto err;
	}
	mmc_dev_kobj = kobject_create_and_add("device", mmcblk0_kobj);
	if (!mmc_dev_kobj) {
		ret = -ENOMEM;
		kobject_put(block_kobj);
		goto err;
	}
	ret = add_attrs(mmc_dev_kobj, mmc_dev_attrs, ARRAY_SIZE(mmc_dev_attrs));
	if (ret)
		goto out_block;
	mmc_queue_kobj = kobject_create_and_add("queue", mmcblk0_kobj);
	if (!mmc_queue_kobj)
		pr_warn(DRV_NAME ": mmcblk0/queue kobject failed\n");
	else if (add_attrs(mmc_queue_kobj, mmc_queue_attrs,
			   ARRAY_SIZE(mmc_queue_attrs))) {
		kobject_put(mmc_queue_kobj);
		mmc_queue_kobj = NULL;
	}
	cls_block = class_block_kobject();
	if (!cls_block) {
		pr_warn(DRV_NAME ": /sys/class/block not found, no class view\n");
	} else {
		cls_mmcblk0_kobj = kobject_create_and_add("mmcblk0", cls_block);
		kobject_put(cls_block);
		if (!cls_mmcblk0_kobj) {
			pr_warn(DRV_NAME ": class mmcblk0 kobject failed\n");
		} else {
			cls_mmc_dev_kobj =
				kobject_create_and_add("device",
						       cls_mmcblk0_kobj);
			if (!cls_mmc_dev_kobj)
				pr_warn(DRV_NAME ": class device kobject failed\n");
			else if (add_attrs(cls_mmc_dev_kobj, mmc_dev_attrs,
					   ARRAY_SIZE(mmc_dev_attrs)))
				pr_warn(DRV_NAME ": class device attrs failed\n");
		}
	}
out_block:
	kobject_put(block_kobj);
	if (ret)
		goto err;
out:
	sda_hr_create();
	{
		struct kobject *base = sysfs_kobject("devices/virtual/mi_memory");
		struct kobject *dev =
			sysfs_kobject("devices/virtual/mi_memory/mi_memory_device");

		if (base) {
			struct kobject *m = kobject_create_and_add("mi_memory",
								   base);

			if (m) {
				mi_mem_kobj = kobject_create_and_add("mmc_info", m);
				kobject_put(m);
			}
			if (mi_mem_kobj &&
			    sysfs_create_file(mi_mem_kobj, &hr_kobj_attr.attr)) {
				kobject_put(mi_mem_kobj);
				mi_mem_kobj = NULL;
			}
		}
		if (dev) {
			mi_dev_kobj = kobject_create_and_add("emmc0", dev);
			if (mi_dev_kobj &&
			    sysfs_create_file(mi_dev_kobj, &hr_kobj_attr.attr)) {
				kobject_put(mi_dev_kobj);
				mi_dev_kobj = NULL;
			}
		}
	}

	pr_info(DRV_NAME ": /sys/ufs/{ufsid,io_sum,nand_info_dec}, /sys/class/block/sda/device/hr and the mmcblk0 identity/queue nodes ready\n");
	return 0;
err:
	sysid_remove_nodes();
	return ret;
}
static void __exit sysid_exit(void)
{
	if (mi_dev_kobj) {
		sysfs_remove_file(mi_dev_kobj, &hr_kobj_attr.attr);
		kobject_put(mi_dev_kobj);
		mi_dev_kobj = NULL;
	}
	if (mi_mem_kobj) {
		sysfs_remove_file(mi_mem_kobj, &hr_kobj_attr.attr);
		kobject_put(mi_mem_kobj);
		mi_mem_kobj = NULL;
	}
	sysid_remove_nodes();
}
module_init(sysid_init);
module_exit(sysid_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UFS/eMMC device-id and storage-info nodes for OriginOS");
MODULE_AUTHOR("~jkoo");
