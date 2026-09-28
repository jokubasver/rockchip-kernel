// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/drivers/leds-pwm.c
 *
 * simple PWM based LED control
 *
 * Copyright 2009 Luotao Fu @ Pengutronix (l.fu@pengutronix.de)
 *
 * based on leds-gpio.c by Raphael Assenat <raph@8d.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/power_supply.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define LED_PWM_BATTERY_DELAY_MS	1000
#define LED_PWM_BATTERY_POLL_MS		5000

struct led_pwm {
	const char	*name;
	u8		active_low;
	bool		default_on;
	bool		battery_scale;
	unsigned int	max_brightness;
};

struct led_pwm_data {
	struct led_classdev	cdev;
	struct pwm_device	*pwm;
	struct pwm_state	pwmstate;
	unsigned int		active_low;
	bool			battery_scale;
	struct power_supply	*battery;
	struct delayed_work	battery_work;
};

struct led_pwm_priv {
	int num_leds;
	struct led_pwm_data leds[];
};

static int led_pwm_set(struct led_classdev *led_cdev,
		       enum led_brightness brightness)
{
	struct led_pwm_data *led_dat =
		container_of(led_cdev, struct led_pwm_data, cdev);
	unsigned int max = led_dat->cdev.max_brightness;
	unsigned long long duty = led_dat->pwmstate.period;

	duty *= brightness;
	do_div(duty, max);

	if (led_dat->active_low)
		duty = led_dat->pwmstate.period - duty;

	led_dat->pwmstate.duty_cycle = duty;
	led_dat->pwmstate.enabled = true;
	return pwm_apply_state(led_dat->pwm, &led_dat->pwmstate);
}

/* battery-scale: 51-100% full, 30-50% dim, 0-29% off */
static void led_pwm_battery_work(struct work_struct *work)
{
	struct led_pwm_data *led_dat = container_of(to_delayed_work(work),
						    struct led_pwm_data,
						    battery_work);
	union power_supply_propval val;
	enum led_brightness brightness;

	if (!IS_ENABLED(CONFIG_POWER_SUPPLY))
		return;

	if (!led_dat->battery)
		led_dat->battery = power_supply_get_by_name("battery");

	if (led_dat->battery &&
	    !power_supply_get_property(led_dat->battery,
				       POWER_SUPPLY_PROP_CAPACITY, &val)) {
		if (val.intval >= 51)
			brightness = LED_FULL;
		else if (val.intval >= 30)
			brightness = 140;
		else
			brightness = LED_OFF;

		led_set_brightness_sync(&led_dat->cdev, brightness);
	}

	queue_delayed_work(system_freezable_wq, &led_dat->battery_work,
			   msecs_to_jiffies(LED_PWM_BATTERY_POLL_MS));
}

static void led_pwm_battery_release(void *data)
{
	struct led_pwm_data *led_dat = data;

	cancel_delayed_work_sync(&led_dat->battery_work);
	if (IS_ENABLED(CONFIG_POWER_SUPPLY) && led_dat->battery)
		power_supply_put(led_dat->battery);
	led_dat->battery = NULL;
}

__attribute__((nonnull))
static int led_pwm_add(struct device *dev, struct led_pwm_priv *priv,
		       struct led_pwm *led, struct fwnode_handle *fwnode)
{
	struct led_pwm_data *led_data = &priv->leds[priv->num_leds];
	struct led_init_data init_data = { .fwnode = fwnode };
	int ret;

	led_data->active_low = led->active_low;
	led_data->cdev.name = led->name;
	led_data->cdev.max_brightness = led->max_brightness;
	if (led->default_on)
		led_data->cdev.brightness = led->max_brightness ?: LED_FULL;
	else
		led_data->cdev.brightness = LED_OFF;
	led_data->cdev.flags = LED_CORE_SUSPENDRESUME;

	led_data->pwm = devm_fwnode_pwm_get(dev, fwnode, NULL);
	if (IS_ERR(led_data->pwm))
		return dev_err_probe(dev, PTR_ERR(led_data->pwm),
				     "unable to request PWM for %s\n",
				     led->name);

	led_data->cdev.brightness_set_blocking = led_pwm_set;

	pwm_init_state(led_data->pwm, &led_data->pwmstate);

	ret = devm_led_classdev_register_ext(dev, &led_data->cdev, &init_data);
	if (ret) {
		dev_err(dev, "failed to register PWM led for %s: %d\n",
			led->name, ret);
		return ret;
	}

	ret = led_pwm_set(&led_data->cdev, led_data->cdev.brightness);
	if (ret) {
		dev_err(dev, "failed to set led PWM value for %s: %d",
			led->name, ret);
		return ret;
	}

	if (led->battery_scale) {
		INIT_DELAYED_WORK(&led_data->battery_work,
				  led_pwm_battery_work);
		ret = devm_add_action_or_reset(dev, led_pwm_battery_release,
					       led_data);
		if (ret)
			return ret;

		led_data->battery_scale = true;
		queue_delayed_work(system_freezable_wq,
				   &led_data->battery_work,
				   msecs_to_jiffies(LED_PWM_BATTERY_DELAY_MS));
	}

	priv->num_leds++;
	return 0;
}

static int led_pwm_create_fwnode(struct device *dev, struct led_pwm_priv *priv)
{
	struct fwnode_handle *fwnode;
	struct led_pwm led;
	const char *state;
	int ret = 0;

	memset(&led, 0, sizeof(led));

	device_for_each_child_node(dev, fwnode) {
		ret = fwnode_property_read_string(fwnode, "label", &led.name);
		if (ret && is_of_node(fwnode))
			led.name = to_of_node(fwnode)->name;

		if (!led.name) {
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		led.active_low = fwnode_property_read_bool(fwnode,
							   "active-low");
		fwnode_property_read_u32(fwnode, "max-brightness",
					 &led.max_brightness);

		led.default_on = !fwnode_property_read_string(fwnode,
							       "default-state",
							       &state) &&
				 !strcmp(state, "on");
		led.battery_scale = fwnode_property_read_bool(fwnode,
							      "battery-scale");

		ret = led_pwm_add(dev, priv, &led, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			break;
		}
	}

	return ret;
}

static int led_pwm_probe(struct platform_device *pdev)
{
	struct led_pwm_priv *priv;
	int ret = 0;
	int count;

	count = device_get_child_node_count(&pdev->dev);

	if (!count)
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev, struct_size(priv, leds, count),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = led_pwm_create_fwnode(&pdev->dev, priv);

	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static void led_pwm_shutdown(struct platform_device *pdev)
{
	struct led_pwm_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->num_leds; i++)
		if (priv->leds[i].battery_scale)
			cancel_delayed_work_sync(&priv->leds[i].battery_work);
}

static const struct of_device_id of_pwm_leds_match[] = {
	{ .compatible = "pwm-leds", },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_leds_match);

static struct platform_driver led_pwm_driver = {
	.probe		= led_pwm_probe,
	.shutdown	= led_pwm_shutdown,
	.driver		= {
		.name	= "leds_pwm",
		.of_match_table = of_pwm_leds_match,
	},
};

module_platform_driver(led_pwm_driver);

MODULE_AUTHOR("Luotao Fu <l.fu@pengutronix.de>");
MODULE_DESCRIPTION("generic PWM LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:leds-pwm");
