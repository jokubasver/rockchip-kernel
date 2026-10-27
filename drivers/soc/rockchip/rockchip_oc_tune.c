// SPDX-License-Identifier: GPL-2.0
/*
 * Runtime OPP voltage tuning for overclock/undervolt stability testing.
 *
 * /sys/kernel/debug/oc_tune/{cpu,gpu,dmc}
 *   read:  one "<freq_hz> <uV>" line per available OPP
 *   write: "<freq_hz> <uV>" sets that OPP's voltage. It takes effect the next
 *          time the domain switches to that OPP, so a tool must hop away and
 *          back to apply it to the running one.
 *
 * Voltages are clamped to OC_TUNE_MIN_UV..OC_TUNE_MAX_UV; the regulator
 * constraints still apply on top of that.
 */

#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define OC_TUNE_MIN_UV	950000	/* RK817 DCDC minimum */
#define OC_TUNE_MAX_UV	1400000

struct oc_tune_dom {
	const char *name;
	const char *compat;	/* NULL: use CPU0 */
};

static const struct oc_tune_dom oc_tune_doms[] = {
	{ "cpu", NULL },
	{ "gpu", "arm,mali-bifrost" },
	{ "dmc", "rockchip,px30-dmc" },
};

/* The device is not refcounted by the caller: the platform devices outlive us. */
static struct device *oc_tune_get_dev(const struct oc_tune_dom *dom)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct device *dev = NULL;

	if (!dom->compat)
		return get_cpu_device(0);

	np = of_find_compatible_node(NULL, NULL, dom->compat);
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (pdev) {
		dev = &pdev->dev;
		put_device(dev);
	}

	return dev;
}

static int oc_tune_show(struct seq_file *s, void *unused)
{
	const struct oc_tune_dom *dom = s->private;
	struct device *dev = oc_tune_get_dev(dom);
	struct dev_pm_opp *opp;
	unsigned long rate = 0;

	if (!dev)
		return -ENODEV;

	while (!IS_ERR(opp = dev_pm_opp_find_freq_ceil(dev, &rate))) {
		seq_printf(s, "%lu %lu\n", rate, dev_pm_opp_get_voltage(opp));
		dev_pm_opp_put(opp);
		rate++;
	}

	return 0;
}

static int oc_tune_open(struct inode *inode, struct file *file)
{
	return single_open(file, oc_tune_show, inode->i_private);
}

static ssize_t oc_tune_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	const struct oc_tune_dom *dom = file_inode(file)->i_private;
	struct device *dev = oc_tune_get_dev(dom);
	unsigned long freq, uv;
	char buf[64];
	int ret;

	if (!dev)
		return -ENODEV;
	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sscanf(buf, "%lu %lu", &freq, &uv) != 2)
		return -EINVAL;
	if (uv < OC_TUNE_MIN_UV || uv > OC_TUNE_MAX_UV)
		return -ERANGE;

	ret = dev_pm_opp_adjust_voltage(dev, freq, uv, uv, OC_TUNE_MAX_UV);
	if (ret)
		return ret;

	dev_info(dev, "oc_tune: %lu Hz -> %lu uV\n", freq, uv);

	return count;
}

static const struct file_operations oc_tune_fops = {
	.owner		= THIS_MODULE,
	.open		= oc_tune_open,
	.read		= seq_read,
	.write		= oc_tune_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init oc_tune_init(void)
{
	struct dentry *root;
	int i;

	root = debugfs_create_dir("oc_tune", NULL);
	for (i = 0; i < ARRAY_SIZE(oc_tune_doms); i++)
		debugfs_create_file(oc_tune_doms[i].name, 0600, root,
				    (void *)&oc_tune_doms[i], &oc_tune_fops);

	return 0;
}
late_initcall(oc_tune_init);
