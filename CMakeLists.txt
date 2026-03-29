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

set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()

set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

set(PICO_BOARD pico2 CACHE STRING "Board type")
include(pico_sdk_import.cmake)

project(rtty_decoder C CXX ASM)
pico_sdk_init()

# Re-add LovyanGFX
add_subdirectory(lib/LovyanGFX)

add_executable(rtty_decoder
    main.cpp
)

target_include_directories(rtty_decoder PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/lib/lgfx_lite
)

# Set global UART baud rate to 9600
target_compile_definitions(rtty_decoder PRIVATE 
    PICO_STDIO_UART_BAUD_RATE=9600
)

target_link_libraries(rtty_decoder
    pico_stdlib
    hardware_adc
    hardware_dma
    hardware_spi
    hardware_pio
    hardware_vreg
    hardware_clocks
    hardware_timer
    hardware_uart
    pico_multicore
    pico_float
    LovyanGFX
)

pico_enable_stdio_usb(rtty_decoder 1)
pico_enable_stdio_uart(rtty_decoder 1) # Enable UART output as well
pico_add_extra_outputs(rtty_decoder)
