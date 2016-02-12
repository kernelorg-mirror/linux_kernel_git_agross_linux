/*
 * MPM - Modem Power Manager
 *
 * Copyright (c) 2016 Linaro Ltd.
*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/spinlock.h>
#include <linux/of_irq.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/cpu_pm.h>

struct qcom_mpm {
	struct device *dev;
	void __iomem *base;
	irq_hw_number_t ipc_irq;
	struct irq_domain *gic_domain;
	struct irq_domain *gpio_domain;
	struct notifier_block pm_notifier;
};

/* can't allocate memory when the domains are originally registered */
static struct qcom_mpm __mpm;
static DECLARE_BITMAP(mpm_gic_wake, 1020);
static DECLARE_BITMAP(mpm_gpio_wake, 1020);
static DEFINE_SPINLOCK(mpm_lock);

#define MPM_RPM_OFFSET	0x1d0
#define MPM_RPM_SIZE	0x1000

static void qcom_mpm_gic_mask(struct irq_data *d)
{
	irq_chip_mask_parent(d);
}

static void qcom_mpm_gic_unmask(struct irq_data *d)
{
	irq_chip_unmask_parent(d);
}

static int qcom_mpm_gic_set_type(struct irq_data *d, unsigned int type)
{
	return irq_chip_set_type_parent(d, type);
}

static int qcom_mpm_gic_set_wake(struct irq_data *d, unsigned int on)
{
	spin_lock(&mpm_lock);
	if (on)
		bitmap_set(mpm_gic_wake, d->hwirq, 1);
	else
		bitmap_clear(mpm_gic_wake, d->hwirq, 1);
	spin_unlock(&mpm_lock);

printk(KERN_ERR "============> %d set to wake\n", d->hwirq);
	return 0;
}

static void qcom_mpm_gpio_mask(struct irq_data *d)
{
}

static void qcom_mpm_gpio_unmask(struct irq_data *d)
{
}

static int qcom_mpm_gpio_set_type(struct irq_data *d, unsigned int type)
{
	return 0;
}

static int qcom_mpm_gpio_set_wake(struct irq_data *d, unsigned int on)
{
	spin_lock(&mpm_lock);
	if (on)
		bitmap_set(mpm_gpio_wake, d->hwirq, 1);
	else
		bitmap_clear(mpm_gpio_wake, d->hwirq, 1);
	spin_unlock(&mpm_lock);

	return 0;
}

static struct irq_chip qcom_mpm_gpio_chip = {
	.name		= "MPM-GPIO",
	.irq_mask	= qcom_mpm_gpio_mask,
	.irq_unmask	= qcom_mpm_gpio_unmask,
	.irq_set_type	= qcom_mpm_gpio_set_type,
	.irq_set_wake	= qcom_mpm_gpio_set_wake,
	.flags		= IRQCHIP_MASK_ON_SUSPEND,
#ifdef CONFIG_SMP
	.irq_set_affinity	= irq_chip_set_affinity_parent,
#endif
};

static struct irq_chip qcom_mpm_gic_chip = {
	.name		= "MPM-GIC",
	.irq_eoi	= irq_chip_eoi_parent,
	.irq_mask	= qcom_mpm_gic_mask,
	.irq_unmask	= qcom_mpm_gic_unmask,
	.irq_retrigger	= irq_chip_retrigger_hierarchy,
	.irq_set_type	= qcom_mpm_gic_set_type,
	.flags		= IRQCHIP_MASK_ON_SUSPEND,
	.irq_set_wake	= qcom_mpm_gic_set_wake,
#ifdef CONFIG_SMP
	.irq_set_affinity	= irq_chip_set_affinity_parent,
#endif
};

static int qcom_mpm_gpio_translate(struct irq_domain *d,
		struct irq_fwspec *fwspec,
		unsigned long *hwirq,
		unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count !=2)
			return -EINVAL;

		*hwirq = fwspec->param[0];
		*type = fwspec->param[1];
		return 0;
	}

	return -EINVAL;
}
static int qcom_mpm_gic_translate(struct irq_domain *d,
		struct irq_fwspec *fwspec,
		unsigned long *hwirq,
		unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count != 3)
			return -EINVAL;

		if (fwspec->param[0] != 0)
			return -EINVAL;

		/* just give back information in the entry */
		*hwirq = fwspec->param[1];
		*type = fwspec->param[2];
		return 0;
	}

	return -EINVAL;
}

