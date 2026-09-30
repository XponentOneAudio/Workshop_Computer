cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(what_three_wiggles C CXX ASM)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(what_three_wiggles
    main.cpp
    usb_descriptors.c
)

target_include_directories(what_three_wiggles PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(what_three_wiggles
    pico_stdlib
    pico_unique_id
    pico_multicore
    hardware_adc
    hardware_dma
    hardware_flash
    hardware_i2c
    hardware_pwm
    hardware_spi
    tinyusb_device
    tinyusb_board
)

# Give the oscillator more time to start; some boards won't run without it
target_compile_definitions(what_three_wiggles PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_compile_options(what_three_wiggles PRIVATE
    -Wall
    -Wextra
    -Wdouble-promotion
    -Wfloat-conversion
    -O2
)

target_link_options(what_three_wiggles PRIVATE -Wl,--print-memory-usage)

# Run entirely from RAM: steadier audio timing, and core 0 keeps running
# while core 1 erases flash to save the words.
pico_set_binary_type(what_three_wiggles copy_to_ram)

pico_enable_stdio_usb(what_three_wiggles 0)
pico_enable_stdio_uart(what_three_wiggles 0)

pico_add_extra_outputs(what_three_wiggles)
