#!/usr/bin/env python3
"""tools/screen.py's key mapping: PC keys to the 'K' packets the firmware's
screen port takes (src/screenport.h)."""

import os
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import screen  # noqa: E402

F1 = 0x110
SHIFT, CTRL, ALT = 1, 2, 4
cases = [
    # keysym, char, shift, ctrl, alt -> key, mods
    (("a", "a"), ("a", 0)),
    (("A", "A", True), ("A", SHIFT)),
    (("exclam", "!", True), ("!", SHIFT)),
    (("space", " "), (" ", 0)),
    (("Return", "\r"), (0x0A, 0)),
    (("BackSpace", "\b"), (0x08, 0)),
    (("Escape", "\x1b"), (0x1B, 0)),
    (("Tab", "\t"), (0x09, 0)),
    (("ISO_Left_Tab", "", True), (0x09, SHIFT)),
    (("Down", ""), (0x101, 0)),
    (("Next", ""), (0x107, 0)),
    (("F1", ""), (F1, 0)),
    (("F5", ""), (F1 + 4, 0)),
    (("F1", "", True), (F1 + 5, SHIFT)),
    (("F5", "", True), (F1 + 9, SHIFT)),
    (("F7", ""), (F1 + 6, 0)),
    (("c", "\x03", False, True), ("c", CTRL)),
    (("C", "\x03", True, True), ("c", SHIFT | CTRL)),
    (("x", "x", False, False, True), ("x", ALT)),
]
none = [("Shift_L", ""), ("Control_L", ""), ("eacute", "é"), ("F11", ""),
        ("Super_L", ""), ("Menu", "")]

bad = 0
for args, (key, mods) in cases:
    key = ord(key) if isinstance(key, str) else key
    want = struct.pack("<BBHB", 0xFB, ord("K"), key, mods)
    got = screen.key_packet(*args)
    if got != want:
        print("key_packet%r = %r, want %r" % (args, got, want))
        bad += 1
for args in none:
    got = screen.key_packet(*args)
    if got is not None:
        print("key_packet%r = %r, want None" % (args, got))
        bad += 1
sys.exit(bad != 0)
