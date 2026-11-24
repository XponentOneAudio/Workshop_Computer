// TinyUSB configuration for a Workshop Link card: USB host only, using
// TinyUSB's built-in MIDI host class (TinyUSB 0.18+). Copy beside a card
// that uses WorkshopLinkHost.h.
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_HOST

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

// Room for a composite (USB audio + MIDI) configuration descriptor such as
// the FM1's.
#define CFG_TUH_ENUMERATION_BUFSIZE 1024

// A hub lets a powered hub supply the device (see README.md).
#define CFG_TUH_HUB 1
#define CFG_TUH_DEVICE_MAX (CFG_TUH_HUB ? 4 : 1)

#define CFG_TUH_MIDI 1
// A few complete Workshop Link frames (up to 119 bytes each) in flight.
#define CFG_TUH_MIDI_RX_BUFSIZE 512
#define CFG_TUH_MIDI_TX_BUFSIZE 512

#define CFG_TUH_CDC 0
#define CFG_TUH_HID 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0

#ifdef __cplusplus
}
#endif

#endif
