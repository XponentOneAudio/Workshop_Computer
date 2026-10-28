cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico CACHE STRING "Board type")
include(pico_sdk_import.cmake)

set(CARD_NAME "krautrock_phaser")
project(${CARD_NAME} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(${CARD_NAME} main.cpp)

# -Wdouble-promotion catches a double sneaking into the float control model,
# where it would cost several times as much on the RP2040's soft float.
target_compile_options(${CARD_NAME} PRIVATE -Wall -Wextra -Wdouble-promotion -O2)

target_compile_definitions(${CARD_NAME} PRIVATE
  # REQUIRED for the Workshop Computer's crystal (cold-boot reliability).
  PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
  # The audio path uses 32x32->64 products for its Q30 coefficients (the
  # bottom of the sweep needs them), so keep __aeabi_lmul out of flash.
  PICO_INT64_OPS_IN_RAM=1
  # One division per sample resolves the delay-free loop; same reasoning.
  PICO_DIVIDER_IN_RAM=1)

target_include_directories(${CARD_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(${CARD_NAME}
  pico_stdlib
  pico_multicore
  hardware_dma
  hardware_i2c
  hardware_pwm
  hardware_adc
  hardware_spi
  hardware_flash)

pico_enable_stdio_usb(${CARD_NAME} 0)
pico_enable_stdio_uart(${CARD_NAME} 0)
pico_add_extra_outputs(${CARD_NAME})
