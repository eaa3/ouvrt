/*
 * Oculus Rift HMDs
 * Copyright 2015-2016 Philipp Zabel
 * SPDX-License-Identifier:	LGPL-2.0+ or BSL-1.0
 */
#include <asm/byteorder.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <math.h>

#include "rift.h"
#include "rift-hid-reports.h"
#include "debug.h"
#include "device.h"
#include "hidraw.h"
#include "imu.h"
#include "math.h"
#include "leds.h"
#include "tracker.h"

/* temporary global */
gboolean rift_flicker = false;

struct _OuvrtRiftPrivate {
	int report_rate;
	int report_interval;
	gboolean flicker;
	uint32_t last_sample_timestamp;
};

G_DEFINE_TYPE_WITH_PRIVATE(OuvrtRift, ouvrt_rift, OUVRT_TYPE_DEVICE)

/*
 * Returns the current sensor configuration.
 */
static int rift_get_config(OuvrtRift *rift)
{
	struct rift_config_report report = {
		.id = RIFT_CONFIG_REPORT_ID,
	};
	uint16_t sample_rate;
	uint16_t report_rate;
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	sample_rate = __le16_to_cpu(report.sample_rate);
	report_rate = sample_rate / (report.packet_interval + 1);

	g_print("Rift: Got sample rate %d Hz, report rate %d Hz, flags: 0x%x\n",
		sample_rate, report_rate, report.flags);

	rift->priv->report_rate = report_rate;
	rift->priv->report_interval = 1000000 / report_rate;

	return 0;
}

/*
 * Configures the sensor report rate
 */
static int rift_set_report_rate(OuvrtRift *rift, int report_rate)
{
	struct rift_config_report report = {
		.id = RIFT_CONFIG_REPORT_ID,
	};
	uint16_t sample_rate;
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	sample_rate = __le16_to_cpu(report.sample_rate);

	if (report_rate > sample_rate)
		report_rate = sample_rate;
	if (report_rate < 5)
		report_rate = 5;

	report.packet_interval = sample_rate / report_rate - 1;

	g_print("Rift: Set sample rate %d Hz, report rate %d Hz\n",
		sample_rate, report_rate);

	ret = hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	rift->priv->report_rate = report_rate;
	rift->priv->report_interval = 1000000 / report_rate;

	return 0;
}

/*
 * Obtains the factory calibrated position data of IR LEDs and IMU
 * from the Rift. Values are stored with µm accuracy in the Rift's
 * local reference frame: the positive x axis points left, the y
 * axis points upward, and z forward:
 *
 *      up
 *       y z forward
 * left  |/
 *    x--+
 */
