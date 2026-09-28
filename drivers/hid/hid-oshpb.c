// SPDX-License-Identifier: GPL-2.0
/*
 * HID driver for OpenSimHardware OSH PB Controller
 * Presents as odroidgo3-joypad compatible input device.
 *
 * The device is a USB HID gamepad (VID:1209 PID:3100) with analog sticks
 * in 0-4095 range. This driver intercepts the raw HID reports, remaps
 * axes/buttons to match odroidgo3-joypad and applies the tuning values,
 * optional axis spike filter, runtime button swap (swap_ab/swap_xy) and
 * PWM rumble described by the "odroidgo3-hid-joypad" DT node.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define USB_VENDOR_ID_OSHPB	0x1209
#define USB_DEVICE_ID_OSHPB	0x3100

#define OSHPB_NAME		"GO-Super Gamepad"
#define OSHPB_PHYS		"odroidgo3_joypad/input0"
#define OSHPB_VENDOR		0x484B
#define OSHPB_PRODUCT		0x1100
#define OSHPB_REVISION		0x0100

/* analog stick center and range */
#define OSHPB_AXIS_CENTER	2048
#define OSHPB_AXIS_MAX		4095
#define OSHPB_AXIS_FUZZ		32
#define OSHPB_AXIS_FLAT		32

/* tuning default (percent) */
#define OSHPB_TUNING_DEFAULT	200

/* deadzone in raw ADC units */
#define OSHPB_DEADZONE_DEFAULT	64

#define OSHPB_MAX_BUTTONS	64

struct oshpb_button {
	int hid_bit;		/* bit position in HID report bytes 1-8 */
	int linux_code;		/* evdev code (BTN_EAST, etc.) */
};

/* per-axis glitch filter state */
struct oshpb_axis {
	int reported;		/* last value passed to the input layer */
	int prev;		/* previous sample, accepted or dropped */
};

struct oshpb_device {
	struct hid_device *hdev;
	struct input_dev *input;

	/* tuning values (percent) */
	int tuning_x_p, tuning_x_n;
	int tuning_y_p, tuning_y_n;
	int tuning_rx_p, tuning_rx_n;
	int tuning_ry_p, tuning_ry_n;

	int deadzone;
	int scale;

	/* per-report axis glitch filter (0 = off) */
	int max_step;
	struct oshpb_axis ax_lx, ax_ly, ax_rx, ax_ry;

	/* button swap: 0 = as-is, 1 = swap */
	int swap_ab;			/* BTN_EAST <-> BTN_SOUTH */
	int swap_xy;			/* BTN_WEST <-> BTN_NORTH */
	struct mutex swap_lock;		/* serializes swap sysfs stores */

	/* debug: log first N reports */
	int debug_count;

	/* button mapping from DT */
	struct oshpb_button buttons[OSHPB_MAX_BUTTONS];
	int button_count;

	/* stick-switch-key: when held, left stick mirrors to right stick */
	int fn_bit;		/* stick-switch-key HID bit, -1 = disabled */
	int l3_bit;		/* HID bit for L3 (left stick click) */
	int r3_bit;		/* HID bit for R3 (right stick click) */

	/* PWM rumble */
	struct pwm_device *pwm;
	struct work_struct play_work;
	u16 level;
	u16 boost_weak;
	u16 boost_strong;
	bool has_rumble;

	/* reports start at hid_hw_open(), before the input device exists */
	bool ready;
};

/* ---- Rumble helpers ---- */

static int oshpb_pwm_start(struct oshpb_device *oshpb)
{
	struct pwm_state state;

	pwm_get_state(oshpb->pwm, &state);
	pwm_set_relative_duty_cycle(&state, oshpb->level, 0xffff);
	state.enabled = true;

	return pwm_apply_state(oshpb->pwm, &state);
}

static void oshpb_pwm_stop(struct oshpb_device *oshpb)
{
	pwm_disable(oshpb->pwm);
}

