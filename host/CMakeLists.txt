# Host (Linux) build of the protocol code: unit tests, plus meshcat, which runs
# the same handshake against a node on a local serial port.
#
#   cmake -S host -B build-host && cmake --build build-host
#   ctest --test-dir build-host
#   build-host/meshcat -1 /dev/ttyACM0
#   build-host/meshcat -1 -x 'send 0 hello' /dev/ttyACM0
cmake_minimum_required(VERSION 3.13)
project(calcmesh_host C)

set(CMAKE_C_STANDARD 11)
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

include(${REPO_ROOT}/cmake/meshtastic_protos.cmake)

set(WARN -Wall -Wextra -Werror)

set(MESH_SRCS
    ${REPO_ROOT}/src/mesh/frame.c
    ${REPO_ROOT}/src/mesh/link.c
    ${REPO_ROOT}/src/mesh/msgs.c
    ${REPO_ROOT}/src/mesh/cmd.c
    ${REPO_ROOT}/src/mesh/nodedb.c
    ${REPO_ROOT}/src/mesh/nodeedit.c
    ${REPO_ROOT}/src/mesh/remoteadmin.c
    ${REPO_ROOT}/src/mesh/metrics.c)

add_library(meshlink STATIC ${MESH_SRCS})
target_include_directories(meshlink PUBLIC ${REPO_ROOT}/src/mesh)
target_link_libraries(meshlink PUBLIC meshtastic_protos)
target_compile_options(meshlink PRIVATE ${WARN})

enable_testing()

add_executable(test_link test_link.c)
target_link_libraries(test_link meshlink)
target_compile_options(test_link PRIVATE ${WARN})
add_test(NAME link COMMAND test_link)

# Same code with a tiny node table, to exercise eviction.
add_executable(test_link_small test_link.c ${MESH_SRCS})
target_include_directories(test_link_small PRIVATE ${REPO_ROOT}/src/mesh)
target_compile_definitions(test_link_small PRIVATE MESH_MAX_NODES=4)
target_link_libraries(test_link_small meshtastic_protos)
target_compile_options(test_link_small PRIVATE ${WARN})
add_test(NAME link_small_table COMMAND test_link_small)

add_executable(meshcat meshcat.c)
target_link_libraries(meshcat meshlink)
target_compile_options(meshcat PRIVATE ${WARN})

# Message store alone, with a pool small enough to exercise eviction.
add_executable(test_msgs test_msgs.c ${REPO_ROOT}/src/mesh/msgs.c ${REPO_ROOT}/src/mesh/nodedb.c)
target_include_directories(test_msgs PRIVATE ${REPO_ROOT}/src/mesh)
target_compile_definitions(test_msgs PRIVATE MESH_MSG_POOL=6 MESH_MAX_DMS=2)
target_link_libraries(test_msgs meshtastic_protos)
target_compile_options(test_msgs PRIVATE ${WARN})
add_test(NAME msgs_store COMMAND test_msgs)

# UI: drawn into the in-memory framebuffer, against a made-up mesh
# (host/fake), and saved as PNG.
#
#   build-host/uishot -t shots/                  # tour of the main screens
#   build-host/uishot out.png tab enter =hello   # keys, then one screen
set(UI_SRCS
    ${REPO_ROOT}/src/ui/fb.c
    ${REPO_ROOT}/src/ui/font.c
    ${REPO_ROOT}/src/ui/font_6x12.c
    ${REPO_ROOT}/src/ui/font_8x16.c
    ${REPO_ROOT}/src/ui/ui.c
    ${REPO_ROOT}/src/ui/ui_nodes.c
    ${REPO_ROOT}/src/ui/ui_chats.c
    ${REPO_ROOT}/src/ui/ui_position.c
    ${REPO_ROOT}/src/ui/ui_metrics.c
    ${REPO_ROOT}/src/ui/ui_settings.c)

# FatFs, for the SD card code; ramdisk.c is its disk
add_library(fatfs_host STATIC ${REPO_ROOT}/lib/fatfs/ff.c ${REPO_ROOT}/lib/fatfs/ffunicode.c)
target_include_directories(fatfs_host PUBLIC ${REPO_ROOT}/lib/fatfs ${REPO_ROOT}/src/sd)
target_compile_definitions(fatfs_host PUBLIC FF_USE_MKFS=1)

