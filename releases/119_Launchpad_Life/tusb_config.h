// =============================================================================
//  tusb_config.h  -  Settings for TinyUSB, the USB library
// =============================================================================
//
//  TinyUSB reads this file to find out what we want it to do. We only need
//  USB HOST mode (the Workshop Computer is in charge, the Launchpad is the
//  device), so all of the device-side features are left out.
//
//  These are "#define"s - old-style C constants that the compiler swaps in
//  before compiling. TinyUSB is written in C, so that is what it expects.
// =============================================================================

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

// Host only, on the RP2040's one USB port.
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_HOST

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE   // no operating system, just our own loop
#endif

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

// Buffer for reading a device's descriptors (its "ID card") when plugged in.
#define CFG_TUH_ENUMERATION_BUFSIZE 1024

// Allow a USB hub between the Computer and the Launchpad.
#define CFG_TUH_HUB        1
#define CFG_TUH_DEVICE_MAX (CFG_TUH_HUB ? 4 : 1)

// USB classes we don't need.
#define CFG_TUH_CDC    0
#define CFG_TUH_HID    0
#define CFG_TUH_MSC    0
#define CFG_TUH_VENDOR 0

// NOTE: do NOT "#define CFG_TUH_MIDI 1". It looks like the switch for MIDI
// host support, but in the TinyUSB that ships with Pico SDK 2.x it breaks
// the build. MIDI host support comes from rppicomidi's driver instead
// (usb_midi_host.c), registered in usb_midi_host_app_driver.c.

// Size of the MIDI receive and transmit buffers, in bytes.
// The transmit buffer is bigger than the default (64) because we send
// LED updates to the Launchpad in SysEx messages of up to ~90 bytes, and
// each 3 MIDI bytes become a 4-byte USB-MIDI packet.
#define CFG_TUH_MIDI_RX_BUFSIZE 256
#define CFG_TUH_MIDI_TX_BUFSIZE 512
#define CFG_TUH_MIDI_EP_BUFSIZE 64

// Keep the device's name strings (handy when debugging).
#define CFG_MIDI_HOST_DEVSTRINGS 1

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