static void oshpb_play_work(struct work_struct *work)
{
	struct oshpb_device *oshpb = container_of(work,
					struct oshpb_device, play_work);

	if (oshpb->level)
		oshpb_pwm_start(oshpb);
	else
		oshpb_pwm_stop(oshpb);
}

static int oshpb_rumble_play(struct input_dev *dev, void *data,
			     struct ff_effect *effect)
{
	struct oshpb_device *oshpb = data;
	u32 boosted_level;

	if (effect->type != FF_RUMBLE)
		return 0;

	if (effect->u.rumble.strong_magnitude)
		boosted_level = effect->u.rumble.strong_magnitude + oshpb->boost_strong;
	else
		boosted_level = effect->u.rumble.weak_magnitude + oshpb->boost_weak;

	oshpb->level = min_t(u32, boosted_level, 0xffff);
	schedule_work(&oshpb->play_work);
	return 0;
}

static int oshpb_rumble_setup(struct hid_device *hdev, struct oshpb_device *oshpb,
			      struct device_node *joypad_np)
{
	struct pwm_device *pwm;
	struct pwm_state state;
	u32 boost_weak = 0, boost_strong = 0;
	int error;

	pwm = devm_of_pwm_get(&hdev->dev, joypad_np, "enable");
	if (IS_ERR(pwm)) {
		hid_dbg(hdev, "PWM get failed: %ld\n", PTR_ERR(pwm));
		return PTR_ERR(pwm);
	}
	oshpb->pwm = pwm;

	pwm_init_state(pwm, &state);
	state.enabled = false;
	error = pwm_apply_state(pwm, &state);
	if (error) {
		hid_err(hdev, "failed to apply initial PWM state: %d\n", error);
		oshpb->pwm = NULL;
		return error;
	}

	hid_info(hdev, "rumble via PWM: period=%llu\n", pwm_get_period(pwm));

	/* read boost values from joypad node */
	of_property_read_u32(joypad_np, "rumble-boost-weak", &boost_weak);
	of_property_read_u32(joypad_np, "rumble-boost-strong", &boost_strong);
	oshpb->boost_weak = boost_weak;
	oshpb->boost_strong = boost_strong;

	return 0;
}

/* runs on unbind after the input device is unregistered */
static void oshpb_rumble_teardown(void *data)
{
	struct oshpb_device *oshpb = data;

	cancel_work_sync(&oshpb->play_work);
	if (oshpb->pwm)
		oshpb_pwm_stop(oshpb);
}

/* ---- Button swap helpers ---- */

/*
 * swap_ab: A<->B (BTN_EAST <-> BTN_SOUTH), swap_xy: X<->Y (BTN_WEST <-> BTN_NORTH).
 * Unrelated codes pass through unchanged.
 */
static int oshpb_swap_ab_code(int code)
{
	switch (code) {
	case BTN_EAST:	return BTN_SOUTH;
	case BTN_SOUTH:	return BTN_EAST;
	default:	return code;
	}
}

static int oshpb_swap_xy_code(int code)
{
	switch (code) {
	case BTN_WEST:	return BTN_NORTH;
	case BTN_NORTH:	return BTN_WEST;
	default:	return code;
	}
}

/* lock-free read, paired with WRITE_ONCE() in the sysfs store path */
static int oshpb_remap_button(struct oshpb_device *oshpb, int code)
{
	if (READ_ONCE(oshpb->swap_ab))
		code = oshpb_swap_ab_code(code);
	if (READ_ONCE(oshpb->swap_xy))
		code = oshpb_swap_xy_code(code);
	return code;
}

/*
 * Drop a sample deviating more than max_step from both the last reported
 * value and the previous sample (isolated glitch) and hold the last value.
 * A sample that stays far away on the next report is accepted at once.
 */
static int oshpb_axis_filter(struct oshpb_axis *axis, int sample, int max_step)
{
	int delta_rep = abs(sample - axis->reported);
	int delta_prev = abs(sample - axis->prev);

	axis->prev = sample;

	if (delta_rep > max_step && delta_prev > max_step)
		return axis->reported;

	axis->reported = sample;
	return sample;
}

