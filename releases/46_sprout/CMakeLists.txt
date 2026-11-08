cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(sprout C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(sprout main.cpp usb_descriptors.c)

# Warn about accidental double-precision maths (slow on the RP2040)
target_compile_options(sprout PRIVATE -Wdouble-promotion -Wfloat-conversion -Wall -Wextra)
target_link_options(sprout PRIVATE -Wl,--print-memory-usage)

# Give the oscillator more time to start - some boards won't run without this
target_compile_definitions(sprout PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(sprout PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sprout
	pico_unique_id pico_stdlib pico_multicore
	hardware_dma hardware_i2c hardware_pwm hardware_adc hardware_spi hardware_flash
	tinyusb_device tinyusb_board)

# Run entirely from RAM: steady audio timing, and lets core 1 write the
# settings to flash while core 0 keeps playing
pico_set_binary_type(sprout copy_to_ram)

pico_enable_stdio_usb(sprout 0)
pico_enable_stdio_uart(sprout 0)
pico_add_extra_outputs(sprout)
