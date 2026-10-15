#include "tusb.h"
#include <stdio.h>
#include <string.h>
#include <pico/unique_id.h>

/*
  USB MIDI device descriptors for the Nunchuk adapter.

  The VID/PID pair is TinyUSB's test pair, fine for personal use but not for
  a product that is distributed.
 */

#define USB_VID   0xCAFE
#define USB_PID   0x4008
#define USB_BCD   0x0200

// String Descriptor Index
enum {
	STRING_LANGID = 0,
	STRING_MANUFACTURER,
	STRING_PRODUCT,
	STRING_SERIAL,
	STRING_LAST,
};

char const *string_desc_arr[] = {
	(const char[]){ 0x09, 0x04 }, // 0: supported language is English (0x0409)
	"Workshop Computer DIY", // 1: Manufacturer
	"Nunchuk MIDI", // 2: Product
	NULL, // 3: Serial number, from flash chip ID
};

tusb_desc_device_t const desc_device = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = USB_BCD,
	.bDeviceClass = 0x00,
	.bDeviceSubClass = 0x00,
	.bDeviceProtocol = 0x00,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

	.idVendor = USB_VID,
	.idProduct = USB_PID,
	.bcdDevice = 0x0100,

	.iManufacturer = STRING_MANUFACTURER,
	.iProduct = STRING_PRODUCT,
	.iSerialNumber = STRING_SERIAL,

	.bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
	return (uint8_t const *)&desc_device;
}

enum
{
	ITF_NUM_MIDI = 0,
	ITF_NUM_MIDI_STREAMING,
	ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

#define EPNUM_MIDI 0x01

uint8_t const desc_fs_configuration[] = {
	// Config number, interface count, string index, total length, attribute, power in mA
	TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

	// Interface number, string index, EP Out & EP In address, EP size
	TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0, EPNUM_MIDI, 0x80 | EPNUM_MIDI, 64)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	return desc_fs_configuration;
}

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;

	uint8_t chr_count;

	if (index == 0)
	{
		memcpy(&_desc_str[1], string_desc_arr[0], 2);
		chr_count = 1;
	}
	else if (index == STRING_SERIAL)
	{
		char temp[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
		pico_get_unique_board_id_string(temp, sizeof(temp));
		chr_count = strlen(temp);
		for (uint8_t i = 0; i < chr_count; i++)
		{
			_desc_str[1 + i] = temp[i];
		}
	}
	else if (index < STRING_LAST)
	{
		const char *str = string_desc_arr[index];

		chr_count = strlen(str);
		if (chr_count > 31)
		{
			chr_count = 31;
		}
		for (uint8_t i = 0; i < chr_count; i++)
		{
			_desc_str[1 + i] = str[i];
		}
	}
	else
	{
		return NULL;
	}

	// first byte is length (including header), second byte is string type
	_desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

	return _desc_str;
}
