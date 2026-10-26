cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

set(CARD_NAME "weierstrass")
project(${CARD_NAME} C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(${CARD_NAME} main.cpp)

# The RP2040 has no FPU: these catch a float or double sneaking into the audio path.
target_compile_options(${CARD_NAME} PRIVATE -Wdouble-promotion -Wfloat-conversion -Wall -Wextra -O2)
target_link_options(${CARD_NAME} PRIVATE -Wl,--print-memory-usage)

# Give the crystal oscillator more time to start; some boards won't boot without it.
target_compile_definitions(${CARD_NAME} PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(${CARD_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(${CARD_NAME} pico_unique_id pico_stdlib hardware_dma hardware_i2c hardware_pwm hardware_adc hardware_spi)

pico_add_extra_outputs(${CARD_NAME})
pico_enable_stdio_usb(${CARD_NAME} 0)
