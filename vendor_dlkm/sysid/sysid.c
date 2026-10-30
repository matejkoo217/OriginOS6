// SPDX-License-Identifier: GPL-2.0-only
/*
 * sysid - UFS/eMMC device-id nodes for the OriginOS 6
 *   /sys/ufs/ufsid                  UFS device unique id
 *   /sys/block/mmcblk0/device/cid   eMMC CID (fallback)
 * (com.vivo.ic.SystemUtils.getUfsid() -> getEmmcId(), the vivo bsp
 * service's "emmcid", vold, com.vivo.ic, ...).  The Xiaomi kernel exposes
 * neither node and fuxi has no eMMC.
 */
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kernfs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#define DRV_NAME "sysid"
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#define UFS_VPD80_PATH	"/sys/block/sda/device/vpd_pg80"
#define SOC_SERIAL_PATH	"/sys/devices/soc0/serial_number"
#define UFS_MFG_PATH "/sys/devices/platform/soc/1d84000.ufshc/device_descriptor/manufacturing_date"
static struct kobject *ufs_kobj;
static struct kobject *mmcblk0_kobj;
static struct kobject *mmc_dev_kobj;
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
static ssize_t ufsid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	char id[48];
	if (ufsid_value(id, sizeof(id)))
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%s\n", id);
}
static struct kobj_attribute ufsid_attr = __ATTR_RO(ufsid);
static ssize_t cid_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
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
static struct kobject *block_kobject(void)
{
	struct kernfs_node *sys, *kn;
	struct kobject *kobj = NULL;
	if (!kernel_kobj || !kernel_kobj->sd)
		return NULL;
	sys = kernel_kobj->sd->parent;
	if (!sys)
		return NULL;
	kn = kernfs_find_and_get_ns(sys, "block", NULL);
	if (kn) {
		kobj = kn->priv;
		kernfs_put(kn);
	}
	return kobj;
}
static int __init sysid_init(void)
{
	struct kobject *block_kobj;
	int ret;
	ufs_kobj = kobject_create_and_add("ufs", NULL);
	if (!ufs_kobj)
		return -ENOMEM;
	ret = sysfs_create_file(ufs_kobj, &ufsid_attr.attr);
	if (ret) {
		pr_err(DRV_NAME ": create /sys/ufs/ufsid failed: %d\n", ret);
		kobject_put(ufs_kobj);
		ufs_kobj = NULL;
		return ret;
	}
	block_kobj = block_kobject();
	if (!block_kobj) {
		pr_warn(DRV_NAME ": /sys/block kobject not found, no CID node\n");
		return 0;
	}
	mmcblk0_kobj = kobject_create_and_add("mmcblk0", block_kobj);
	if (!mmcblk0_kobj) {
		ret = -ENOMEM;
		goto err;
	}
	mmc_dev_kobj = kobject_create_and_add("device", mmcblk0_kobj);
	if (!mmc_dev_kobj) {
		ret = -ENOMEM;
		goto err;
	}
	ret = sysfs_create_file(mmc_dev_kobj, &cid_attr.attr);
	if (ret) {
		pr_err(DRV_NAME ": create .../device/cid failed: %d\n", ret);
		goto err;
	}
	pr_info(DRV_NAME ": /sys/ufs/ufsid and /sys/block/mmcblk0/device/cid ready\n");
	return 0;
err:
	if (mmc_dev_kobj) {
		kobject_put(mmc_dev_kobj);
		mmc_dev_kobj = NULL;
	}
	if (mmcblk0_kobj) {
		kobject_put(mmcblk0_kobj);
		mmcblk0_kobj = NULL;
	}
	if (ufs_kobj) {
		sysfs_remove_file(ufs_kobj, &ufsid_attr.attr);
		kobject_put(ufs_kobj);
		ufs_kobj = NULL;
	}
	return ret;
}
static void __exit sysid_exit(void)
{
	if (mmc_dev_kobj) {
		sysfs_remove_file(mmc_dev_kobj, &cid_attr.attr);
		kobject_put(mmc_dev_kobj);
		mmc_dev_kobj = NULL;
	}
	if (mmcblk0_kobj) {
		kobject_put(mmcblk0_kobj);
		mmcblk0_kobj = NULL;
	}
	if (ufs_kobj) {
		sysfs_remove_file(ufs_kobj, &ufsid_attr.attr);
		kobject_put(ufs_kobj);
		ufs_kobj = NULL;
	}
}
module_init(sysid_init);
module_exit(sysid_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UFS/eMMC device-id nodes for OriginOS");
MODULE_AUTHOR("~jkoo");
