cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(nam_amp C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(nam_amp main.cpp)

target_link_libraries(nam_amp
	pico_stdlib
	pico_multicore
	pico_unique_id
	hardware_dma
	hardware_i2c
	hardware_pwm
	hardware_adc
	hardware_spi
	hardware_vreg
)

target_compile_options(nam_amp PRIVATE -O2 -Wall -Wextra -Wdouble-promotion)
target_link_options(nam_amp PRIVATE -Wl,--print-memory-usage)
target_compile_definitions(nam_amp PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
target_include_directories(nam_amp PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Run entirely from RAM: both cores run model code continuously, and flash
# access would slow them down
pico_set_binary_type(nam_amp copy_to_ram)

pico_enable_stdio_usb(nam_amp 0)
pico_enable_stdio_uart(nam_amp 0)
pico_add_extra_outputs(nam_amp)