# The real message store, node editing and settings table, but link.c stays
# out: fake_link.c stands in for its state, sending and clock, and fake_sd.c
# for the card's state.
add_library(ui_fake STATIC ${UI_SRCS}
    ${REPO_ROOT}/src/mesh/frame.c
    ${REPO_ROOT}/src/mesh/msgs.c
    ${REPO_ROOT}/src/mesh/nodedb.c
    ${REPO_ROOT}/src/mesh/nodeedit.c
    ${REPO_ROOT}/src/mesh/remoteadmin.c
    ${REPO_ROOT}/src/mesh/metrics.c
    ${REPO_ROOT}/src/sd/settings.c
    fake/fake_link.c
    fake/fake_sd.c
    fake/scene.c
    png.c
    # settings.c needs FatFs; one archive, so the linker finds the disk
    ramdisk.c ${REPO_ROOT}/lib/fatfs/ff.c ${REPO_ROOT}/lib/fatfs/ffunicode.c)
set_source_files_properties(${REPO_ROOT}/lib/fatfs/ff.c ${REPO_ROOT}/lib/fatfs/ffunicode.c
    PROPERTIES COMPILE_OPTIONS -w)
target_include_directories(ui_fake PUBLIC
    ${REPO_ROOT}/src ${REPO_ROOT}/src/ui ${REPO_ROOT}/src/mesh fake .)
target_link_libraries(ui_fake PUBLIC meshtastic_protos m)
target_include_directories(ui_fake PUBLIC ${REPO_ROOT}/lib/fatfs ${REPO_ROOT}/src/sd)
target_compile_definitions(ui_fake PUBLIC FF_USE_MKFS=1)
target_compile_options(ui_fake PRIVATE ${WARN})

add_executable(uishot uishot.c)
target_link_libraries(uishot ui_fake)
target_compile_options(uishot PRIVATE ${WARN})

# The USB screen mirror against a stand-in port, then tools/screen.py on what
# it sent.
add_executable(test_screenport test_screenport.c
    ${REPO_ROOT}/src/screenport.c fake/fake_tusb.c)
target_link_libraries(test_screenport ui_fake)
target_compile_options(test_screenport PRIVATE ${WARN})
add_test(NAME screenport COMMAND test_screenport)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    set(SP_DIR ${CMAKE_CURRENT_BINARY_DIR}/screenport)
    file(MAKE_DIRECTORY ${SP_DIR})
    add_test(NAME screen_py COMMAND sh -c
        "$<TARGET_FILE:test_screenport> ${SP_DIR} && \
         ${Python3_EXECUTABLE} ${REPO_ROOT}/tools/screen.py -s 1 ${SP_DIR}/stream.bin ${SP_DIR}/got.png && \
         ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/pngcmp.py ${SP_DIR}/expect.png ${SP_DIR}/got.png")
endif()

add_executable(test_ui test_ui.c)
target_link_libraries(test_ui ui_fake)
target_compile_options(test_ui PRIVATE ${WARN})
add_test(NAME ui COMMAND test_ui)
add_executable(test_kbd test_kbd.c ${REPO_ROOT}/src/input/kbd_map.c)
target_include_directories(test_kbd PRIVATE ${REPO_ROOT}/src)
target_compile_options(test_kbd PRIVATE ${WARN})
add_test(NAME kbd COMMAND test_kbd)

# Morse timing for the message chimes (src/audio/chime.c drives the speakers
# and stays on the Pico).
add_executable(test_morse test_morse.c ${REPO_ROOT}/src/audio/morse.c)
target_include_directories(test_morse PRIVATE ${REPO_ROOT}/src)
target_compile_options(test_morse PRIVATE ${WARN})
add_test(NAME morse COMMAND test_morse)

# SD card message history, on FatFs over a RAM disk, with the store built
# small so restore has to choose what comes back.

add_executable(test_msglog test_msglog.c ramdisk.c
    ${REPO_ROOT}/src/sd/msglog.c ${REPO_ROOT}/src/mesh/msgs.c ${REPO_ROOT}/src/mesh/nodedb.c)
target_include_directories(test_msglog PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/src/mesh .)
target_compile_definitions(test_msglog PRIVATE MESH_MSG_POOL=6 MESH_MAX_DMS=2)
target_link_libraries(test_msglog fatfs_host meshtastic_protos)
target_compile_options(test_msglog PRIVATE ${WARN})
add_test(NAME msglog COMMAND test_msglog)

add_executable(test_settings test_settings.c ramdisk.c ${REPO_ROOT}/src/sd/settings.c)
target_include_directories(test_settings PRIVATE ${REPO_ROOT}/src .)
target_link_libraries(test_settings fatfs_host)
target_compile_options(test_settings PRIVATE ${WARN})
add_test(NAME settings COMMAND test_settings)
