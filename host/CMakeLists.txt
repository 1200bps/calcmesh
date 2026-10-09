# Host (Linux) build of the protocol code: unit tests, plus meshcat, which runs
# the same handshake against a node on a local serial port.
#
#   cmake -S host -B build-host && cmake --build build-host
#   ctest --test-dir build-host
#   build-host/meshcat -1 /dev/ttyACM0
cmake_minimum_required(VERSION 3.13)
project(calcmesh_host C)

set(CMAKE_C_STANDARD 11)
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

include(${REPO_ROOT}/cmake/meshtastic_protos.cmake)

set(WARN -Wall -Wextra -Werror)

set(MESH_SRCS
    ${REPO_ROOT}/src/mesh/frame.c
    ${REPO_ROOT}/src/mesh/link.c
    ${REPO_ROOT}/src/mesh/nodedb.c)

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
