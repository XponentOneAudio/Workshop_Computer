cmake_minimum_required (VERSION 3.13)
include(pico_sdk_import.cmake)
project(tonnetz C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(tonnetz main.cpp)
target_compile_options(tonnetz PRIVATE -Wdouble-promotion -Wfloat-conversion -Wall -Wextra -O2)
target_link_options(tonnetz PRIVATE -Wl,--print-memory-usage)
# Required for the Workshop Computer's crystal to start on a cold power-up
target_compile_definitions(tonnetz PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
target_include_directories(tonnetz PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(tonnetz pico_unique_id pico_stdlib hardware_dma hardware_i2c hardware_pwm hardware_adc hardware_spi)

# EightMU.h carries the usb_midi_host driver and the host-mode TinyUSB config,
# used for the Wii Nunchuk adapter (which appears as an 8mu)
target_compile_definitions(tonnetz PRIVATE CFG_TUSB_CONFIG_FILE="EightMU.h")
target_link_libraries(tonnetz pico_multicore tinyusb_host tinyusb_board)

pico_set_binary_type(tonnetz copy_to_ram)
pico_enable_stdio_usb(tonnetz 0)
pico_enable_stdio_uart(tonnetz 0)
pico_add_extra_outputs(tonnetz)
