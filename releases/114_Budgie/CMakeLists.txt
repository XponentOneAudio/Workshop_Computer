cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(budgie VERSION 0.1.0 LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(budgie
	main.cpp
)

target_compile_options(budgie PRIVATE
	-Wdouble-promotion
	-Wall
	-Wextra
)

target_compile_definitions(budgie PRIVATE
	PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
)

# cmake -DBUDGIE_PROFILE=ON: CV Out 2 reports the worst-case sample time
option(BUDGIE_PROFILE "Report audio-interrupt cycle counts on CV Out 2" OFF)
if(BUDGIE_PROFILE)
	target_compile_definitions(budgie PRIVATE BUDGIE_PROFILE=1)
endif()

target_include_directories(budgie PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(budgie
	pico_unique_id
	pico_stdlib
	hardware_dma
	hardware_i2c
	hardware_pwm
	hardware_adc
	hardware_spi
)

pico_set_binary_type(budgie copy_to_ram)
pico_set_program_name(budgie "Budgie")
pico_set_program_version(budgie "0.1.0")
pico_enable_stdio_usb(budgie 0)
pico_add_extra_outputs(budgie)
