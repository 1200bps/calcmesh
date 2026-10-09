#!/usr/bin/env python3
"""Show the calcmesh screen on a PC, from the "calcmesh screen" USB port.

Stands in for the LCD until it's wired up: the firmware mirrors its
framebuffer on that port (src/screenport.h has the format). Python 3 standard
library only; the live window needs tkinter.

  screen.py PORT OUT.png        save one screen and exit
  screen.py -w PORT OUT.png     keep OUT.png up to date (for a viewer that
                                reloads, e.g. feh -R 0.5 OUT.png)
  screen.py -v PORT             live window

PORT is /dev/serial/by-id/usb-calcmesh_Calcmesh_*-if05 (its ttyACM number
depends on what else is plugged in). The stream restarts from a whole screen
each time DTR comes up, so keep one reader on it at a time.
A regular file holding a captured stream works too. -s N scales the image
(default 2).
"""

import argparse
import base64
import os
import struct
import sys
import termios
import zlib

W = H = 320
STRIDE = W // 4
MAGIC = 0xFB


class Screen:
    def __init__(self):
        self.palette = [(0, 0, 0), (0x60,) * 3, (0xA8,) * 3, (0xFF,) * 3]
        self.rows = [bytes(STRIDE)] * H
        self.seen = [False] * H
        self.buf = bytearray()

    def complete(self):
        return all(self.seen)

    def feed(self, data):
        """Take stream bytes; return how many frames ended in them."""
        self.buf += data
        frames = 0
        b = self.buf
        i = 0
        while True:
            # Resync on the magic byte; anything else is a torn packet.
            while i < len(b) and b[i] != MAGIC:
                i += 1
            if len(b) - i < 4:
                break
            kind, arg = b[i + 1], b[i + 2] | b[i + 3] << 8
            need = {ord("P"): 12, ord("R"): STRIDE, ord("E"): 0}.get(kind)
            if need is None or (kind == ord("R") and arg >= H):
                i += 1
                continue
            if len(b) - i < 4 + need:
                break
            body = bytes(b[i + 4 : i + 4 + need])
            i += 4 + need
            if kind == ord("P"):
                self.palette = [tuple(body[k : k + 3]) for k in range(0, 12, 3)]
            elif kind == ord("R"):
                self.rows[arg] = body
                self.seen[arg] = True
            elif self.complete():
                frames += 1
        del b[:i]
        return frames

    def rgb_rows(self, scale):
        px = [bytes(c) * scale for c in self.palette]
        # Each framebuffer byte is four pixels.
        lut = [b"".join(px[(v >> sh) & 3] for sh in (6, 4, 2, 0)) for v in range(256)]
        for row in self.rows:
            line = b"".join(lut[v] for v in row)
            for _ in range(scale):
                yield line

    def png(self, scale):
        raw = b"".join(b"\0" + line for line in self.rgb_rows(scale))

        def chunk(kind, data):
            c = struct.pack(">I", len(data)) + kind + data
            return c + struct.pack(">I", zlib.crc32(kind + data))

        ihdr = struct.pack(">IIBBBBB", W * scale, H * scale, 8, 2, 0, 0, 0)
        return (
            b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 6))
            + chunk(b"IEND", b"")
        )


def open_port(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY)
    if os.isatty(fd):
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0  # iflag
        attrs[1] = 0  # oflag
        attrs[2] |= termios.CLOCAL | termios.CREAD
        attrs[3] = 0  # lflag: raw
        attrs[6][termios.VMIN] = 1
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def save(screen, path, scale):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(screen.png(scale))
    os.replace(tmp, path)


def run_file(fd, screen, out, scale, watch):
    """Read until a whole screen has arrived; with watch, keep going."""
    got = False
    while True:
        data = os.read(fd, 4096)
        if not data:
            break
        if screen.feed(data):
            save(screen, out, scale)
            got = True
            if not watch:
                return 0
    if not got and screen.complete():
        save(screen, out, scale)  # a capture cut off before its 'E'
        got = True
    if not got:
        print("no complete screen in the stream", file=sys.stderr)
    return 0 if got else 1


def run_view(fd, screen, scale):
    import tkinter as tk

    root = tk.Tk()
    root.title("calcmesh")
    label = tk.Label(root, bg="black", fg="white", text="waiting for the screen")
    label.pack()
    state = {"img": None}

    def readable(_fd, _mask):
        try:
            data = os.read(fd, 4096)
        except OSError:
            data = b""
        if not data:
            root.title("calcmesh (disconnected)")
            root.deletefilehandler(fd)
            return
        if screen.feed(data):
            png = base64.b64encode(screen.png(scale))
            state["img"] = tk.PhotoImage(data=png, format="png")
            label.configure(image=state["img"])

    root.createfilehandler(fd, tk.READABLE, readable)
    root.mainloop()
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join(__doc__.splitlines()[2:]),
    )
    ap.add_argument("-s", "--scale", type=int, default=2)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("-w", "--watch", action="store_true", help="keep OUT.png up to date")
    mode.add_argument("-v", "--view", action="store_true", help="live window")
    ap.add_argument("port")
    ap.add_argument("out", nargs="?")
    args = ap.parse_args()
    if not args.view and not args.out:
        ap.error("OUT.png is needed unless -v")
    scale = max(1, args.scale)

    fd = open_port(args.port)
    screen = Screen()
    try:
        if args.view:
            return run_view(fd, screen, scale)
        return run_file(fd, screen, args.out, scale, args.watch)
    except KeyboardInterrupt:
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
