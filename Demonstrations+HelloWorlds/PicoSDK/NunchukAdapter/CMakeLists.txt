cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)

set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project).
# Set PICO_SDK_PATH in the environment, or pass -DPICO_SDK_PATH=... to cmake.
include(pico_sdk_import.cmake)

project(nunchuk_adapter C CXX ASM)

pico_sdk_init()

add_executable(nunchuk_adapter main.c usb_descriptors.c)

target_compile_options(nunchuk_adapter PRIVATE -Wall -Wextra)

# tusb_config.h lives next to main.c
target_include_directories(nunchuk_adapter PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(nunchuk_adapter
	pico_stdlib
	pico_unique_id
	hardware_i2c
	tinyusb_device
	tinyusb_board
)

# USB is used for MIDI, so no USB serial
pico_enable_stdio_usb(nunchuk_adapter 0)
pico_enable_stdio_uart(nunchuk_adapter 0)

pico_add_extra_outputs(nunchuk_adapter)