static int rift_get_positions(OuvrtRift *rift)
{
	struct rift_position_report report = {
		.id = RIFT_POSITION_REPORT_ID,
	};
	int fd = rift->dev.fd;
	uint8_t type;
	vec3 pos, dir;
	uint16_t index;
	uint16_t num;
	int ret;
	int i;

	ret = hid_get_feature_report(fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	num = __le16_to_cpu(report.num);
	if (num > MAX_POSITIONS)
		return -1;

	for (i = 0; ; i++) {
		index = __le16_to_cpu(report.index);
		if (index >= num)
			return -1;

		type = __le16_to_cpu(report.type);

		/* Position in µm */
		pos.x = 1e-6f * (int32_t)__le32_to_cpu(report.pos[0]);
		pos.y = 1e-6f * (int32_t)__le32_to_cpu(report.pos[1]);
		pos.z = 1e-6f * (int32_t)__le32_to_cpu(report.pos[2]);

		if (type == 0) {
			rift->leds.positions[index] = pos;

			/* Direction, magnitude in unknown units */
			dir.x = 1e-6f * (int16_t)__le16_to_cpu(report.dir[0]);
			dir.y = 1e-6f * (int16_t)__le16_to_cpu(report.dir[1]);
			dir.z = 1e-6f * (int16_t)__le16_to_cpu(report.dir[2]);
			rift->leds.directions[index] = dir;
		} else if (type == 1) {
			rift->imu.position = pos;
		}

		/* Break out before reading the first report again */
		if (i + 1 == num)
			break;

		ret = hid_get_feature_report(fd, &report, sizeof(report));
		if (ret < 0)
			return ret;
	}

	rift->leds.num = num - 1;

	return 0;
}

/*
 * Obtains the blinking patterns of the IR LEDs from the Rift.
 */
static int rift_get_led_patterns(OuvrtRift *rift)
{
	struct rift_led_pattern_report report = {
		.id = RIFT_LED_PATTERN_REPORT_ID,
	};
	int fd = rift->dev.fd;
	uint8_t pattern_length;
	uint32_t pattern;
	uint16_t index;
	uint16_t num;
	int ret;
	int i;

	ret = hid_get_feature_report(fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	num = __le16_to_cpu(report.num);
	if (num > MAX_LEDS)
		return -1;

	for (i = 0; ; i++) {
		index = __le16_to_cpu(report.index);
		if (index >= num)
			return -1;

		pattern_length = report.pattern_length;
		pattern = __le32_to_cpu(report.pattern);

		/* pattern_length should be 10 */
		if (pattern_length != 10) {
			g_print("Rift: Unexpected pattern length: %d\n",
				pattern_length);
			return -1;
		}

		/*
		 * pattern should consist of 10 2-bit values that are either
		 * 1 (dark) or 3 (bright).
		 */
		if ((pattern & ~0xaaaaa) != 0x55555) {
			g_print("Rift: Unexpected pattern: 0x%x\n",
				pattern);
			return -1;
		}

		/* Convert into 10 single-bit values 1 -> 0, 3 -> 1 */
		pattern &= 0xaaaaa;
		pattern |= pattern >> 1;
		pattern &= 0x66666;
		pattern |= pattern >> 2;
		pattern &= 0xe1e1e;
		pattern |= pattern >> 4;
		pattern &= 0xe01fe;
		pattern |= pattern >> 8;
		pattern = (pattern >> 1) & 0x3ff;

		rift->leds.patterns[index] = pattern;

		/* Break out before reading the first report again */
		if (i + 1 == num)
			break;

		ret = hid_get_feature_report(fd, &report, sizeof(report));
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * Sends a keepalive report to keep the device active for 10 seconds.
 */
static int rift_send_keepalive(OuvrtRift *rift)
{
	const struct rift_keepalive_report report = {
		.id = RIFT_KEEPALIVE_REPORT_ID,
		.type = RIFT_KEEPALIVE_TYPE,
		.timeout_ms = __cpu_to_le16(RIFT_KEEPALIVE_TIMEOUT_MS),
	};

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Sends a tracking report to enable the IR tracking LEDs.
 */
static int rift_send_tracking(OuvrtRift *rift, bool blink)
{
	struct rift_tracking_report report = {
		.id = RIFT_TRACKING_REPORT_ID,
		.exposure_us = __cpu_to_le16(RIFT_TRACKING_EXPOSURE_US),
		.period_us = __cpu_to_le16(RIFT_TRACKING_PERIOD_US),
		.vsync_offset = __cpu_to_le16(RIFT_TRACKING_VSYNC_OFFSET),
		.duty_cycle = RIFT_TRACKING_DUTY_CYCLE,
	};

	if (blink) {
		report.pattern = 0;
		report.flags = RIFT_TRACKING_ENABLE |
			       RIFT_TRACKING_USE_CARRIER |
			       RIFT_TRACKING_AUTO_INCREMENT;
	} else {
		report.pattern = 0xff;
		report.flags = RIFT_TRACKING_ENABLE |
			       RIFT_TRACKING_USE_CARRIER;
	}

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Sends a display report to set up low persistence and pixel readback
 * for latency measurement.
 */
static int rift_send_display(OuvrtRift *rift, bool low_persistence,
			     bool pixel_readback)
{
	struct rift_display_report report = {
		.id = RIFT_DISPLAY_REPORT_ID,
	};
	uint16_t persistence;
	uint16_t total_rows;
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	persistence = __le16_to_cpu(report.persistence);
	total_rows = __le16_to_cpu(report.total_rows);

	if (low_persistence) {
		report.brightness = 255;
		persistence = total_rows * 18 / 100;
	} else {
		report.brightness = 0;
		persistence = total_rows;
	}
	if (pixel_readback)
		report.flags2 |= RIFT_DISPLAY_READ_PIXEL;
	else
		report.flags2 &= ~RIFT_DISPLAY_READ_PIXEL;
	report.flags2 &= ~RIFT_DISPLAY_DIRECT_PENTILE;

	report.persistence = __cpu_to_le16(persistence);

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Powers up components of the Rift CV1.
 */
static int rift_cv1_power_up(OuvrtRift *rift, uint8_t components)
{
	struct rift_cv1_power_report report = {
		.id = RIFT_CV1_POWER_REPORT_ID,
	};
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	report.components |= components;

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Powers down components of the Rift CV1.
 */
static int rift_cv1_power_down(OuvrtRift *rift, uint8_t components)
{
	struct rift_cv1_power_report report = {
		.id = RIFT_CV1_POWER_REPORT_ID,
	};
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	report.components &= ~components;

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Unpacks three signed 21-bit values packed into a big-endian 64-bit value
 * and stores them in a floating point vector after multiplying by 10⁻⁴.
 */
static void unpack_3x21bit(__be64 *buf, vec3 *v)
{
	uint64_t xyz = __be64_to_cpup(buf);

	v->x = 0.0001f * ((int64_t)xyz >> 43);
	v->y = 0.0001f * ((int64_t)(xyz << 21) >> 43);
	v->z = 0.0001f * ((int64_t)(xyz << 42) >> 43);
}

/*
 * Decodes the periodic sensor message containing IMU sample(s) and
 * frame timing data.
 * Without calibration, the accelerometer reports acceleration in units
 * of 10⁻⁴ m/s² in the accelerometer reference frame: the positive x
 * axis points forward, the y axis points right, and z down.
 * The gyroscope reports angular velocity in units of 10⁻⁴ rad/s around
 * those axes. With onboard calibration enabled, the Rift's local frame
 * of reference is used instead:
 *
 *      x forward       up
 *     /                 y z forward
 *    +--y right   left  |/
 *    |               x--+
 *    z down
 */
static void rift_decode_sensor_message(OuvrtRift *rift,
				       const unsigned char *buf,
				       size_t len)
{
	struct rift_sensor_message *message = (void *)buf;
	uint8_t num_samples;
	uint16_t sample_count;
	int16_t temperature;
	uint32_t sample_timestamp;
	int16_t mag[3];
	uint16_t frame_count;
	uint32_t frame_timestamp;
	uint8_t frame_id;
	uint8_t led_pattern_phase;
	uint16_t exposure_count;
	uint32_t exposure_timestamp;

	struct imu_state state;
	int32_t dt;
	int i;

	if (len < sizeof(*message))
		return;

	num_samples = message->num_samples;
	sample_count = __le16_to_cpu(message->sample_count);
	/* 10⁻²°C */
	temperature = __le16_to_cpu(message->temperature);
	state.sample.temperature = 0.01f * temperature;

	sample_timestamp = __le32_to_cpu(message->timestamp);
	/* µs, wraps every ~72 min */
	state.sample.time = 1e-6 * sample_timestamp;

	dt = sample_timestamp - rift->priv->last_sample_timestamp;
	rift->priv->last_sample_timestamp = sample_timestamp;
	if ((dt < rift->priv->report_interval - 1) ||
	    (dt > rift->priv->report_interval + 1) ||
	    (1000 * num_samples != rift->priv->report_interval)) {
		g_print("Rift: got %d samples after %d µs\n", num_samples,
			dt);
	}

	mag[0] = __le16_to_cpu(message->mag[0]);
	mag[1] = __le16_to_cpu(message->mag[1]);
	mag[2] = __le16_to_cpu(message->mag[2]);
	state.sample.magnetic_field.x = 0.0001f * mag[0];
	state.sample.magnetic_field.y = 0.0001f * mag[1];
	state.sample.magnetic_field.z = 0.0001f * mag[2];

	frame_count = __le16_to_cpu(message->frame_count);
	frame_timestamp = __le32_to_cpu(message->frame_timestamp);
	frame_id = message->frame_id;
	led_pattern_phase = message->led_pattern_phase;
	exposure_count = __le16_to_cpu(message->exposure_count);
	exposure_timestamp = __le32_to_cpu(message->exposure_timestamp);

	num_samples = num_samples > 1 ? 2 : 1;
	for (i = 0; i < num_samples; i++) {
		/* 10⁻⁴ m/s² */
		unpack_3x21bit(&message->sample[i].accel,
			       &state.sample.acceleration);
		/* 10⁻⁴ rad/s */
		unpack_3x21bit(&message->sample[i].gyro,
			       &state.sample.angular_velocity);

		debug_imu_fifo_in(&state, 1);
	}

	(void)exposure_timestamp;
	(void)exposure_count;
	(void)led_pattern_phase;
	(void)frame_id;
	(void)frame_timestamp;
	(void)frame_count;
	(void)sample_count;
}

/*
 * Enables the IR tracking LEDs and registers them with the tracker.
 */
static int rift_start(OuvrtDevice *dev)
{
	OuvrtRift *rift = OUVRT_RIFT(dev);
	int fd = rift->dev.fd;
	int ret;

	if (fd == -1) {
		fd = open(rift->dev.devnode, O_RDWR);
		if (fd == -1) {
			g_print("Rift: Failed to open '%s': %d\n",
				rift->dev.devnode, errno);
			return -1;
		}
		rift->dev.fd = fd;
	}

	ret = rift_get_positions(rift);
	if (ret < 0) {
		g_print("Rift: Error reading factory calibrated positions\n");
		return ret;
	}

	ret = rift_get_led_patterns(rift);
	if (ret < 0) {
		g_print("Rift: Error reading IR LED blinking patterns\n");
		return ret;
	}
	if ((rift->type == RIFT_DK2 && rift->leds.num != 40) ||
	    (rift->type == RIFT_CV1 && rift->leds.num != 44))
		g_print("Rift: Reported %d IR LEDs\n", rift->leds.num);

	ret = rift_get_config(rift);
	if (ret < 0)
		return ret;

	ret = rift_set_report_rate(rift, 500);
	if (ret < 0)
		return ret;

	ret = rift_send_tracking(rift, TRUE);
	if (ret < 0)
		return ret;

	ret = rift_send_display(rift, TRUE, TRUE);
	if (ret < 0)
		return ret;

	ouvrt_tracker_register_leds(rift->tracker, &rift->leds);

	return 0;
}

/*
 * Keeps the Rift active.
 */
static void rift_thread(OuvrtDevice *dev)
{
	OuvrtRift *rift = OUVRT_RIFT(dev);
	unsigned char buf[64];
	struct pollfd fds;
	int count;
	int ret;

	g_print("Rift: Sending keepalive\n");
	rift_send_keepalive(rift);
	count = 0;

	while (dev->active) {
		fds.fd = dev->fd;
		fds.events = POLLIN;
		fds.revents = 0;

		ret = poll(&fds, 1, 1000);
		if (ret == -1 || ret == 0 ||
		    count > 9 * rift->priv->report_rate) {
			if (ret == -1 || ret == 0)
				g_print("Rift: Resending keepalive\n");
			rift_send_keepalive(rift);
			count = 0;
			continue;
		}

		if (fds.events & (POLLERR | POLLHUP | POLLNVAL))
			break;

		ret = read(dev->fd, buf, sizeof(buf));
		if (ret == -1) {
			g_print("%s: Read error: %d\n", dev->name, errno);
			continue;
		}
		if (ret < 64) {
			g_print("%s: Error, invalid %d-byte report 0x%02x\n",
				dev->name, ret, buf[0]);
			continue;
		}

		rift_decode_sensor_message(rift, buf, sizeof(buf));
		count++;
	}
}

/*
 * Disables the IR tracking LEDs and unregisters model from the
 * tracker.
 */
static void rift_stop(OuvrtDevice *dev)
{
	OuvrtRift *rift = OUVRT_RIFT(dev);
	struct rift_tracking_report report = {
		.id = RIFT_TRACKING_REPORT_ID,
	};
	int fd = rift->dev.fd;

	ouvrt_tracker_unregister_leds(rift->tracker, &rift->leds);
	g_object_unref(rift->tracker);
	rift->tracker = NULL;

	hid_get_feature_report(fd, &report, sizeof(report));
	report.flags &= ~RIFT_TRACKING_ENABLE;
	hid_send_feature_report(fd, &report, sizeof(report));

	rift_set_report_rate(rift, 50);
}

/*
 * Frees the device structure and its contents.
 */
static void ouvrt_rift_finalize(GObject *object)
{
	OuvrtRift *rift = OUVRT_RIFT(object);

	g_object_unref(rift->tracker);
	G_OBJECT_CLASS(ouvrt_rift_parent_class)->finalize(object);
}

static void ouvrt_rift_class_init(OuvrtRiftClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ouvrt_rift_finalize;
	OUVRT_DEVICE_CLASS(klass)->start = rift_start;
	OUVRT_DEVICE_CLASS(klass)->thread = rift_thread;
	OUVRT_DEVICE_CLASS(klass)->stop = rift_stop;
}

static void ouvrt_rift_init(OuvrtRift *self)
{
	self->dev.type = DEVICE_TYPE_HMD;
	self->priv = ouvrt_rift_get_instance_private(self);
	self->priv->flicker = false;
	self->priv->last_sample_timestamp = 0;
}

/*
 * Allocates and initializes the device structure and opens the HID device
 * file descriptor.
 *
 * Returns the newly allocated Rift device.
 */
OuvrtDevice *rift_new(const char *devnode, enum rift_type type)
{
	OuvrtRift *rift;

	rift = g_object_new(OUVRT_TYPE_RIFT, NULL);
	if (rift == NULL)
		return NULL;

	rift->dev.devnode = g_strdup(devnode);
	rift->tracker = ouvrt_tracker_new();
	rift->type = type;

	return &rift->dev;
}

OuvrtDevice *rift_dk2_new(const char *devnode)
{
	return rift_new(devnode, RIFT_DK2);
}

OuvrtDevice *rift_cv1_new(const char *devnode)
{
	return rift_new(devnode, RIFT_CV1);
}

void ouvrt_rift_set_flicker(OuvrtRift *rift, gboolean flicker)
{
	if (rift->priv->flicker == flicker)
		return;

	rift->priv->flicker = flicker;
	rift_flicker = flicker;

	if (rift->dev.active)
		rift_send_tracking(rift, flicker);
}
