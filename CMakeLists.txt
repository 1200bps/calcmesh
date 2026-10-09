cmake_minimum_required(VERSION 3.13)

# The PicoCalc ships with a Pico (RP2040) but takes a Pico 2 (RP2350) as
# well. Pick with -DPICO_BOARD=pico|pico_w|pico2|pico2_w.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Board type")
endif()

# Must come before project()
include(pico_sdk_import.cmake)

project(calcmesh C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include(cmake/meshtastic_protos.cmake)

add_executable(calcmesh
    src/main.c
    src/meshlink.c
    src/mesh/frame.c
    src/mesh/link.c
    src/mesh/nodedb.c
)

target_include_directories(calcmesh PRIVATE src)
target_compile_options(calcmesh PRIVATE -Wall -Wextra)

target_link_libraries(calcmesh
    pico_stdlib
    hardware_uart
    pico_rand
    meshtastic_protos
)

# Console on USB CDC. uart0 belongs to the radio link, so keep stdio off it.
pico_enable_stdio_usb(calcmesh 1)
pico_enable_stdio_uart(calcmesh 0)

# 1200-baud CDC "touch" -> BOOTSEL, and `picotool reboot -f -u` via the vendor
# interface. These default on only while nothing links TinyUSB directly, so
# pin them. SDK > 2.2.0 renamed them; set both spellings.
target_compile_definitions(calcmesh PRIVATE
    PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
)

pico_set_program_name(calcmesh "calcmesh")
pico_add_extra_outputs(calcmesh)
