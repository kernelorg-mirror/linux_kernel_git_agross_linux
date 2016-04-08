#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/cpuidle.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include <linux/psci.h>
#include <linux/nmi.h>

#define QCOM_SUSPEND_LOCKUP_DET		1

static bool qcom_disable_lock_det;

static void qcom_pm_enter_freeze(struct cpuidle_device *dev,
	struct cpuidle_driver *drv,
	int index)
{
	if (qcom_disable_lock_det) {
		local_fiq_disable();
		lockup_detector_suspend();
	}

	drv->states[index].enter(dev, drv, index);

	if (qcom_disable_lock_det) {
		lockup_detector_resume();
		local_fiq_enable();
	}
}

static const struct of_device_id qcom_idle_state_match[] = {
	{ .compatible = "qcom,idle-state-spc", },
	{ },
};

static const struct of_device_id qcom_pm_match_table[] = {
	{ .compatible = "qcom,pm-msm8916", },
	{ .compatible = "qcom,pm-apq8084", .data = (void *)QCOM_SUSPEND_LOCKUP_DET },
	{ },
};

static const struct platform_suspend_ops qcom_suspend_ops = {
	.valid          = suspend_valid_only_mem,
};

static int qcom_pm_probe(struct platform_device *pdev)
{
	struct cpuidle_device *cpu_dev;
	struct cpuidle_driver *cpu_drv;
	int state_count;
	struct device_node *state_np, *cpu_np;
	const struct of_device_id *match;
	int i;

	match = of_match_node(qcom_pm_match_table, pdev->dev.of_node);
	if (match)
		qcom_disable_lock_det = match->data ? true : false;

	/* configure CPU enter_freeze if applicable */
	for_each_present_cpu(i) {
		cpu_np = of_get_cpu_node(i, NULL);
		cpu_dev = per_cpu_ptr(cpuidle_devices, i);
		cpu_drv = cpuidle_get_cpu_driver(cpu_dev);

		if (!cpu_dev || !cpu_drv) {
			of_node_put(cpu_np);
			return -EPROBE_DEFER;
		}

		state_count = 0;
		state_np = of_parse_phandle(cpu_np, "cpu-idle-states",
						  state_count);

		while(state_np) {
			match = of_match_node(qcom_idle_state_match,
					      state_np);

			state_count++;
			if (match)
				cpu_drv->states[state_count].enter_freeze =
						&qcom_pm_enter_freeze;
			of_node_put(state_np);

			state_np = of_parse_phandle(cpu_np, "cpu-idle-states",
						    state_count);
		}
		
		of_node_put(cpu_np);
	}

	suspend_set_ops(&qcom_suspend_ops);

	return 0;
}

static struct platform_driver qcom_pm_driver = {
	.probe = qcom_pm_probe,
	.driver = {
		.name = "qcom,pm",
		.of_match_table = qcom_pm_match_table,
	},
};
module_platform_driver(qcom_pm_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QCOM Power Management Driver");
MODULE_ALIAS("platform:qcom_pm");