/* ---- HID raw event handler ---- */

/*
 * Report ID 1 (gamepad):
 *   Byte      0: Report ID (0x01)
 *   Bytes   1-8: 64 buttons (bit field)
 *   Bytes  9-10: Rx (left stick Y), 16-bit LE, 0-4095
 *   Bytes 11-12: Ry (right stick X)
 *   Bytes 13-14: Rz (right stick Y)
 *   Bytes 15-18: Sliders (unused)
 *   Bytes 19-20: X  (left stick X)
 *   Byte     21: Hat switch 1 (0-7, 8 = center)
 *   Bytes 22-24: Hat switches 2-4 (unused)
 */
#define OSHPB_REPORT_ID		0x01
#define OSHPB_REPORT_SIZE	25

static int oshpb_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *data, int size)
{
	struct oshpb_device *oshpb = hid_get_drvdata(hdev);
	struct input_dev *input;
	int lx, ly, rx, ry, hat, i;
	u64 btns;

	/* pairs with smp_store_release() once the input device is registered */
	if (!smp_load_acquire(&oshpb->ready))
		return 0;
	input = oshpb->input;

	if (size < OSHPB_REPORT_SIZE)
		return 0;

	if (data[0] != OSHPB_REPORT_ID)
		return 0;

	/* 0..4095 -> -2048..2047 */
	lx = (data[19] | (data[20] << 8)) - OSHPB_AXIS_CENTER;
	ly = (data[9]  | (data[10] << 8)) - OSHPB_AXIS_CENTER;
	rx = (data[11] | (data[12] << 8)) - OSHPB_AXIS_CENTER;
	ry = (data[13] | (data[14] << 8)) - OSHPB_AXIS_CENTER;

	/* invert left stick (odroidgo3-joypad invert-absx/invert-absy) */
	lx = -lx;
	ly = -ly;

	/* log the first 10 reports (dynamic debug) */
	if (oshpb->debug_count < 10) {
		oshpb->debug_count++;
		hid_dbg(hdev,
			"raw[%d]: lx=%d ly=%d rx=%d ry=%d btn=%02x%02x%02x%02x%02x%02x%02x%02x hat=%d\n",
			oshpb->debug_count, lx + OSHPB_AXIS_CENTER, ly + OSHPB_AXIS_CENTER,
			rx + OSHPB_AXIS_CENTER, ry + OSHPB_AXIS_CENTER,
			data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8],
			data[21]);
	}

	if (oshpb->deadzone) {
		if (abs(lx) < oshpb->deadzone)
			lx = 0;
		if (abs(ly) < oshpb->deadzone)
			ly = 0;
		if (abs(rx) < oshpb->deadzone)
			rx = 0;
		if (abs(ry) < oshpb->deadzone)
			ry = 0;
	}

	if (lx > 0 && oshpb->tuning_x_p)
		lx = (lx * oshpb->tuning_x_p) / 100;
	else if (lx < 0 && oshpb->tuning_x_n)
		lx = (lx * oshpb->tuning_x_n) / 100;

	if (ly > 0 && oshpb->tuning_y_p)
		ly = (ly * oshpb->tuning_y_p) / 100;
	else if (ly < 0 && oshpb->tuning_y_n)
		ly = (ly * oshpb->tuning_y_n) / 100;

	if (rx > 0 && oshpb->tuning_rx_p)
		rx = (rx * oshpb->tuning_rx_p) / 100;
	else if (rx < 0 && oshpb->tuning_rx_n)
		rx = (rx * oshpb->tuning_rx_n) / 100;

	if (ry > 0 && oshpb->tuning_ry_p)
		ry = (ry * oshpb->tuning_ry_p) / 100;
	else if (ry < 0 && oshpb->tuning_ry_n)
		ry = (ry * oshpb->tuning_ry_n) / 100;

	if (oshpb->scale) {
		lx *= oshpb->scale;
		ly *= oshpb->scale;
		rx *= oshpb->scale;
		ry *= oshpb->scale;
	}

	/* spike filter on the physical axes, before stick-switch emulation */
	if (oshpb->max_step) {
		lx = oshpb_axis_filter(&oshpb->ax_lx, lx, oshpb->max_step);
		ly = oshpb_axis_filter(&oshpb->ax_ly, ly, oshpb->max_step);
		rx = oshpb_axis_filter(&oshpb->ax_rx, rx, oshpb->max_step);
		ry = oshpb_axis_filter(&oshpb->ax_ry, ry, oshpb->max_step);
	}

	btns = (u64)data[1]         | ((u64)data[2] << 8)  |
	       ((u64)data[3] << 16) | ((u64)data[4] << 24) |
	       ((u64)data[5] << 32) | ((u64)data[6] << 40) |
	       ((u64)data[7] << 48) | ((u64)data[8] << 56);

	/*
	 * stick-switch-key held: left stick drives the right stick and
	 * L3 acts as R3.
	 */
	if (oshpb->fn_bit >= 0 && (btns & BIT_ULL(oshpb->fn_bit))) {
		rx = lx;
		ry = ly;
		lx = 0;
		ly = 0;
		if (oshpb->l3_bit >= 0 && oshpb->r3_bit >= 0) {
			if (btns & BIT_ULL(oshpb->l3_bit)) {
				btns |= BIT_ULL(oshpb->r3_bit);
				btns &= ~BIT_ULL(oshpb->l3_bit);
			} else {
				btns &= ~BIT_ULL(oshpb->r3_bit);
			}
		}
	}

	input_report_abs(input, ABS_X, lx);
	input_report_abs(input, ABS_Y, ly);
	input_report_abs(input, ABS_RX, rx);
	input_report_abs(input, ABS_RY, ry);

	/* hat -> D-pad buttons */
	hat = data[21];
	input_report_key(input, BTN_DPAD_UP,    hat == 0 || hat == 1 || hat == 7);
	input_report_key(input, BTN_DPAD_RIGHT, hat == 1 || hat == 2 || hat == 3);
	input_report_key(input, BTN_DPAD_DOWN,  hat == 3 || hat == 4 || hat == 5);
	input_report_key(input, BTN_DPAD_LEFT,  hat == 5 || hat == 6 || hat == 7);

	for (i = 0; i < oshpb->button_count; i++) {
		int bit = oshpb->buttons[i].hid_bit;

		input_report_key(input,
				 oshpb_remap_button(oshpb, oshpb->buttons[i].linux_code),
				 btns & BIT_ULL(bit));
	}

	input_sync(input);
	return 0;
}

