// SPDX-License-Identifier: GPL-2.0
// Made for Xiaomi 13...🐥
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

static unsigned long base = 0xb8000000;
static unsigned long size = 0x2c00000;
static unsigned int  panel_w = 1080;
static unsigned int  panel_h = 2400;
static unsigned int  bpp = 4;
static unsigned int  pitch;
static char *path = "/splash.rgb";

module_param(base, ulong, 0444);
module_param(size, ulong, 0444);
module_param(panel_w, uint, 0444);
module_param(panel_h, uint, 0444);
module_param(bpp, uint, 0444);
module_param(pitch, uint, 0444);
MODULE_PARM_DESC(pitch, "bytes per scanline, 0 = panel_w*bpp (unpadded)");
module_param(path, charp, 0444);
MODULE_PARM_DESC(base, "frame buffer physical base");
MODULE_PARM_DESC(size, "frame buffer size in bytes");
MODULE_PARM_DESC(path, "SMP1 payload to draw");

struct smp1_hdr {
	char     magic[4];
	__le32   width;
	__le32   height;
} __packed;

static void resolve_from_dt(void)
{
	struct device_node *np;
	struct resource r;
	np = of_find_node_by_path("/reserved-memory/splash_region");
	if (!np)
		return;
	if (of_address_to_resource(np, 0, &r) == 0) {
		base = r.start;
		size = resource_size(&r);
		pr_info("splash: DT splash_region %pa+0x%lx\n", &r.start, size);
	}
	of_node_put(np);
}

static int __init splash_init(void)
{
	struct file *f;
	void *blob = NULL;
	size_t blob_len;
	loff_t pos;
	const struct smp1_hdr *hdr;
	const u8 *src;
	unsigned int sw, sh, x, y, sx, sy;
	void __iomem *fb;
	u8 *row;
	int ret;
	bool claimed = false;
	u64 map_len;
	bool direct = false;
	resolve_from_dt();
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_err("splash: cannot open %s (%ld)\n", path, PTR_ERR(f));
		return PTR_ERR(f);
	}
	blob_len = i_size_read(file_inode(f));
	if (blob_len < sizeof(*hdr) || blob_len > 64 * 1024 * 1024) {
		pr_err("splash: %s has implausible size %zu\n", path, blob_len);
		filp_close(f, NULL);
		return -EINVAL;
	}
	blob = vmalloc(blob_len);
	if (!blob) {
		filp_close(f, NULL);
		return -ENOMEM;
	}
	pos = 0;
	ret = kernel_read(f, blob, blob_len, &pos);
	filp_close(f, NULL);
	if (ret != (ssize_t)blob_len) {
		pr_err("splash: short read on %s (%d)\n", path, ret);
		vfree(blob);
		return -EIO;
	}
	if (memcmp(blob, "SMP1", 4)) {
		pr_err("splash: %s is not an SMP1 payload\n", path);
		vfree(blob);
		return -EINVAL;
	}
	hdr = (const struct smp1_hdr *)blob;
	sw = le32_to_cpu(hdr->width);
	sh = le32_to_cpu(hdr->height);
	src = blob + sizeof(*hdr);
	if (!sw || !sh || sw > 8192 || sh > 8192 ||
	    blob_len < sizeof(*hdr) + (size_t)sw * sh * 3) {
		pr_err("splash: bad payload geometry %ux%u\n", sw, sh);
		vfree(blob);
		return -EINVAL;
	}
	if (!request_mem_region(base, size, "splash"))
		pr_warn("splash: region %pa not free, writing anyway\n", &base);
	else
		claimed = true;
	if (!pitch)
		pitch = panel_w * bpp;
	map_len = (u64)pitch * panel_h;
	if (map_len > size)
		map_len = size;
	fb = NULL;
	pr_info("splash: mapping %llu bytes at %pa\n", map_len, &base);
	if (!fb) {
		fb = memremap(base, map_len, MEMREMAP_WC);
		if (fb)
			pr_info("splash: memremap(WC) ok\n");
	}
	if (!fb) {
		fb = ioremap(base, map_len);
		if (fb)
			pr_info("splash: ioremap ok\n");
	}
	if (!fb && pfn_valid(base >> PAGE_SHIFT)) {
		fb     = (void __iomem *)phys_to_virt(base);
		direct = true;
		pr_info("splash: using linear map at %pa\n", &base);
	}
	if (!fb) {
		pr_err("splash: all mappings failed (len %llu)\n", map_len);
		if (claimed)
			release_mem_region(base, size);
		vfree(blob);
		return -ENOMEM;
	}
	row = kmalloc((size_t)pitch, GFP_KERNEL);
	if (!row) {
		if (!direct)
			iounmap(fb);
		if (claimed)
			if (claimed)
			release_mem_region(base, size);
		vfree(blob);
		return -ENOMEM;
	}
	for (y = 0; y < panel_h; y++) {
		sy = (unsigned int)((u64)y * sh / panel_h);
		for (x = 0; x < panel_w; x++) {
			const u8 *p;
			sx = (unsigned int)((u64)x * sw / panel_w);
			p = src + ((size_t)sy * sw + sx) * 3;
			row[x * bpp + 0] = p[2];
			row[x * bpp + 1] = p[1];
			row[x * bpp + 2] = p[0];
			if (bpp == 4)
				row[x * bpp + 3] = 0xff;
		}
		memcpy_toio(fb + (size_t)y * pitch, row, (size_t)pitch);
	}
	kfree(row);
	if (!direct)
		iounmap(fb);
	if (claimed)
		release_mem_region(base, size);
	vfree(blob);
	pr_info("splash: drew %ux%u from %s at %pa\n", panel_w, panel_h, path, &base);
	return 0;
}

static void __exit splash_exit(void) {} //stub
module_init(splash_init);
module_exit(splash_exit);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_AUTHOR("~jkoo");
MODULE_DESCRIPTION("splash sent into the bootloader frame buffer");
