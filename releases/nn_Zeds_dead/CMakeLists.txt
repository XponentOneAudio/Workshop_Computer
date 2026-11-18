# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(zeds_dead C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
pico_sdk_init()

add_executable(zeds_dead main.cpp usb_descriptors.c)

# Warn about accidental double-precision maths: the RP2040 has no FPU, and
# double is roughly twice as slow again as float. Always write 1.0f, not 1.0.
target_compile_options(zeds_dead PRIVATE -Wdouble-promotion -Wfloat-conversion -Wall -Wextra)

# Give the crystal oscillator more time to start - some boards need it.
target_compile_definitions(zeds_dead PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(zeds_dead PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(zeds_dead pico_stdlib pico_multicore pico_unique_id
    hardware_dma hardware_i2c hardware_pwm hardware_adc hardware_spi hardware_flash
    tinyusb_device tinyusb_board)

# Copy the whole program into RAM at power-up and run it from there. Flash
# can't be read while it is being written, so this is what lets core 1 save
# banks to flash while core 0 carries on making audio. (See main.cpp.)
pico_set_binary_type(zeds_dead copy_to_ram)
target_link_options(zeds_dead PRIVATE -Wl,--print-memory-usage)

pico_enable_stdio_usb(zeds_dead 0)
pico_add_extra_outputs(zeds_dead)   # produces zeds_dead.uf2