/* ---- Button swap sysfs (swap_ab / swap_xy) ---- */

/*
 * /sys/bus/hid/devices/<id>/swap_ab, swap_xy [rw]:
 * 0 = report as-is (default), 1 = swap. Other input returns -EINVAL.
 */
static ssize_t swap_ab_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct oshpb_device *oshpb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", oshpb->swap_ab);
}

static ssize_t swap_xy_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct oshpb_device *oshpb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", oshpb->swap_xy);
}

/*
 * On a swap change force-release the four face buttons so a held key
 * cannot stay pressed under its old code.
 */
static int oshpb_store_swap(struct oshpb_device *oshpb, int *field,
			    const char *buf)
{
	unsigned int enable;
	int error;

	error = kstrtouint(buf, 10, &enable);
	if (error)
		return error;
	if (enable > 1)
		return -EINVAL;

	mutex_lock(&oshpb->swap_lock);
	if (enable != READ_ONCE(*field)) {
		WRITE_ONCE(*field, enable);
		if (smp_load_acquire(&oshpb->ready)) {
			input_report_key(oshpb->input, BTN_EAST, 0);
			input_report_key(oshpb->input, BTN_SOUTH, 0);
			input_report_key(oshpb->input, BTN_WEST, 0);
			input_report_key(oshpb->input, BTN_NORTH, 0);
			input_sync(oshpb->input);
		}
	}
	mutex_unlock(&oshpb->swap_lock);

	return 0;
}