static int qcom_mpm_gpio_alloc(struct irq_domain *domain,
	unsigned int virq, unsigned nr_irqs, void *data)
{
	int ret, i;
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;

	ret = qcom_mpm_gpio_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq+i, hwirq+i,
					      &qcom_mpm_gpio_chip, NULL);
	return 0;
}
static int qcom_mpm_gic_alloc(struct irq_domain *domain,
	unsigned int virq, unsigned nr_irqs, void *data)
{
	struct irq_fwspec *fwspec = data;
	struct irq_fwspec parent_fwspec;
	irq_hw_number_t hwirq;
	int i;

	if (fwspec->param_count != 3)
		return -EINVAL;
	if (fwspec->param[0] != 0)
		return -EINVAL;

	hwirq = fwspec->param[1];

	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq +i,
			&qcom_mpm_gic_chip, NULL);
	}

	parent_fwspec = *fwspec;
	parent_fwspec.fwnode = domain->parent->fwnode;
	return irq_domain_alloc_irqs_parent(domain, virq, nr_irqs,
					    &parent_fwspec);
}

static const struct irq_domain_ops qcom_mpm_gpio_domain_ops = {
	.translate	= qcom_mpm_gpio_translate,
	.alloc		= qcom_mpm_gpio_alloc,
	.free		= irq_domain_free_irqs_common,
};

static const struct irq_domain_ops qcom_mpm_gic_domain_ops = {
	.translate	= qcom_mpm_gic_translate,
	.alloc		= qcom_mpm_gic_alloc,
	.free		= irq_domain_free_irqs_common,
};

struct mpm_pin {
	uint8_t pin;
	int source;
	irq_hw_number_t hwirq;	
};

enum {
	QCOM_MPM_SOURCE_GIC,
	QCOM_MPM_SOURCE_GPIO,
} mpm_pin_source;

static const struct mpm_pin mpm_apq8084_data[] = {
	{2, QCOM_MPM_SOURCE_GIC, 216},
	{3, QCOM_MPM_SOURCE_GPIO, 1},
	{4, QCOM_MPM_SOURCE_GPIO, 5},
	{5, QCOM_MPM_SOURCE_GPIO, 8},
	{6, QCOM_MPM_SOURCE_GPIO, 9},
	{7, QCOM_MPM_SOURCE_GPIO, 28},
	{8, QCOM_MPM_SOURCE_GPIO, 34},
	{9, QCOM_MPM_SOURCE_GPIO, 30},
	{10, QCOM_MPM_SOURCE_GPIO, 44},
	{11, QCOM_MPM_SOURCE_GPIO, 48},
	{12, QCOM_MPM_SOURCE_GPIO, 52},
	{13, QCOM_MPM_SOURCE_GPIO, 55},
	{14, QCOM_MPM_SOURCE_GPIO, 56},
	{15, QCOM_MPM_SOURCE_GPIO, 58},
	{16, QCOM_MPM_SOURCE_GPIO, 60},
	{17, QCOM_MPM_SOURCE_GPIO, 64},
	{18, QCOM_MPM_SOURCE_GPIO, 67},
	{19, QCOM_MPM_SOURCE_GPIO, 68},
	{20, QCOM_MPM_SOURCE_GPIO, 69},
	{21, QCOM_MPM_SOURCE_GPIO, 76},
	{22, QCOM_MPM_SOURCE_GPIO, 77},
	{23, QCOM_MPM_SOURCE_GPIO, 78},
	{24, QCOM_MPM_SOURCE_GPIO, 79},
	{25, QCOM_MPM_SOURCE_GPIO, 84},
	{26, QCOM_MPM_SOURCE_GPIO, 95},
	{27, QCOM_MPM_SOURCE_GPIO, 102},
	{28, QCOM_MPM_SOURCE_GPIO, 103},
	{29, QCOM_MPM_SOURCE_GPIO, 104},
	{30, QCOM_MPM_SOURCE_GPIO, 105},
	{31, QCOM_MPM_SOURCE_GPIO, 107},
	{32, QCOM_MPM_SOURCE_GPIO, 109},
	{33, QCOM_MPM_SOURCE_GPIO, 111},
	{34, QCOM_MPM_SOURCE_GPIO, 113},
	{35, QCOM_MPM_SOURCE_GPIO, 121},
	{36, QCOM_MPM_SOURCE_GPIO, 122},
	{37, QCOM_MPM_SOURCE_GPIO, 123},
	{38, QCOM_MPM_SOURCE_GPIO, 131},
	{40, QCOM_MPM_SOURCE_GPIO, 139},
	{41, QCOM_MPM_SOURCE_GPIO, 141},
	{48, QCOM_MPM_SOURCE_GIC, 165},
	{49, QCOM_MPM_SOURCE_GIC, 165},
	{50, QCOM_MPM_SOURCE_GIC, 172},
	{53, QCOM_MPM_SOURCE_GIC, 104},
	{62, QCOM_MPM_SOURCE_GIC, 222},
};

