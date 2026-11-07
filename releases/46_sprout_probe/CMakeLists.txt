cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(sprout_probe C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(sprout_probe main.cpp)

# Warn about accidental double-precision maths (slow on the RP2040)
target_compile_options(sprout_probe PRIVATE -Wdouble-promotion -Wfloat-conversion -Wall -Wextra)
target_link_options(sprout_probe PRIVATE -Wl,--print-memory-usage)

# Give the oscillator more time to start - some boards won't run without this
target_compile_definitions(sprout_probe PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(sprout_probe PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sprout_probe
	pico_unique_id pico_stdlib pico_multicore
	hardware_dma hardware_i2c hardware_pwm hardware_adc hardware_spi)

pico_enable_stdio_usb(sprout_probe 0)
pico_enable_stdio_uart(sprout_probe 0)
pico_add_extra_outputs(sprout_probe)
