cmake_minimum_required(VERSION 3.13)

# The PicoCalc ships with a Pico (RP2040) but takes a Pico 2 (RP2350) as
# well. Pick with -DPICO_BOARD=pico|pico_w|pico2|pico2_w.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Board type")
endif()

# Must come before project()
include(pico_sdk_import.cmake)

project(calcmesh VERSION 0.1.1 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include(cmake/meshtastic_protos.cmake)

add_executable(calcmesh
    src/main.c
    src/audio/chime.c
    src/audio/morse.c
    src/input/kbd.c
    src/input/backlight.c
    src/input/kbd_map.c
    src/lcd/lcd.c
    src/meshlink.c
    src/screenport.c
    src/mesh/frame.c
    src/mesh/link.c
    src/mesh/msgs.c
    src/mesh/cmd.c
    src/mesh/nodedb.c
    src/mesh/nodeedit.c
    src/mesh/remoteadmin.c
    src/mesh/metrics.c
    src/mesh/metsched.c
    src/ui/fb.c
    src/ui/font.c
    src/ui/font_6x12.c
    src/ui/font_8x16.c
    src/ui/ui.c
    src/ui/ui_chats.c
    src/ui/ui_nodes.c
    src/ui/ui_position.c
    src/ui/ui_metrics.c
    src/ui/ui_settings.c
    src/sd/metlog.c
    src/sd/metview.c
    src/sd/msglog.c
    src/sd/settings.c
    src/sd/sd.c
    src/sd/sd_card.c
    src/usb/usb_descriptors.c
    lib/fatfs/ff.c
    lib/fatfs/ffunicode.c
)

# src/mesh: the UI includes link.h, msgs.h etc. by bare name.
# src/usb first: its tusb_config.h (two CDC ports) replaces pico_stdio_usb's.
target_include_directories(calcmesh PRIVATE src/usb src src/mesh lib/fatfs src/sd)
target_compile_options(calcmesh PRIVATE -Wall -Wextra)
# Vendored as is; not ours to warn about.
set_source_files_properties(lib/fatfs/ff.c lib/fatfs/ffunicode.c PROPERTIES COMPILE_OPTIONS -w)

target_link_libraries(calcmesh
    pico_stdlib
    hardware_uart
    hardware_i2c
    hardware_spi
    hardware_dma
    hardware_pwm
    pico_rand
    pico_unique_id
    tinyusb_device
    meshtastic_protos
)

# Console on the first USB CDC port; the second is the desktop-mode bridge to
# the node (src/usb/usb_descriptors.c). uart0 belongs to the radio link, so
# keep stdio off it.
pico_enable_stdio_usb(calcmesh 1)
pico_enable_stdio_uart(calcmesh 0)

# 1200-baud CDC "touch" -> BOOTSEL, and `picotool reboot -f -u` via the vendor
# interface. These default on only while nothing links TinyUSB directly (we
# do, for the second CDC port), so pin them. SDK > 2.2.0 renamed them; set
# both spellings.
target_compile_definitions(calcmesh PRIVATE
    CALCMESH_VERSION="${PROJECT_VERSION}"
    PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
)

pico_set_program_name(calcmesh "calcmesh")
pico_set_program_version(calcmesh "${PROJECT_VERSION}")
pico_add_extra_outputs(calcmesh)
