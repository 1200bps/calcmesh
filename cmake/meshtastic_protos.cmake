# Generates nanopb C sources for the Meshtastic protobufs at build time and
# wraps them, plus the nanopb runtime, in the `meshtastic_protos` library.
#
# Needs a Python 3 with `protobuf` and `grpcio-tools` installed
# (see lib/nanopb/extra/requirements.txt).

set(NANOPB_DIR ${CMAKE_CURRENT_LIST_DIR}/../lib/nanopb)
set(PROTO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../lib/protobufs)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Only what a client needs: mesh.proto and admin.proto plus their imports.
set(MESHTASTIC_PROTOS
    admin
    atak
    channel
    config
    connection_status
    device_ui
    field_metadata
    mesh
    module_config
    portnums
    telemetry
    xmodem
)

set(_proto_files)
set(_proto_deps)
set(_gen_srcs)
set(_gen_hdrs)
foreach(p ${MESHTASTIC_PROTOS})
    list(APPEND _proto_files meshtastic/${p}.proto)
    list(APPEND _proto_deps ${PROTO_ROOT}/meshtastic/${p}.proto)
    if(EXISTS ${PROTO_ROOT}/meshtastic/${p}.options)
        list(APPEND _proto_deps ${PROTO_ROOT}/meshtastic/${p}.options)
    endif()
    list(APPEND _gen_srcs ${PROTO_GEN_DIR}/meshtastic/${p}.pb.c)
    list(APPEND _gen_hdrs ${PROTO_GEN_DIR}/meshtastic/${p}.pb.h)
endforeach()

add_custom_command(
    OUTPUT ${_gen_srcs} ${_gen_hdrs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}
    COMMAND ${Python3_EXECUTABLE} ${NANOPB_DIR}/generator/nanopb_generator.py
            -q -D ${PROTO_GEN_DIR} -I ${PROTO_ROOT} ${_proto_files}
    WORKING_DIRECTORY ${PROTO_ROOT}
    DEPENDS ${_proto_deps}
    COMMENT "Generating nanopb sources for Meshtastic protobufs"
    VERBATIM
)

add_library(meshtastic_protos STATIC
    ${_gen_srcs}
    ${NANOPB_DIR}/pb_common.c
    ${NANOPB_DIR}/pb_encode.c
    ${NANOPB_DIR}/pb_decode.c
)
target_include_directories(meshtastic_protos PUBLIC ${PROTO_GEN_DIR} ${NANOPB_DIR})