static ssize_t swap_ab_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct oshpb_device *oshpb = dev_get_drvdata(dev);
	int error;

	error = oshpb_store_swap(oshpb, &oshpb->swap_ab, buf);
	return error ? error : count;
}

static ssize_t swap_xy_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct oshpb_device *oshpb = dev_get_drvdata(dev);
	int error;

	error = oshpb_store_swap(oshpb, &oshpb->swap_xy, buf);
	return error ? error : count;
}

static DEVICE_ATTR_RW(swap_ab);
static DEVICE_ATTR_RW(swap_xy);

static struct attribute *oshpb_attrs[] = {
	&dev_attr_swap_ab.attr,
	&dev_attr_swap_xy.attr,
	NULL,
};

static const struct attribute_group oshpb_attr_group = {
	.attrs = oshpb_attrs,
};

/* ---- probe / remove ---- */

static void oshpb_parse_buttons(struct hid_device *hdev,
				struct oshpb_device *oshpb,
				struct device_node *joypad_np)
{
	struct input_dev *input = oshpb->input;
	struct device_node *child;
	int idx = 0;

	for_each_child_of_node(joypad_np, child) {
		u32 hid_bit, linux_code, swapped;

		if (of_property_read_u32(child, "hid-bit", &hid_bit))
			continue;
		if (of_property_read_u32(child, "linux,code", &linux_code))
			continue;
		if (hid_bit >= OSHPB_MAX_BUTTONS || linux_code > KEY_MAX) {
			hid_warn(hdev, "%pOFn: invalid hid-bit %u / linux,code %u\n",
				 child, hid_bit, linux_code);
			continue;
		}
		if (idx >= OSHPB_MAX_BUTTONS) {
			of_node_put(child);
			break;
		}

		oshpb->buttons[idx].hid_bit = hid_bit;
		oshpb->buttons[idx].linux_code = linux_code;
		input_set_capability(input, EV_KEY, linux_code);
		/* swapped codes too, else the input core drops swapped reports */
		swapped = oshpb_swap_ab_code(linux_code);
		if (swapped != linux_code)
			input_set_capability(input, EV_KEY, swapped);
		swapped = oshpb_swap_xy_code(linux_code);
		if (swapped != linux_code)
			input_set_capability(input, EV_KEY, swapped);

		/* track L3/R3 for stick-switch emulation */
		if (linux_code == BTN_TRIGGER_HAPPY3)
			oshpb->l3_bit = hid_bit;
		else if (linux_code == BTN_TRIGGER_HAPPY4)
			oshpb->r3_bit = hid_bit;

		idx++;
	}
	oshpb->button_count = idx;
}

static void oshpb_read_tuning(struct oshpb_device *oshpb,
			      struct device_node *joypad_np)
{
	u32 val;

	oshpb->tuning_x_p  = OSHPB_TUNING_DEFAULT;
	oshpb->tuning_x_n  = OSHPB_TUNING_DEFAULT;
	oshpb->tuning_y_p  = OSHPB_TUNING_DEFAULT;
	oshpb->tuning_y_n  = OSHPB_TUNING_DEFAULT;
	oshpb->tuning_rx_p = OSHPB_TUNING_DEFAULT;
	oshpb->tuning_rx_n = OSHPB_TUNING_DEFAULT;
	oshpb->tuning_ry_p = OSHPB_TUNING_DEFAULT;
	oshpb->tuning_ry_n = OSHPB_TUNING_DEFAULT;
	oshpb->deadzone    = OSHPB_DEADZONE_DEFAULT;
	oshpb->scale       = 2;

	if (!joypad_np)
		return;

