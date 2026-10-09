# calcmesh
Meshtastic client for PicoCalc

Talks to a Meshtastic node over the protobuf serial API on uart0
(GP0 = TX, GP1 = RX, 115200 8N1). The debug console is USB CDC.

The node needs its serial API on the UART you wire up: in the node's
Serial module, enable it, set mode `PROTO`, 115200 baud, and the RX/TX pins
you're using.

## Building

Needs the [pico-sdk](https://github.com/raspberrypi/pico-sdk) (2.x, with
its `lib/tinyusb` submodule), an `arm-none-eabi` GCC, CMake, Ninja, and
Python 3 with the nanopb generator's dependencies:

    git submodule update --init
    pip install -r lib/nanopb/extra/requirements.txt

    export PICO_SDK_PATH=/path/to/pico-sdk
    cmake -S . -B build -G Ninja              # Pico (RP2040), the PicoCalc default
    cmake -S . -B build -G Ninja -DPICO_BOARD=pico2   # Pico 2 (RP2350)
    cmake --build build

Flash `build/calcmesh.uf2` the usual way (hold BOOTSEL, copy it over).

## Layout

- `src/meshlink.[ch]` – serial framing and ToRadio/FromRadio encode/decode
- `src/main.c` – entry point
- `cmake/meshtastic_protos.cmake` – generates nanopb C from the protobufs at build time
- `lib/nanopb`, `lib/protobufs` – submodules ([nanopb](https://github.com/nanopb/nanopb), [meshtastic/protobufs](https://github.com/meshtastic/protobufs))

Bump the protobufs submodule to track newer Meshtastic firmware.
