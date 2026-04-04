# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")
include(pico_sdk_import.cmake)

project(TouchRTTY C CXX ASM)
pico_sdk_init()

# Re-add LovyanGFX
add_subdirectory(lib/LovyanGFX)

add_executable(TouchRTTY
    src/main.cpp
    src/display/ili9488_driver.c
)

pico_generate_pio_header(TouchRTTY ${CMAKE_CURRENT_LIST_DIR}/src/display/ili9488_spi.pio)

target_include_directories(TouchRTTY PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/lib/lgfx_lite
    ${CMAKE_CURRENT_LIST_DIR}/src
)

# Optimization Flags for DSP performance on RP2350 (Cortex-M33)
# NOTE: -flto is incompatible with Pico SDK's __wrap_ symbols (printf, snprintf etc.)
target_compile_options(TouchRTTY PRIVATE
    -O3
    -ffast-math
    -funroll-loops
)

# Set global UART baud rate to 115200
target_compile_definitions(TouchRTTY PRIVATE 
    PICO_STDIO_UART_BAUD_RATE=115200
)

target_link_libraries(TouchRTTY
    pico_stdlib
    hardware_adc
    hardware_dma
    hardware_spi
    hardware_pio
    hardware_vreg
    hardware_clocks
    hardware_timer
    hardware_uart
    hardware_interp
    hardware_flash
    pico_multicore
    pico_float
    LovyanGFX
)

pico_enable_stdio_usb(TouchRTTY 1)
pico_enable_stdio_uart(TouchRTTY 0) # Disable UART stdio to save cycles (use USB)
pico_add_extra_outputs(TouchRTTY)