	if (!of_property_read_u32(joypad_np, "abs_x-p-tuning", &val))
		oshpb->tuning_x_p = val;
	if (!of_property_read_u32(joypad_np, "abs_x-n-tuning", &val))
		oshpb->tuning_x_n = val;
	if (!of_property_read_u32(joypad_np, "abs_y-p-tuning", &val))
		oshpb->tuning_y_p = val;
	if (!of_property_read_u32(joypad_np, "abs_y-n-tuning", &val))
		oshpb->tuning_y_n = val;
	if (!of_property_read_u32(joypad_np, "abs_rx-p-tuning", &val))
		oshpb->tuning_rx_p = val;
	if (!of_property_read_u32(joypad_np, "abs_rx-n-tuning", &val))
		oshpb->tuning_rx_n = val;
	if (!of_property_read_u32(joypad_np, "abs_ry-p-tuning", &val))
		oshpb->tuning_ry_p = val;
	if (!of_property_read_u32(joypad_np, "abs_ry-n-tuning", &val))
		oshpb->tuning_ry_n = val;
	if (!of_property_read_u32(joypad_np, "button-adc-deadzone", &val))
		oshpb->deadzone = val;
	if (!of_property_read_u32(joypad_np, "button-adc-scale", &val))
		oshpb->scale = val;
	/* spike filter threshold in output units, absent = 0 = off */
	if (!of_property_read_u32(joypad_np, "button-adc-max-step", &val))
		oshpb->max_step = val;
}

