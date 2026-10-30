// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/mutex.h>

static struct kset *fp_id_kset;
static DEFINE_MUTEX(fp_id_lock);
static char fp_id_value[32] = "udfp_optical_goodix_jv0301";
/*
- common → 1 (capacitance/rear)
- ultrasonic → 2
- udfp_optical → 3
- sidefp → 4
*/
static ssize_t fp_id_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    ssize_t ret;
    mutex_lock(&fp_id_lock);
    ret = sysfs_emit(buf, "%s\n", fp_id_value);
    mutex_unlock(&fp_id_lock);
    return ret;
}
static ssize_t fp_id_store(struct kobject *kobj, struct kobj_attribute *attr,
                           const char *buf, size_t count)
{
    size_t len;
    mutex_lock(&fp_id_lock);
    len = min(count, sizeof(fp_id_value) - 1);
    memcpy(fp_id_value, buf, len);
    if (len && fp_id_value[len - 1] == '\n')
        len--;
    fp_id_value[len] = '\0';
    mutex_unlock(&fp_id_lock);
    return count;
}
static struct kobj_attribute fp_id_attr = __ATTR(fp_id, 0644, fp_id_show, fp_id_store);
/*🐥*/
static int __init fp_id_init(void)
{
    int ret;
    fp_id_kset = kset_create_and_add("fp_id", NULL, NULL);
    if (!fp_id_kset) {
        pr_err("fp_id: kset_create_and_add failed\n");
        return -ENOMEM;
    }
    ret = sysfs_create_file(&fp_id_kset->kobj, &fp_id_attr.attr);
    if (ret) {
        pr_err("fp_id: sysfs_create_file failed: %d\n", ret);
        kset_unregister(fp_id_kset);
        fp_id_kset = NULL;
        return ret;
    }
    pr_info("fp_id: /sys/fp_id/fp_id = '%s'\n", fp_id_value);
    return 0;
}

static void __exit fp_id_exit(void)
{
    if (!fp_id_kset)
        return;
    sysfs_remove_file(&fp_id_kset->kobj, &fp_id_attr.attr);
    kset_unregister(fp_id_kset);
    fp_id_kset = NULL;
}

module_init(fp_id_init);
module_exit(fp_id_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("/sys/fp_id/fp_id sysfs attributes");
MODULE_AUTHOR("~jkoo");
