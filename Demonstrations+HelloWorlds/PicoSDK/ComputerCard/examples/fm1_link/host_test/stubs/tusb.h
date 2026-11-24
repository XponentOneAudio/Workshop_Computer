#pragma once
#include <stdint.h>
typedef struct { uint8_t daddr, bInterfaceNumber, rx_cable_count, tx_cable_count; } tuh_midi_mount_cb_t;
static inline void tusb_init(void) {}
static inline void tuh_task(void) {}
bool tuh_midi_mounted(uint8_t idx);
uint32_t tuh_midi_stream_write(uint8_t idx, uint8_t cable, const uint8_t *p, uint32_t n);
uint32_t tuh_midi_write_flush(uint8_t idx);
uint32_t tuh_midi_stream_read(uint8_t idx, uint8_t *cable, uint8_t *p, uint16_t n);
