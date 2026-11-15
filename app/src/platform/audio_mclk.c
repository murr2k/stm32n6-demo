/*
 * Clock-controller shim for the codec's MCLK. See the binding
 * dts/bindings/clock/n6demo,audio-mclk.yaml.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>

#define DT_DRV_COMPAT n6demo_audio_mclk

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int mclk_on(const struct device *dev, clock_control_subsys_t sys)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	return 0; /* the SAI drives MCLK as soon as it is configured */
}

static int mclk_off(const struct device *dev, clock_control_subsys_t sys)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	return 0;
}

static int mclk_get_rate(const struct device *dev, clock_control_subsys_t sys, uint32_t *rate)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	*rate = DT_INST_PROP(0, clock_frequency);
	return 0;
}

static DEVICE_API(clock_control, mclk_api) = {
	.on = mclk_on,
	.off = mclk_off,
	.get_rate = mclk_get_rate,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, PRE_KERNEL_1, CONFIG_CLOCK_CONTROL_INIT_PRIORITY, &mclk_api);

#endif
