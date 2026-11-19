# =============================================================================
#  CMakeLists.txt - build instructions for Launchpad Life
# =============================================================================
#  CMake reads this file and generates the commands that compile the code.
#  To build (with the Pico SDK installed and PICO_SDK_PATH set):
#
#     mkdir build
#     cd build
#     cmake ..
#     make
#
#  The result, build/launchpad_life.uf2, is the file you copy to the card.
# =============================================================================

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico CACHE STRING "Board type")
include(pico_sdk_import.cmake)

set(CARD_NAME "launchpad_life")
project(${CARD_NAME} C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(${CARD_NAME}
  main.cpp
  usb_core1.cpp
  ui.cpp
  launchpad.cpp
  life.cpp
  usb_midi_host.c
  usb_midi_host_app_driver.c
)

target_include_directories(${CARD_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Warn about common mistakes.
target_compile_options(${CARD_NAME} PRIVATE -Wall -Wextra -O2)

# Needed by the Workshop Computer's crystal oscillator: without it the card
# may fail to start from a cold power-on.
target_compile_definitions(${CARD_NAME} PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_link_libraries(${CARD_NAME}
  pico_stdlib
  pico_multicore
  hardware_dma
  hardware_i2c
  hardware_pwm
  hardware_adc
  hardware_spi
  hardware_sync
  tinyusb_host
  tinyusb_board
)

# Run the whole program from RAM rather than flash, so that reading code from
# flash can never cause timing hiccups in the 48 kHz sample loop.
pico_set_binary_type(${CARD_NAME} copy_to_ram)

# The USB socket is busy being a USB host, so no USB serial console.
pico_enable_stdio_usb(${CARD_NAME} 0)
pico_enable_stdio_uart(${CARD_NAME} 0)

pico_add_extra_outputs(${CARD_NAME})
