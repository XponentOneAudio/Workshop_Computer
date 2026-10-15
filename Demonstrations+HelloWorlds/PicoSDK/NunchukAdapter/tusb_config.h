#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// USB MIDI device configuration for the Nunchuk adapter

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_NONE
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE    64

#define CFG_TUD_HID               0
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              1
#define CFG_TUD_VENDOR            0

#define CFG_TUD_MIDI_RX_BUFSIZE   64
#define CFG_TUD_MIDI_TX_BUFSIZE   64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
