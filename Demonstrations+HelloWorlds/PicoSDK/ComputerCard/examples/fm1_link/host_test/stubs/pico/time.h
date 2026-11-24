#pragma once
#include <stdint.h>
extern uint32_t fake_us;
static inline uint32_t time_us_32(void) { return fake_us; }
