#pragma once
#include <cstdint>
inline void multicore_launch_core1(void (*)()) {}
inline void __dmb() {}
inline void sleep_us(uint32_t) {}
inline void busy_wait_us_32(uint32_t) {}