static int oshpb_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct oshpb_device *oshpb;
	struct input_dev *input;
	struct device_node *joypad_np;
	u32 fuzz, flat;
	int error, idx;

	oshpb = devm_kzalloc(&hdev->dev, sizeof(*oshpb), GFP_KERNEL);
	if (!oshpb)
		return -ENOMEM;

	oshpb->hdev = hdev;
	oshpb->fn_bit = -1;
	oshpb->l3_bit = -1;
	oshpb->r3_bit = -1;
	mutex_init(&oshpb->swap_lock);
	INIT_WORK(&oshpb->play_work, oshpb_play_work);
	hid_set_drvdata(hdev, oshpb);

	error = hid_parse(hdev);
	if (error) {
		hid_err(hdev, "parse failed: %d\n", error);
		return error;
	}

	/* no hid-input: this driver creates its own input device */
	error = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_HIDINPUT);
	if (error) {
		hid_err(hdev, "hw start failed: %d\n", error);
		return error;
	}

	error = hid_hw_open(hdev);
	if (error) {
		hid_err(hdev, "hw open failed: %d\n", error);
		goto err_stop;
	}

	error = sysfs_create_group(&hdev->dev.kobj, &oshpb_attr_group);
	if (error) {
		hid_err(hdev, "sysfs group create failed: %d\n", error);
		goto err_close;
	}

	joypad_np = of_find_compatible_node(NULL, NULL, "odroidgo3-hid-joypad");

	/* input device matching the odroidgo3-joypad identity */
	input = devm_input_allocate_device(&hdev->dev);
	if (!input) {
		error = -ENOMEM;
		goto err_put_node;
	}

	oshpb->input = input;
	input->name = OSHPB_NAME;
	input->phys = OSHPB_PHYS;
	input->id.bustype = BUS_HOST;
	input->id.vendor  = OSHPB_VENDOR;
	input->id.product = OSHPB_PRODUCT;
	input->id.version = OSHPB_REVISION;
	input->dev.parent = &hdev->dev;

	fuzz = OSHPB_AXIS_FUZZ;
	flat = OSHPB_AXIS_FLAT;
	if (joypad_np) {
		of_property_read_u32(joypad_np, "button-adc-fuzz", &fuzz);
		of_property_read_u32(joypad_np, "button-adc-flat", &flat);
	}
	input_set_abs_params(input, ABS_X,
			     -OSHPB_AXIS_CENTER * 3, OSHPB_AXIS_CENTER * 3, fuzz, flat);
	input_set_abs_params(input, ABS_Y,
			     -OSHPB_AXIS_CENTER * 3, OSHPB_AXIS_CENTER * 3, fuzz, flat);
	input_set_abs_params(input, ABS_RX,
			     -OSHPB_AXIS_CENTER * 3, OSHPB_AXIS_CENTER * 3, fuzz, flat);
	input_set_abs_params(input, ABS_RY,
			     -OSHPB_AXIS_CENTER * 3, OSHPB_AXIS_CENTER * 3, fuzz, flat);

	input_set_capability(input, EV_KEY, BTN_DPAD_UP);
	input_set_capability(input, EV_KEY, BTN_DPAD_DOWN);
	input_set_capability(input, EV_KEY, BTN_DPAD_LEFT);
	input_set_capability(input, EV_KEY, BTN_DPAD_RIGHT);

	if (joypad_np)
		oshpb_parse_buttons(hdev, oshpb, joypad_np);

	/* stick-switch-key is only honoured together with skip-absr */
	if (joypad_np && of_property_read_bool(joypad_np, "skip-absr")) {
		u32 switch_code;

		if (!of_property_read_u32(joypad_np, "stick-switch-key", &switch_code)) {
			for (idx = 0; idx < oshpb->button_count; idx++) {
				if (oshpb->buttons[idx].linux_code == switch_code) {
					oshpb->fn_bit = oshpb->buttons[idx].hid_bit;
					break;
				}
			}
			if (oshpb->fn_bit < 0)
				hid_warn(hdev, "stick-switch-key code %u not found in button mapping\n",
					 switch_code);
		}
	}

	error = oshpb_rumble_setup(hdev, oshpb, joypad_np);
	if (error) {
		hid_info(hdev, "rumble not available, continuing without\n");
		oshpb->has_rumble = false;
	} else {
		oshpb->has_rumble = true;
		input_set_capability(input, EV_FF, FF_RUMBLE);
		error = input_ff_create_memless(input, oshpb,
						oshpb_rumble_play);
		if (error) {
			hid_err(hdev, "FF create failed: %d\n", error);
			goto err_put_node;
		}
	}

	/* registered before the input device, so it runs after unregister */
	error = devm_add_action_or_reset(&hdev->dev, oshpb_rumble_teardown, oshpb);
	if (error)
		goto err_put_node;

	error = input_register_device(input);
	if (error) {
		hid_err(hdev, "input register failed: %d\n", error);
		goto err_put_node;
	}

	oshpb_read_tuning(oshpb, joypad_np);
	of_node_put(joypad_np);
	smp_store_release(&oshpb->ready, true);

	/* startup rumble: vibrate 1 second on probe */
	if (oshpb->has_rumble) {
		oshpb->level = 0xffff;
		oshpb_pwm_start(oshpb);
		msleep(1000);
		oshpb_pwm_stop(oshpb);
		oshpb->level = 0;
	}

	hid_info(hdev, "OSH PB Controller registered as '%s'\n", OSHPB_NAME);
	return 0;

err_put_node:
	of_node_put(joypad_np);
	sysfs_remove_group(&hdev->dev.kobj, &oshpb_attr_group);
err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return error;
}

static void oshpb_remove(struct hid_device *hdev)
{
	sysfs_remove_group(&hdev->dev.kobj, &oshpb_attr_group);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id oshpb_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_OSHPB, USB_DEVICE_ID_OSHPB) },
	{ }
};
MODULE_DEVICE_TABLE(hid, oshpb_devices);

static struct hid_driver oshpb_driver = {
	.name		= "oshpb-controller",
	.id_table	= oshpb_devices,
	.probe		= oshpb_probe,
	.remove		= oshpb_remove,
	.raw_event	= oshpb_raw_event,
};
module_hid_driver(oshpb_driver);

MODULE_AUTHOR("lcdyk");
MODULE_DESCRIPTION("OpenSimHardware OSH PB Controller with odroidgo3-joypad compatible interface");
MODULE_LICENSE("GPL");
