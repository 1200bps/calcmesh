# calcmesh
Meshtastic client for PicoCalc

Talks to a Meshtastic node over the protobuf serial API on uart0
(GP0 = TX, GP1 = RX, 115200 8N1).

The node needs its serial API on the UART you wire up: in the node's
Serial module, enable it, set mode `PROTO`, 115200 baud, and the RX/TX pins
you're using.

## USB ports

The Pico's micro USB shows up as two serial ports:

- **console** (`/dev/serial/by-id/usb-calcmesh_Calcmesh_*-if00`, usually
  `/dev/ttyACM0`): debug log and commands. Type `help`.
- **node** (`...-if03`, usually `/dev/ttyACM1`): desktop mode. While a PC
  has this port open, it's a straight pipe to the node and calcmesh only
  listens, so the meshtastic CLI or web client works as if the node were
  plugged in:

      meshtastic --port /dev/ttyACM1 --info

  When the port closes, calcmesh re-runs its handshake with the node.
  `desktop on|off|auto` on the console forces it either way (default auto).

The PicoCalc's own USB-C port can't be used to reach the node: its serial
chip only hears GP0 (what the Pico sends), not the node's replies.

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

On boot it runs the `want_config` handshake with the node and prints your
node, the channel table and the node list on the USB console.

## Testing without the Pico

The protocol code in `src/mesh/` builds on Linux too. `host/` has unit tests
against a simulated node, and `meshcat`, which runs the same handshake against
a node on a local serial port, e.g. the node's own USB port:

    cmake -S host -B build-host && cmake --build build-host
    ctest --test-dir build-host
    build-host/meshcat -1 /dev/ttyACM0   # -1: exit after the first dump

## Layout

- `src/mesh/frame.[ch]` – serial framing (`0x94 0xC3` + 16-bit length)
- `src/mesh/link.[ch]` – want_config handshake, retries, heartbeat, FromRadio dispatch
- `src/mesh/nodedb.[ch]` – in-memory node and channel tables, text dump
- `src/meshlink.[ch]` – uart0 transport (interrupt-driven receive)
- `src/main.c` – entry point
- `host/` – Linux unit tests and `meshcat`
- `cmake/meshtastic_protos.cmake` – generates nanopb C from the protobufs at build time
- `lib/nanopb`, `lib/protobufs` – submodules ([nanopb](https://github.com/nanopb/nanopb), [meshtastic/protobufs](https://github.com/meshtastic/protobufs))

Bump the protobufs submodule to track newer Meshtastic firmware.
