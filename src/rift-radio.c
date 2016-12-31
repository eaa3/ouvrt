/*
 * Oculus Rift CV1 Radio
 * Copyright 2016 Philipp Zabel
 * SPDX-License-Identifier:	LGPL-2.0+ or BSL-1.0
 */
#include <asm/byteorder.h>
#include <errno.h>
#include <glib.h>
#include <stdint.h>

#include "rift-hid-reports.h"
#include "rift-radio.h"
#include "hidraw.h"

static void rift_dump_message(const unsigned char *buf, size_t len)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		g_print(" %02x", buf[i]);
	g_print("\n");
}

static int rift_radio_transfer(int fd, uint8_t a, uint8_t b, uint8_t c)
{
	struct rift_radio_control_report report = {
		.id = RIFT_RADIO_CONTROL_REPORT_ID,
		.unknown = { a, b, c },
	};
	int ret;

	ret = hid_send_feature_report(fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	do {
		ret = hid_get_feature_report(fd, &report, sizeof(report));
		if (ret < 0)
			return ret;
	} while (report.unknown[0] & 0x80);

	if (report.unknown[0] & 0x08)
		return -EIO;

	return 0;
}

static int rift_radio_read(int fd, uint8_t a, uint8_t b, uint8_t c,
			   struct rift_radio_data_report *report)
{
	int ret;

	if (report->id != RIFT_RADIO_DATA_REPORT_ID)
		return -EINVAL;

	ret = rift_radio_transfer(fd, a, b, c);
	if (ret < 0)
		return ret;

	return hid_get_feature_report(fd, report, sizeof(*report));
}

int rift_get_firmware_version(int fd)
{
	struct rift_radio_data_report report = {
		.id = RIFT_RADIO_DATA_REPORT_ID,
	};
	int ret;
	int i;

	ret = rift_radio_read(fd, 0x05, RIFT_RADIO_FIRMWARE_VERSION_CONTROL,
			      0x05, &report);
	if (ret < 0)
		return ret;

	g_print("Rift: Firmware version ");
	for (i = 14; i < 24 && g_ascii_isalnum(report.payload[i]); i++)
		g_print("%c", report.payload[i]);
	g_print("\n");

	return 0;
}

static int rift_radio_get_serial(int fd, int device_type, char *serial)
{
	struct rift_radio_data_report report = {
		.id = RIFT_RADIO_DATA_REPORT_ID,
	};
	int ret;
	int i;

	ret = rift_radio_read(fd, 0x03, RIFT_RADIO_SERIAL_NUMBER_CONTROL,
			      device_type, &report);
	if (ret < 0)
		return ret;

	for (i = 0; i < 14 && g_ascii_isalnum(report.serial.number[i]); i++)
		serial[i] = report.serial.number[i];

	return 0;
}

static int rift_radio_get_firmware_version(int fd, int device_type,
					   char *firmware_date,
					   char *firmware_version)
{
	struct rift_radio_data_report report = {
		.id = RIFT_RADIO_DATA_REPORT_ID,
	};
	int ret;
	int i;

	ret = rift_radio_read(fd, 0x03, RIFT_RADIO_FIRMWARE_VERSION_CONTROL,
			      device_type, &report);
	if (ret < 0)
		return ret;

	for (i = 0; i < 11 && g_ascii_isprint(report.firmware.date[i]); i++)
		firmware_date[i] = report.firmware.date[i];

	for (i = 0; i < 10 && g_ascii_isalnum(report.firmware.version[i]); i++)
		firmware_version[i] = report.firmware.version[i];

	return 0;
}

static void rift_decode_remote_message(struct rift_remote *remote,
				       const struct rift_radio_message *message)
{
	int16_t buttons = __le16_to_cpu(message->remote.buttons);

	if (remote->buttons != buttons)
		remote->buttons = buttons;
}

static void rift_decode_touch_message(struct rift_touch_controller *touch,
				      const struct rift_radio_message *message)
{
	int16_t accel[3] = {
		__le16_to_cpu(message->touch.accel[0]),
		__le16_to_cpu(message->touch.accel[1]),
		__le16_to_cpu(message->touch.accel[2]),
	};
	int16_t gyro[3] = {
		__le16_to_cpu(message->touch.gyro[0]),
		__le16_to_cpu(message->touch.gyro[1]),
		__le16_to_cpu(message->touch.gyro[2]),
	};
	const uint8_t *tgs = message->touch.trigger_grip_stick;
	uint16_t trigger = tgs[0] | ((tgs[1] & 0x03) << 8);
	uint16_t grip = ((tgs[1] & 0xfc) >> 2) | ((tgs[2] & 0x0f) << 6);
	uint16_t stick[2] = {
		((tgs[2] & 0xf0) >> 4) | ((tgs[3] & 0x3f) << 4),
		((tgs[3] & 0xc0) >> 6) | ((tgs[4] & 0xff) << 2),
	};
	uint16_t adc_value = __le16_to_cpu(message->touch.adc_value);

	switch (message->touch.adc_channel) {
	case RIFT_TOUCH_CONTROLLER_ADC_A_X:
		touch->cap_a_x = adc_value;
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_B_Y:
		touch->cap_b_y = adc_value;
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_REST:
		touch->cap_rest = adc_value;
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_STICK:
		touch->cap_stick = adc_value;
		break;
	case RIFT_TOUCH_CONTROLLER_ADC_TRIGGER:
		touch->cap_trigger = adc_value;
		break;
	}

	(void)accel;
	(void)gyro;
	(void)trigger;
	(void)grip;
	(void)stick;
}

static int rift_radio_activate(struct rift_wireless_device *dev, int fd)
{
	int ret;

	ret = rift_radio_get_serial(fd, dev->id, dev->serial);
	if (ret < 0) {
		g_print("Rift: Failed to read %s serial number\n", dev->name);
		return ret;
	}

	g_print("Rift: %s: Serial %.14s\n", dev->name, dev->serial);

	ret = rift_radio_get_firmware_version(fd, dev->id, dev->firmware_date,
					      dev->firmware_version);
	if (ret < 0) {
		g_print("Rift: Failed to read firmware version\n");
		return ret;
	}

	g_print("Rift: %s: Firmware version %.10s\n", dev->name,
		dev->firmware_version);

	dev->active = true;

	return 0;
}

void rift_decode_radio_message(struct rift_radio *radio, int fd,
			       const unsigned char *buf, size_t len)
{
	const struct rift_radio_message *message = (const void *)buf;

	if (message->id == RIFT_RADIO_MESSAGE_ID) {
		if (message->device_type == RIFT_REMOTE) {
			rift_decode_remote_message(&radio->remote, message);
		} else if (message->device_type == RIFT_TOUCH_CONTROLLER_LEFT) {
			if (!radio->touch[0].base.active)
				rift_radio_activate(&radio->touch[0].base, fd);
			rift_decode_touch_message(&radio->touch[0], message);
		} else if (message->device_type == RIFT_TOUCH_CONTROLLER_RIGHT) {
			if (!radio->touch[1].base.active)
				rift_radio_activate(&radio->touch[1].base, fd);
			rift_decode_touch_message(&radio->touch[1], message);
		} else {
			g_print("%s: unknown device %02x:", radio->name,
				message->device_type);
			rift_dump_message(buf, len);
		}
	} else {
		unsigned int i;

		for (i = 1; i < len && !buf[i]; i++);
		if (i != len) {
			g_print("%s: unknown message:", radio->name);
			rift_dump_message(buf, len);
			return;
		}
	}
}

void rift_radio_init(struct rift_radio *radio)
{
	radio->remote.base.name = "Remote";
	radio->remote.base.id = RIFT_REMOTE;
	radio->touch[0].base.name = "Left Touch Controller";
	radio->touch[0].base.id = RIFT_TOUCH_CONTROLLER_LEFT;
	radio->touch[1].base.name = "Right Touch Controller";
	radio->touch[1].base.id = RIFT_TOUCH_CONTROLLER_RIGHT;
}