static const struct mpm_pin **mpm_apq8084_pdata = {
	mpm_apq8084_data,
};

static const struct mpm_pin mpm_msm8916_data[] = {
	{2, QCOM_MPM_SOURCE_GIC, 216},
	{3, QCOM_MPM_SOURCE_GPIO, 108},
	{4, QCOM_MPM_SOURCE_GPIO, 1},
	{5, QCOM_MPM_SOURCE_GPIO, 5},
	{6, QCOM_MPM_SOURCE_GPIO, 9},
	{7, QCOM_MPM_SOURCE_GPIO, 107},
	{8, QCOM_MPM_SOURCE_GPIO, 98},
	{9, QCOM_MPM_SOURCE_GPIO, 97},
	{10, QCOM_MPM_SOURCE_GPIO, 11},
	{11, QCOM_MPM_SOURCE_GPIO, 69},
	{12, QCOM_MPM_SOURCE_GPIO, 12},
	{13, QCOM_MPM_SOURCE_GPIO, 13},
	{14, QCOM_MPM_SOURCE_GPIO, 20},
	{15, QCOM_MPM_SOURCE_GPIO, 62},
	{16, QCOM_MPM_SOURCE_GPIO, 54},
	{17, QCOM_MPM_SOURCE_GPIO, 21},
	{18, QCOM_MPM_SOURCE_GPIO, 52},
	{19, QCOM_MPM_SOURCE_GPIO, 25},
	{20, QCOM_MPM_SOURCE_GPIO, 51},
	{21, QCOM_MPM_SOURCE_GPIO, 50},
	{22, QCOM_MPM_SOURCE_GPIO, 28},
	{23, QCOM_MPM_SOURCE_GPIO, 31},
	{24, QCOM_MPM_SOURCE_GPIO, 34},
	{25, QCOM_MPM_SOURCE_GPIO, 35},
	{26, QCOM_MPM_SOURCE_GPIO, 36},
	{27, QCOM_MPM_SOURCE_GPIO, 37},
	{28, QCOM_MPM_SOURCE_GPIO, 38},
	{29, QCOM_MPM_SOURCE_GPIO, 49},
	{30, QCOM_MPM_SOURCE_GPIO, 109},
	{31, QCOM_MPM_SOURCE_GPIO, 110},
	{32, QCOM_MPM_SOURCE_GPIO, 111},
	{33, QCOM_MPM_SOURCE_GPIO, 112},
	{34, QCOM_MPM_SOURCE_GPIO, 113},
	{35, QCOM_MPM_SOURCE_GPIO, 114},
	{36, QCOM_MPM_SOURCE_GPIO, 115},
	{37, QCOM_MPM_SOURCE_GPIO, 117},
	{38, QCOM_MPM_SOURCE_GPIO, 118},
	{39, QCOM_MPM_SOURCE_GPIO, 120},
	{40, QCOM_MPM_SOURCE_GPIO, 121},
	{49, QCOM_MPM_SOURCE_GIC, 172},
	{50, QCOM_MPM_SOURCE_GPIO, 66},
	{51, QCOM_MPM_SOURCE_GPIO, 68},
	{53, QCOM_MPM_SOURCE_GIC, 104},
	{58, QCOM_MPM_SOURCE_GIC, 166},
	{62, QCOM_MPM_SOURCE_GIC, 222},
};

