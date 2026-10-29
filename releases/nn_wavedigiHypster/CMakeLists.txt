cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(wavedigiHypster C CXX ASM)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(HYPSTER_VERSION "0.1")

add_executable(${PROJECT_NAME}
    src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_compile_options(${PROJECT_NAME} PRIVATE -O2 -Wall -Wextra -Wdouble-promotion)

target_link_options(${PROJECT_NAME} PRIVATE -Wl,--print-memory-usage)

# Run from RAM: the audio interrupt never waits on flash.
pico_set_binary_type(${PROJECT_NAME} copy_to_ram)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
)

target_link_libraries(${PROJECT_NAME}
    pico_unique_id
    pico_stdlib
    hardware_dma
    hardware_i2c
    hardware_pwm
    hardware_adc
    hardware_spi
    hardware_clocks
    hardware_vreg
    pico_multicore
)

pico_enable_stdio_usb(${PROJECT_NAME} 0)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

pico_set_program_name(${PROJECT_NAME} "wavedigiHypster")
pico_set_program_version(${PROJECT_NAME} "${HYPSTER_VERSION}")

pico_add_extra_outputs(${PROJECT_NAME})

option(HYPSTER_BENCH "Build for the cycle-counting emulator (tools/bench)" OFF)
if (HYPSTER_BENCH)
    target_sources(${PROJECT_NAME} PRIVATE tools/bench/offsets.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HYPSTER_BENCH=1)
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--undefined=bench_offsets -Wl,--undefined=bench_init -Wl,--undefined=bench_tables)
endif()

# Emit a single, version-stamped UF2 (wavedigiHypster.0.1.uf2)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rename
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.uf2"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.${HYPSTER_VERSION}.uf2"
    VERBATIM)
