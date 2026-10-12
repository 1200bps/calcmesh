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

Prebuilt UF2s for the Pico and Pico 2 are on the
[releases page](https://github.com/1200bps/calcmesh/releases); `version` on
the console, the Settings tab, or `picotool info` says which one is flashed.
To build your own:

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

    ch-LongFast-1c2d3e4f.tsv   channel LongFast
    dm-a1b2c3d4.tsv            DMs with !a1b2c3d4
    channels.txt               which channel index is which file

A channel's file is named for the channel (and a hash of its name and key),
not its index, so its history follows it if the node's channels are
reordered. Cards written before that have `ch0.tsv` and so on; each is
renamed the first time the node says which channel that index is.

Tab-separated with a header row (`n time from to ch id reply status err snr
rssi hops flags text`), so a card read on a PC opens in a spreadsheet, or:

    column -ts$'\t' /media/sd/calcmesh/msgs/ch-LongFast-*.tsv

Times are UTC by the node's clock; text has `\\ \t \n \r` escaped. An
outgoing message's status is rewritten in place as acks come in, and one
still in flight when the PicoCalc lost power comes back as failed (timed
out). The format is documented in `src/sd/msglog.h`.

Settings live in `/calcmesh/settings.txt`, one `key = value` per line, which
is fine to edit on a PC. So far that's the screen and keyboard backlight
levels (`screen.backlight`, `keys.backlight`, 0-255), which the keyboard chip
forgets whenever it loses power: they're saved a few seconds after you change
them (with `bl`/`kbl` on the console or the keyboard's own Alt+, Alt+.
Alt+Space) and put back at boot. Without a card they're kept until one turns
up. Desktop mode and its idle timeout (`desktop.mode`, `desktop.idle_min`)
and console key echo (`console.key_echo`) are kept there too; all of them can
be changed on the Settings tab.

No card, or a card pulled out, and calcmesh carries on in RAM; put one in
and logging resumes (it's on spi0, GP16-19, with card detect on GP22).
`sd` on the console shows the state.

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

The status line along the top shows your node and primary channel, then the
link to the node (a solid square and "connected", a hollow one while
syncing, a crossed one with "no node" when the node isn't answering, or an
inverted DESKTOP tag while a PC has it) and the PicoCalc's own battery
(`+` while charging, inverted at 15% and under, `usb` with no cells in).

Keys for `uishot`: `tab s-tab enter esc up down left right home end pgup
pgdn bs del f1`..`f10`, and `=TEXT` types TEXT. `-s N` scales the PNG
(default 2).

The tabs run along the bottom of the screen as five softkey slots, one over
each of F1-F5 (Nodes, Chats, Position, Metrics and Settings), after
the HP 48/49/50 menu row.

In the UI: F1-F5 or Tab / Shift-Tab switch tabs; arrows,
PgUp/PgDn, Home/End move; Enter opens a chat (on a node: a DM with it) or
sends what you typed; Esc leaves a chat.

On the Settings tab, Left/Right change a value and Enter edits a name (Enter
again to keep it, Esc to drop it). The node's settings (names, role, LoRa
region, preset, hop limit and power, channel names, position, power saving,
Bluetooth) are edited on a copy: changed fields get a `*` and show what they
were, and nothing goes to the node until "Write changes to the node", which
lists them and asks for `Y`. They go out as admin messages to our own node in
one edit transaction; the node saves them and restarts, and calcmesh reads
everything back and says whether it all took. calcmesh's own settings take
effect at once and are saved to the SD card.

The top row, "Settings of", picks another node to manage over the mesh
(remote admin): our node first, then favorites, then any other node whose
public key we have. Nothing is sent until "Fetch", which asks it for its
settings one message at a time (a dozen or so packets). Edits then work the
same way, and a write goes over the mesh with the node's session key, then
calcmesh waits for it to restart and reads the changes back. The other node
only listens if our node's public key is one of its admin keys
(Security > Admin key in the phone app). The Settings tab shows our key in
base64 under our own node's settings, so you can paste it there.

Position (F3) plots every node that has shared a position as a point on a
blank, north-up plane around our own node (a hollow square), labelled with
short names and fading with time since heard; there are no maps. Arrows
pan, `+`/`-` zoom, `0` or Home fits everyone back in, `n`/`p` (or
PgDn/PgUp) pick the next node outward from us, showing its distance,
bearing and last-heard, `c` centres on it, Enter opens a DM with it and Esc
drops it. Positions come from the node's NodeInfo dump and from
POSITION_APP packets as they arrive.

Metrics (F4) shows telemetry: a row per node with battery, voltage and a
sparkline of its history, channel and airtime use, temperature and how old
the newest numbers are; our node first, then favorites, then nodes that have
sent metrics. Enter opens a node's page: device metrics, environment and
power sensors, and local stats (packet counts, relays, dupes, noise floor),
each with how old it is, and graphs of battery, channel use and temperature
over the last 3h, 12h, 24h, 3d or 7d (`[` and `]` step through; the choice
is kept in the settings as `ui.metrics.window`). `D`, `E`, `P` and `L` ask the node for a fresh set of
that kind: our own node answers over the serial link, any other node with one
packet each way on the primary channel (PKI when our node has its key).
Local stats from other nodes only come by asking. Left/Right step through the
nodes, Esc goes back. Metrics arrive from broadcasts, our node's own reports
to its client (every minute), and the NodeInfo dump; calcmesh keeps the
latest of each kind and a history point every 10 minutes at most, for 16
nodes, and that survives a resync with the node. The graphs read the card's
metrics log (below) back too, so a reboot doesn't start them over, a bit at
a time from the main loop; until that's done they show what's in RAM. On the
console, `metrics [node]` prints what's kept and `telreq <node> [device|env|
power|local]` asks.

A node can also be asked on a schedule, for the graphs and a log: `S` on its
page picks the kinds (`D` `E` `P` `L`) and how often (Left/Right, 30 minutes
to a day), Enter saves, and picking none turns it off. Up to 4 nodes; a
scheduled node has a little clock by its name in the list. Each run asks for
the kinds one at a time, a minute apart (an answer over several LongFast hops
can take a while), waits 90 s for each answer, and asks once more five
minutes later for one that got none; a request made by hand goes first. The
schedule is at the bottom of the node's page. Schedules are kept in the settings (`metrics.sched1`-`4`), and the
first run after boot comes 2 minutes after the node connects. Telemetry from
every node, asked for or not (our own node's unasked minute-by-minute
reports once per 10 minutes), and a line for each kind a run got nothing
for, go to
`/calcmesh/metrics/<node>-<kind>.tsv` on the card, one tab-separated file
per node and kind (`src/sd/metlog.h` lists the columns). Local stats add
graphs of packets heard per hour, nodes online and noise floor, and device
metrics one of airtime. On the console, `metsched` lists the schedules and
`metsched <node> <kinds|off> [minutes]` sets one. Use it sparingly on a busy
mesh: every kind is a packet each way, each run.

On the PicoCalc, the same framebuffer goes out the **screen** USB port
whenever something has the port open, and `tools/screen.py` (Python 3, no
extra packages) shows it:

    SCREEN=$(ls /dev/serial/by-id/usb-calcmesh_*-if05)
    tools/screen.py -v $SCREEN               # live window (needs tkinter)
    tools/screen.py $SCREEN shot.png         # save one screen
    tools/screen.py -w $SCREEN live.png &    # keep a PNG current...
    feh -R 0.5 live.png                      # ...for any viewer that reloads

Keys typed into the live window go to the PicoCalc as if from its own
keyboard (Shift+F1..F5 are F6..F10, as there; keys the PicoCalc doesn't have
are ignored), and its own keyboard keeps working too. Only changed rows are
sent, nothing at all while the screen is still or the port is closed, and
sending never holds up the rest of the firmware.

A display driver only needs `fb_flush()`, which hands it the runs of rows
that changed since the last flush, and `fb_row_rgb565()` (or `fb_palette`)
to convert a row to its pixel format.

## Layout

- `src/mesh/frame.[ch]` – serial framing (`0x94 0xC3` + 16-bit length)
- `src/mesh/link.[ch]` – want_config handshake, retries, heartbeat, FromRadio dispatch
- `src/mesh/nodedb.[ch]` – in-memory node and channel tables, text dump
- `src/mesh/msgs.[ch]` – text message store: per-channel and per-DM conversations, delivery status
- `src/mesh/nodeedit.[ch]` – editing the node's settings over the admin API, and checking they took
- `src/mesh/remoteadmin.[ch]` – another node's settings over the mesh (PKI admin, session key, one message at a time)
- `src/mesh/metrics.[ch]` – telemetry per node, its history, and asking a node for it
- `src/mesh/metsched.[ch]` – asking nodes for metrics on a schedule
- `src/mesh/cmd.[ch]` – console commands shared by the firmware and `meshcat`
- `src/sd/msglog.[ch]` – message history on the card (portable; host-tested on a RAM disk)
- `src/sd/settings.[ch]` – client settings in `/calcmesh/settings.txt` (portable; host-tested)
- `src/sd/metlog.[ch]` – every node's telemetry, logged to the card (portable; host-tested)
- `src/sd/metview.[ch]` – the log read back for the Metrics graphs, per window (portable; host-tested)
- `src/sd/sd.[ch]` – card detect, mount, retries; `sd_card.c` – SPI SD driver for FatFs
- `src/ui/fb.[ch]` – framebuffer, drawing primitives, changed-row tracking
- `src/ui/ui*.c` – tab bar, Nodes, Chats, Position, Metrics and Settings views, compose line
- `src/ui/font_*.c` – [Spleen](https://github.com/fcambus/spleen) 6x12 and 8x16 (BSD 2-Clause), generated by `tools/bdf2c.py`
- `src/input/backlight.[ch]` – saves and restores the backlight levels
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