static const struct mpm_pin **msm8916_pdata = {
	mpm_msm8916_data,
};

#if 1
static const struct of_device_id qcom_mpm_dt_match[] = {
	{ .compatible = "qcom,mpm-msm8916", .data = mpm_msm8916_data},
	{ .compatible = "qcom,mpm-apq8074", .data = mpm_apq8084_data},
	{ },
};

static const struct of_device_id qcom_mpm_child_dt_match[] = {
	{ .compatible = "qcom,mpm-gpio", .data = &qcom_mpm_gpio_chip },
	{ .compatible = "qcom,mpm-gic", .data = &qcom_mpm_gic_chip },
	{ },
};

static irqreturn_t qcom_mpm_irq(int irq, void *dev_id)
{

	/*
	 * Triggered by RPM when system resumes from deep sleep
	 */
	return IRQ_HANDLED;
}

static int mpm_notifier(struct notifier_block *self, unsigned long cmd,
	void *v)
{
	printk(KERN_ERR "GOT PM BLOCK\n");

	return NOTIFY_OK;
}

static struct notifier_block mpm_notifier_block = {
	.notifier_call = mpm_notifier,
};

static int qcom_mpm_probe(struct platform_device *pdev)
{ 
	struct qcom_mpm *mpm;
	struct resource r;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child, *irq_parent_node;
	struct irq_domain *gic_parent;
	const struct of_device_id *match;
	int ret;

	__mpm.dev = &pdev->dev;

	np = of_parse_phandle(pdev->dev.of_node, "qcom,rpm-msg-ram", 0);
	if (!np) {
		dev_err(&pdev->dev,
			"qcom,rpm-msg-ram entry not found\n");
		return -EINVAL;
	}

	__mpm.ipc_irq = platform_get_irq(pdev, 0);
	if (__mpm.ipc_irq < 0) {
		dev_err(&pdev->dev, "no IRQ resource info\n");
		return __mpm.ipc_irq;
	}

	ret = devm_request_irq(&pdev->dev, __mpm.ipc_irq, qcom_mpm_irq,
		IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND, pdev->name,
		qcom_mpm_irq);

	cpu_pm_register_notifier(&mpm_notifier_block);

	printk(KERN_ERR "=======================================\n");
	return 0;
}

static struct platform_driver qcom_mpm_driver = {
	.probe = qcom_mpm_probe,
	.driver = {
		.name = "qcom,mpm",
		.of_match_table = qcom_mpm_dt_match,
	},
};

static int __init qcom_mpm_init(void)
{
	return platform_driver_register(&qcom_mpm_driver);
}

builtin_platform_driver(qcom_mpm_driver);
#endif

#if 1

static int __init qcom_mpm_gpio_init(struct device_node *node,
				struct device_node *parent)
{
	__mpm.gpio_domain = irq_domain_create_linear(&node->fwnode, 0,
					&qcom_mpm_gpio_domain_ops, NULL);

	if (!__mpm.gpio_domain)
		return -ENOMEM;

	return 0;
}

static int __init qcom_mpm_gic_init(struct device_node *node,
				struct device_node *parent)
{
	struct irq_domain *parent_domain;

	if (!parent) {
		pr_err("%s: no parent\n", node->full_name);
	}

	parent_domain = irq_find_host(parent);
	if (!parent_domain) {
		pr_err("unable to obtain gic parent domain\n");
		return -ENXIO;
	}

	__mpm.gic_domain = irq_domain_add_hierarchy(parent_domain, 0, 0, node,
				&qcom_mpm_gic_domain_ops, NULL);
	if (!__mpm.gic_domain) {
		pr_err("gic domain add failed\n");
		return -ENOMEM;
	}

	return 0;
}

IRQCHIP_DECLARE(mpm_gic, "qcom,mpm-gic", qcom_mpm_gic_init);
IRQCHIP_DECLARE(mpm_gpio, "qcom,mpm-gpio", qcom_mpm_gpio_init);
#endif
