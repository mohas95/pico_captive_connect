# ====================================================================================
# CMake Pico project file for pico_web (dual-mode: library + optional demo)
# ====================================================================================

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fetch Pico SDK automatically if not already provided
set(PICO_SDK_FETCH_FROM_GIT ON)
set(PICOTOOL_FETCH_FROM_GIT_PATH "${CMAKE_SOURCE_DIR}/.pico-deps" CACHE PATH "Shared picotool path")

# Board type (defaults to pico2_w for Wi-Fi support)
if (NOT PICO_BOARD)
    set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif()

# Import Pico SDK
include(pico_sdk_import.cmake)

project(pico_captive_connect C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# ====================================================================================
# Library definition
# ====================================================================================

add_library(pico_captive_connect
        src/creds_store.cpp
        src/http_portal.cpp
        src/sta_portal.cpp
        src/dns_hijack.cpp
        src/dhcpserver.c
        src/pico_captive_connect.cpp
)

target_include_directories(pico_captive_connect PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(pico_captive_connect
        pico_stdlib
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mqtt
        hardware_flash
        hardware_sync
)

# ====================================================================================
# Standalone executable (only builds if this repo is the root project)
# ====================================================================================
if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    add_executable(pico_captive_connect_standalone
        src/main.cpp
    )

    target_link_libraries(pico_captive_connect_standalone pico_captive_connect)

    pico_set_program_name(pico_captive_connect_standalone "pico_captive_connect_standalone")
    pico_set_program_version(pico_captive_connect_standalone "0.1")

    # Enable USB stdio, disable UART
    pico_enable_stdio_uart(pico_captive_connect_standalone 0)
    pico_enable_stdio_usb(pico_captive_connect_standalone 1)

    pico_add_extra_outputs(pico_captive_connect_standalone)
endif()
