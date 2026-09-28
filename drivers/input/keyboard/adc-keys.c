// SPDX-License-Identifier: GPL-2.0-only
/*
 * Input driver for resistor ladder connected on ADC
 *
 * Copyright (c) 2016 Alexandre Belloni
 */

#include <linux/err.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

struct adc_keys_button {
	u32 voltage;
	u32 keycode;
	int raw;	/* sysfs tuning value in raw ADC codes */
};

struct adc_keys_state {
	struct iio_channel *channel;
	u32 num_keys;
	u32 last_key;
	u32 keyup_voltage;
	struct adc_keys_button *map;
	/* raw ADC tuning, disabled when raw_bits == 0 */
	struct mutex tune_lock;
	u32 vref_mv;
	u32 raw_bits;
	int raw_max;
};

struct adc_keys_attr {
	struct device_attribute dattr;
	u32 keycode;
};

static void adc_keys_poll(struct input_dev *input)
{
	struct adc_keys_state *st = input_get_drvdata(input);
	int i, value, ret;
	u32 diff, closest = 0xffffffff;
	int keycode = 0;

	ret = iio_read_channel_processed(st->channel, &value);
	if (unlikely(ret < 0)) {
		/* Forcibly release key if any was pressed */
		value = st->keyup_voltage;
	} else {
		for (i = 0; i < st->num_keys; i++) {
			diff = abs(READ_ONCE(st->map[i].voltage) - value);
			if (diff < closest) {
				closest = diff;
				keycode = st->map[i].keycode;
			}
		}
	}

	if (abs(st->keyup_voltage - value) < closest)
		keycode = 0;

	if (st->last_key && st->last_key != keycode)
		input_report_key(input, st->last_key, 0);

	if (keycode)
		input_report_key(input, keycode, 1);

	input_sync(input);
	st->last_key = keycode;
}

/*
 * Raw ADC tuning needs a linear scale of 2^bits codes over vref, which is
 * how SAR ADCs such as rockchip-saradc report it.
 */
static void adc_keys_init_tuning(struct device *dev, struct adc_keys_state *st)
{
	int val, val2, ret;

	mutex_init(&st->tune_lock);

	ret = iio_read_channel_scale(st->channel, &val, &val2);
	if (ret != IIO_VAL_FRACTIONAL_LOG2 || val <= 0 || val2 <= 0 ||
	    val2 > 16) {
		dev_dbg(dev, "no linear raw scale, ADC tuning disabled\n");
		return;
	}

	st->vref_mv = val;
	st->raw_bits = val2;
	st->raw_max = BIT(val2) - 1;
}

/* Round to nearest, so thresholds derived from raw codes map back exactly */
static int adc_keys_uv_to_raw(const struct adc_keys_state *st, u32 uv)
{
	u64 raw;

	if (!st->raw_bits)
		return -1;

	raw = DIV_ROUND_CLOSEST_ULL((u64)uv << st->raw_bits,
				    st->vref_mv * 1000);

	return min_t(u64, raw, st->raw_max);
}

static int adc_keys_load_keymap(struct device *dev, struct adc_keys_state *st)
{
	struct adc_keys_button *map;
	struct fwnode_handle *child;
	int i;

	st->num_keys = device_get_child_node_count(dev);
	if (st->num_keys == 0) {
		dev_err(dev, "keymap is missing\n");
		return -EINVAL;
	}

	map = devm_kmalloc_array(dev, st->num_keys, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	i = 0;
	device_for_each_child_node(dev, child) {
		if (fwnode_property_read_u32(child, "press-threshold-microvolt",
					     &map[i].voltage)) {
			dev_err(dev, "Key with invalid or missing voltage\n");
			fwnode_handle_put(child);
			return -EINVAL;
		}
		map[i].raw = adc_keys_uv_to_raw(st, map[i].voltage);
		map[i].voltage /= 1000;

		if (fwnode_property_read_u32(child, "linux,code",
					     &map[i].keycode)) {
			dev_err(dev, "Key with invalid or missing linux,code\n");
			fwnode_handle_put(child);
			return -EINVAL;
		}

		i++;
	}

	st->map = map;
	return 0;
}

static struct adc_keys_button *adc_keys_find(struct adc_keys_state *st,
					     u32 keycode)
{
	u32 i;

	for (i = 0; i < st->num_keys; i++)
		if (st->map[i].keycode == keycode)
			return &st->map[i];

	return NULL;
}

static struct adc_keys_button *adc_keys_attr_button(struct device *dev,
						    struct device_attribute *attr)
{
	struct adc_keys_state *st = dev_get_drvdata(dev);
	struct adc_keys_attr *kattr = container_of(attr, struct adc_keys_attr,
						   dattr);

	return adc_keys_find(st, kattr->keycode);
}

/*
 * adc_value_volume_{up,down}: press level in raw ADC codes, same format
 * and range as the rk_keys attributes. A new value is used from the next
 * poll and is not persistent.
 */
static ssize_t adc_value_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct adc_keys_state *st = dev_get_drvdata(dev);
	struct adc_keys_button *button = adc_keys_attr_button(dev, attr);
	int raw;

	mutex_lock(&st->tune_lock);
	raw = button->raw;
	mutex_unlock(&st->tune_lock);

	return sysfs_emit(buf, "%d\n", raw);
}

