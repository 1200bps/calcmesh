# calcmesh
Meshtastic client for PicoCalc

Talks to a Meshtastic node over the protobuf serial API on uart0
(GP0 = TX, GP1 = RX, 115200 8N1).

The node needs its serial API on the UART you wire up: in the node's
Serial module, enable it, set mode `PROTO`, 115200 baud, and the RX/TX pins
you're using.

## USB ports

The Pico's micro USB shows up as three serial ports:

- **console** (`/dev/serial/by-id/usb-calcmesh_Calcmesh_*-if00`, usually
  `/dev/ttyACM0`): debug log and commands. Type `help`.
- **node** (`...-if03`, usually `/dev/ttyACM1`): desktop mode. Once a PC
  starts talking on this port, it's a straight pipe to the node and calcmesh
  only listens, so the meshtastic CLI or web client works as if the node
  were plugged in:

      meshtastic --port /dev/ttyACM1 --info

  Desktop mode ends when the client sends its disconnect, the port closes,
  or the PC goes quiet for 10 minutes; calcmesh then re-runs its handshake
  with the node. `desktop on|off|auto` on the console forces it either way
  (default auto).
- **screen** (`...-if05`; its ttyACM number depends on what else is
  plugged in, so use the by-id name): the screen, mirrored for
  a PC to show while the LCD isn't hooked up (see below).

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
node, the channel table and the node list on the USB console. Text messages
print there as they arrive, and the console can send them:

    send 0 hello mesh        # to channel 0
    dm HILL you up?          # DM by short name, or !a1b2c3d4
    msgs                     # conversations, with unread counts
    msgs HILL 20             # last 20 messages of one, marks it read

Outgoing messages show their delivery as the node reports it: sending, sent
(in the node's queue), relayed (another node rebroadcast it), delivered (DM
acked by its destination), or failed with the reason.

## SD card

With a microSD card in the PicoCalc (FAT or exFAT, as cards come), every
text message is also saved to it, and the newest are loaded back at boot, so
chats survive a reboot. One file per conversation, under `/calcmesh/msgs/`:

    ch0.tsv            channel index 0
    dm-a1b2c3d4.tsv    DMs with !a1b2c3d4

Tab-separated with a header row (`n time from to ch id reply status err snr
rssi hops flags text`), so a card read on a PC opens in a spreadsheet, or:

    column -ts$'\t' /media/sd/calcmesh/msgs/ch0.tsv

Times are UTC by the node's clock; text has `\\ \t \n \r` escaped. An
outgoing message's status is rewritten in place as acks come in, and one
still in flight when the PicoCalc lost power comes back as failed (timed
out). The format is documented in `src/sd/msglog.h`. Other things will go
under `/calcmesh/` later.

No card, or a card pulled out, and calcmesh carries on in RAM; put one in
and logging resumes (it's on spi0, GP16-19, with card detect on GP22).
`sd_print_status()` (for an `sd` console command) shows the state.

## Testing without the Pico

The protocol code in `src/mesh/` builds on Linux too. `host/` has unit tests
against a simulated node, and `meshcat`, which runs the same handshake against
a node on a local serial port, e.g. the node's own USB port:

    cmake -S host -B build-host && cmake --build build-host
    ctest --test-dir build-host
    build-host/meshcat -1 /dev/ttyACM0   # -1: exit after the first dump
    build-host/meshcat /dev/ttyACM0      # stay up; type console commands
    build-host/meshcat -1 -x 'send 0 test' /dev/ttyACM0  # send, wait for the ack

## The screen, without the screen

The UI draws into a 320x320 framebuffer in RAM (`src/ui/fb.c`, 2 bits per
pixel, 25 KB), so it can be developed and checked on Linux before the LCD
driver exists. `uishot` renders it against a made-up mesh and saves PNGs:

    build-host/uishot -t shots/                          # tour of the main screens
    build-host/uishot out.png tab enter =hello left left  # press keys, save one screen

Keys for `uishot`: `tab s-tab enter esc up down left right home end pgup
pgdn bs del f1`..`f10`, and `=TEXT` types TEXT. `-s N` scales the PNG
(default 2).

The tabs run along the bottom of the screen as five softkey slots, one over
each of F1-F5 (Nodes and Chats so far), after the HP 48/49/50 menu row.

In the UI: F1-F5 or Tab / Shift-Tab switch tabs; arrows,
PgUp/PgDn, Home/End move; Enter opens a chat (on a node: a DM with it) or
sends what you typed; Esc leaves a chat.

On the PicoCalc, the same framebuffer goes out the **screen** USB port
whenever something has the port open, and `tools/screen.py` (Python 3, no
extra packages) shows it:

    SCREEN=$(ls /dev/serial/by-id/usb-calcmesh_*-if05)
    tools/screen.py -v $SCREEN               # live window (needs tkinter)
    tools/screen.py $SCREEN shot.png         # save one screen
    tools/screen.py -w $SCREEN live.png &    # keep a PNG current...
    feh -R 0.5 live.png                      # ...for any viewer that reloads

Use the PicoCalc's own keyboard to drive it. Only changed rows are sent,
nothing at all while the screen is still or the port is closed, and sending
never holds up the rest of the firmware.

A display driver only needs `fb_flush()`, which hands it the runs of rows
that changed since the last flush, and `fb_row_rgb565()` (or `fb_palette`)
to convert a row to its pixel format.

## Layout

- `src/mesh/frame.[ch]` – serial framing (`0x94 0xC3` + 16-bit length)
- `src/mesh/link.[ch]` – want_config handshake, retries, heartbeat, FromRadio dispatch
- `src/mesh/nodedb.[ch]` – in-memory node and channel tables, text dump
- `src/mesh/msgs.[ch]` – text message store: per-channel and per-DM conversations, delivery status
- `src/mesh/cmd.[ch]` – console commands shared by the firmware and `meshcat`
- `src/sd/msglog.[ch]` – message history on the card (portable; host-tested on a RAM disk)
- `src/sd/sd.[ch]` – card detect, mount, retries; `sd_card.c` – SPI SD driver for FatFs
- `src/ui/fb.[ch]` – framebuffer, drawing primitives, changed-row tracking
- `src/ui/ui*.c` – tab bar, Nodes and Chats views, compose line
- `src/ui/font_*.c` – [Spleen](https://github.com/fcambus/spleen) 6x12 and 8x16 (BSD 2-Clause), generated by `tools/bdf2c.py`
- `src/input/keys.h` – key codes and events, shared with the keyboard driver
- `src/meshlink.[ch]` – uart0 transport (interrupt-driven receive)
- `src/screenport.[ch]` – the framebuffer, mirrored on the screen USB port
- `src/main.c` – entry point
- `host/` – Linux unit tests, `meshcat` and `uishot`; `host/fake/` holds the
  made-up mesh, a stand-in link the UI is tested against, and a stand-in USB
  port for the screen mirror
- `tools/screen.py` – shows the screen USB port on a PC
- `cmake/meshtastic_protos.cmake` – generates nanopb C from the protobufs at build time
- `lib/fatfs` – [FatFs](http://elm-chan.org/fsw/ff/) R0.16, vendored (config in `src/sd/ffconf.h`)
- `lib/nanopb`, `lib/protobufs` – submodules ([nanopb](https://github.com/nanopb/nanopb), [meshtastic/protobufs](https://github.com/meshtastic/protobufs))

Bump the protobufs submodule to track newer Meshtastic firmware.
