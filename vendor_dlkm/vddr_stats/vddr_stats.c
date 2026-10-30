// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/vddr_stats - DDR frequency residency for vivo's com.vivo.pem.
 * PEM (Qualcomm branch of com/vivo/pem/bd/b.L2) reads /dev/vddr_stats
 * CONFIG_DEVTMPFS is off, so ueventd creates
 * /dev/vddr_stats and it must be made readable by uid system:
 * add to ueventd.rc: under kmesg
 *     /dev/vddr_stats    0444    root    system
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#define DRV_NAME "vddr_stats"
#define DDR_STATS_MAGIC_KEY	0xA1157A75
#define DDR_STATS_MAX_NUM_MODES	0x14
#define DDR_STATS_NUM_MODES_ADDR 0x4
#define DDR_OFFSET_ADDR		0x1c
#define DDR_ENTRY_TYPE_FREQ	0x1
struct stats_entry {
	u32 name;
	u32 count;
	u64 duration;
};
struct qmp_pkt {
	u32 size;
	void *data;
};
static void __iomem *vddr_reg;
static struct mbox_chan *vddr_chan;
static struct mbox_client vddr_cl;
static struct device *vddr_dev;
static struct device_node *vddr_np;
static DEFINE_MUTEX(vddr_lock);
static int send_sync = 1;
module_param(send_sync, int, 0644);
MODULE_PARM_DESC(send_sync, "send the AOP ddr freqsync QMP packet before reading");
static const char vddr_fallback[] ="";
static void vddr_try_mbox(void) {
	if (vddr_chan || !vddr_dev)
		return;
	vddr_cl.dev = vddr_dev;
	vddr_cl.tx_block = true;
	vddr_cl.tx_tout = 1000;
	vddr_cl.knows_txdone = false;
	vddr_chan = mbox_request_channel(&vddr_cl, 0);
	if (IS_ERR(vddr_chan)) {
		pr_info(DRV_NAME ": mbox_request_channel failed: %ld\n",
			PTR_ERR(vddr_chan));
		vddr_chan = NULL;
		return;
	}
	pr_info(DRV_NAME ": aop mailbox acquired\n");
}
static int vddr_setup(void) {
	struct platform_device *pdev;
	struct resource res;
	void __iomem *off;
	phys_addr_t ddr_base;
	u32 val;
	vddr_np = of_find_compatible_node(NULL, NULL, "qcom,rpmh-sleep-stats");
	if (!vddr_np) {
		pr_err(DRV_NAME ": soc-sleep-stats node not found\n");
		return -ENODEV;
	}
	if (of_address_to_resource(vddr_np, 0, &res)) {
		pr_err(DRV_NAME ": no reg resource\n");
		return -EINVAL;
	}
	off = ioremap(res.start + DDR_OFFSET_ADDR, sizeof(u32));
	if (!off)
		return -ENOMEM;
	val = readl_relaxed(off);
	iounmap(off);
	ddr_base = res.start | val;
	vddr_reg = ioremap(ddr_base, resource_size(&res));
	if (!vddr_reg) {
		pr_err(DRV_NAME ": ioremap ddr MSGRAM failed\n");
		return -ENOMEM;
	}
	pr_info(DRV_NAME ": ddr MSGRAM %pa magic=%08x modes=%u\n", &ddr_base, readl_relaxed(vddr_reg), readl_relaxed(vddr_reg + DDR_STATS_NUM_MODES_ADDR));
	pdev = of_find_device_by_node(vddr_np);
	if (!pdev) {
		pr_warn(DRV_NAME ": no platform device for mbox\n");
		return 0;
	}
	vddr_dev = &pdev->dev;
	vddr_try_mbox();
	return 0;
}
static void vddr_send_sync(void) {
	char msg[40] = {};
	struct qmp_pkt pkt;
	int ret;
	if (!vddr_chan)
		return;
	snprintf(msg, sizeof(msg), "{class: ddr, action: freqsync}");
	pkt.size = ALIGN(strlen(msg) + 1, 4);
	pkt.data = msg;
	ret = mbox_send_message(vddr_chan, &pkt);
	pr_info(DRV_NAME ": freqsync mbox ret=%d\n", ret);
	msleep(20);
}
static int vddr_build(char *buf, size_t size) {
	struct stats_entry e;
	void __iomem *p;
	u32 modes, i;
	u64 total = 0;
	int len = 0;
	if (!vddr_reg)
		return 0;
	if (readl_relaxed(vddr_reg) != DDR_STATS_MAGIC_KEY) {
		pr_info(DRV_NAME ": bad MSGRAM magic %08x\n",
			readl_relaxed(vddr_reg));
		return 0;
	}
	if (send_sync) {
		vddr_try_mbox();
		vddr_send_sync();
	}
	modes = readl_relaxed(vddr_reg + DDR_STATS_NUM_MODES_ADDR);
	if (!modes || modes > DDR_STATS_MAX_NUM_MODES)
		modes = DDR_STATS_MAX_NUM_MODES;
	p = vddr_reg + DDR_STATS_NUM_MODES_ADDR + 0x4;
	for (i = 0; i < modes; i++) {
		memcpy_fromio(&e, p + i * sizeof(e), sizeof(e));
		if (((e.name >> 8) & 0xFF) == DDR_ENTRY_TYPE_FREQ)
			total += e.duration;
	}
	for (i = 0; i < modes && len < size; i++) {
		u32 pct;
		memcpy_fromio(&e, p + i * sizeof(e), sizeof(e));
		if (((e.name >> 8) & 0xFF) != DDR_ENTRY_TYPE_FREQ)
			continue;
		pct = total ? (u32)div64_u64(e.duration * 100, total) : 0;
		len += scnprintf(buf + len, size - len,
			"Freq %uMhz:\tCP IDX:%u\tDuration (ticks):%llu (~%u%%)\n",
			e.name >> 16, e.name & 0x1F, e.duration, pct);
	}
	return len;
}
static ssize_t vddr_read(struct file *fp, char __user *ubuf, size_t count, loff_t *ppos) {
	static char buf[4096];
	int len;
	ssize_t ret;
	mutex_lock(&vddr_lock);
	if (*ppos == 0) {
		len = vddr_build(buf, sizeof(buf));
		if (len == 0)
			len = scnprintf(buf, sizeof(buf), "%s", vddr_fallback);
	} else {
		len = strnlen(buf, sizeof(buf));
	}
	if (*ppos >= len) {
		ret = 0;
		goto out;
	}
	count = min(count, (size_t)(len - *ppos));
	if (copy_to_user(ubuf, buf + *ppos, count)) {
		ret = -EFAULT;
		goto out;
	}
	*ppos += count;
	ret = count;
out:
	mutex_unlock(&vddr_lock);
	return ret;
}
static const struct file_operations vddr_fops = {
	.owner = THIS_MODULE,
	.read = vddr_read,
};
static struct miscdevice vddr_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRV_NAME,
	.fops = &vddr_fops,
};
static int __init vddr_init(void)
{
	int ret = vddr_setup();
	if (ret)
		return ret;
	return misc_register(&vddr_miscdev);
}
static void __exit vddr_exit(void)
{
	misc_deregister(&vddr_miscdev);
	if (vddr_chan)
		mbox_free_channel(vddr_chan);
	if (vddr_reg)
		iounmap(vddr_reg);
	if (vddr_dev)
		put_device(vddr_dev);
	if (vddr_np)
		of_node_put(vddr_np);
}
module_init(vddr_init);
module_exit(vddr_exit);
MODULE_DESCRIPTION("/dev/vddr_stats DDR residency for PEM");
MODULE_AUTHOR("~jkoo");
MODULE_LICENSE("GPL");