static ssize_t adc_value_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct adc_keys_state *st = dev_get_drvdata(dev);
	struct adc_keys_button *button = adc_keys_attr_button(dev, attr);
	int raw, mv, ret;

	ret = kstrtoint(buf, 0, &raw);
	if (ret)
		return ret;

	if (raw < 0 || raw > st->raw_max)
		return -EINVAL;

	/* Same raw to mV conversion the poll path applies to readings */
	ret = iio_convert_raw_to_processed(st->channel, raw, &mv, 1);
	if (ret)
		return ret;

	mutex_lock(&st->tune_lock);
	button->raw = raw;
	WRITE_ONCE(button->voltage, mv);
	mutex_unlock(&st->tune_lock);

	return count;
}

#define ADC_KEYS_ATTR(_name, _code)					\
	struct adc_keys_attr adc_keys_attr_##_name = {			\
		.dattr = __ATTR(adc_value_##_name, 0644,		\
				adc_value_show, adc_value_store),	\
		.keycode = _code,					\
	}

static ADC_KEYS_ATTR(volume_up, KEY_VOLUMEUP);
static ADC_KEYS_ATTR(volume_down, KEY_VOLUMEDOWN);

static struct attribute *adc_keys_attrs[] = {
	&adc_keys_attr_volume_up.dattr.attr,
	&adc_keys_attr_volume_down.dattr.attr,
	NULL
};

static umode_t adc_keys_attr_is_visible(struct kobject *kobj,
					struct attribute *attr, int n)
{
	struct adc_keys_state *st = dev_get_drvdata(kobj_to_dev(kobj));
	struct adc_keys_attr *kattr = container_of(attr, struct adc_keys_attr,
						   dattr.attr);

	if (!st->raw_bits || !adc_keys_find(st, kattr->keycode))
		return 0;

	return attr->mode;
}

static const struct attribute_group adc_keys_group = {
	.attrs = adc_keys_attrs,
	.is_visible = adc_keys_attr_is_visible,
};
__ATTRIBUTE_GROUPS(adc_keys);

static int adc_keys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct adc_keys_state *st;
	struct input_dev *input;
	enum iio_chan_type type;
	int i, value;
	int error;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->channel = devm_iio_channel_get(dev, "buttons");
	if (IS_ERR(st->channel))
		return PTR_ERR(st->channel);

	if (!st->channel->indio_dev)
		return -ENXIO;

	error = iio_get_channel_type(st->channel, &type);
	if (error < 0)
		return error;

	if (type != IIO_VOLTAGE) {
		dev_err(dev, "Incompatible channel type %d\n", type);
		return -EINVAL;
	}

	if (device_property_read_u32(dev, "keyup-threshold-microvolt",
				     &st->keyup_voltage)) {
		dev_err(dev, "Invalid or missing keyup voltage\n");
		return -EINVAL;
	}
	st->keyup_voltage /= 1000;

	adc_keys_init_tuning(dev, st);

	error = adc_keys_load_keymap(dev, st);
	if (error)
		return error;

	platform_set_drvdata(pdev, st);

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	input_set_drvdata(input, st);

	input->name = pdev->name;
	input->phys = "adc-keys/input0";

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	__set_bit(EV_KEY, input->evbit);
	for (i = 0; i < st->num_keys; i++)
		__set_bit(st->map[i].keycode, input->keybit);

	if (device_property_read_bool(dev, "autorepeat"))
		__set_bit(EV_REP, input->evbit);


	error = input_setup_polling(input, adc_keys_poll);
	if (error) {
		dev_err(dev, "Unable to set up polling: %d\n", error);
		return error;
	}

	if (!device_property_read_u32(dev, "poll-interval", &value))
		input_set_poll_interval(input, value);

	error = input_register_device(input);
	if (error) {
		dev_err(dev, "Unable to register input device: %d\n", error);
		return error;
	}

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id adc_keys_of_match[] = {
	{ .compatible = "adc-keys", },
	{ }
};
MODULE_DEVICE_TABLE(of, adc_keys_of_match);
#endif

static struct platform_driver __refdata adc_keys_driver = {
	.driver = {
		.name = "adc_keys",
		.of_match_table = of_match_ptr(adc_keys_of_match),
		.dev_groups = adc_keys_groups,
	},
	.probe = adc_keys_probe,
};
module_platform_driver(adc_keys_driver);

MODULE_AUTHOR("Alexandre Belloni <alexandre.belloni@free-electrons.com>");
MODULE_DESCRIPTION("Input driver for resistor ladder connected on ADC");
MODULE_LICENSE("GPL v2");
