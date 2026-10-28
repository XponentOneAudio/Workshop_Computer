cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(wdf_benjolin C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(WDFB_VERSION "1.0")

# Set -DWDFB_PROFILE=ON to light LED 6 if ProcessSample ever exceeds 75% of
# its time budget
option(WDFB_PROFILE "Build with ProcessSample timing check" OFF)

add_executable(wdf_benjolin main.cpp)
target_include_directories(wdf_benjolin PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(wdf_benjolin PRIVATE -Wdouble-promotion -Wall -Wextra)
target_compile_definitions(wdf_benjolin PRIVATE
    PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
    WDFB_PROFILE=$<BOOL:${WDFB_PROFILE}>)
target_link_libraries(wdf_benjolin pico_unique_id pico_stdlib hardware_dma hardware_i2c hardware_pwm hardware_adc hardware_spi)
pico_set_binary_type(wdf_benjolin copy_to_ram)
pico_enable_stdio_usb(wdf_benjolin 0)
pico_set_program_name(wdf_benjolin "WDF Benjolin")
pico_set_program_version(wdf_benjolin "${WDFB_VERSION}")
pico_add_extra_outputs(wdf_benjolin)

add_custom_command(TARGET wdf_benjolin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE_DIR:wdf_benjolin>/wdf_benjolin.uf2"
        "$<TARGET_FILE_DIR:wdf_benjolin>/wdf_benjolin.${WDFB_VERSION}.uf2"
    VERBATIM)
