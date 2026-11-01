#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Drive an FM-1 that runs an INOP_PROBE=1 build over its USB serial port. Standard library only; ffmpeg for `record`.

  tools/inop_probe.py 'tap B_LFO' 'knob 1 5' 'shot /tmp/a.png' stat
  tools/inop_probe.py -f pattern.txt

A command is a device command (firmware/inop/probe.c), or one of these:
  tap BUTTON | hold BUTTON | free BUTTON      press and release, press, release
  down KEY | up KEY                           a note key, 0 to 26
  knob N DELTA                                N: 0 SELECT, 1 to 4, 5 ALGO
  wait MS
  shot FILE.png
  record FILE.wav SECONDS                     the master as the USB audio input sends it
Names from the firmware's enums (PV_FLT_CUT, G_DELAY_FB, B_LFO, E_K1, MOD_SRC_LFO1) stand for their numbers."""
import argparse
import glob
import os
import re
import select
import struct
import subprocess
import sys
import termios
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENUM_FILES = ("firmware/inop/model.h", "firmware/inop/ui.c", "firmware/inop/voice_chain.c", "firmware/hal/fm1_input.h")
PRESS_MS = 40                # longer than the release debounce, about 9 ms, and a main loop pass, 15 ms
READ_S = 5.0
KNOB_ENC = ("E_SELECT", "E_K1", "E_K1_2", "E_K1_3", "E_K1_4", "E_ALGO")


def enum_names():
    names = {}
    for rel in ENUM_FILES:
        text = re.sub(r"/\*.*?\*/", "", (ROOT / rel).read_text(), flags=re.S)
        for body in re.findall(r"enum\s*\w*\s*\{(.*?)\}", text, flags=re.S):
            value = 0
            for item in body.split(","):
                m = re.fullmatch(r"\s*(\w+)\s*(?:=\s*(\w+))?\s*", item)
                if not m:
                    continue
                if m.group(2):
                    v = m.group(2)
                    if v in names:
                        value = names[v]
                    else:
                        try:
                            value = int(v, 0)
                        except ValueError:
                            continue
                names[m.group(1)] = value
                value += 1
    names["E_K1_2"], names["E_K1_3"], names["E_K1_4"] = names["E_K1"] + 1, names["E_K1"] + 2, names["E_K1"] + 3
    return names


class Port:
    def __init__(self):
        ports = glob.glob("/dev/cu.usbmodem*")
        if len(ports) != 1:
            sys.exit(f"need one /dev/cu.usbmodem port, found {ports}: is an INOP_PROBE build installed?")
        self.fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY)
        mode = termios.tcgetattr(self.fd)
        mode[0] = mode[1] = mode[3] = 0
        mode[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, mode)
        time.sleep(0.05)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def read(self, n):
        out = bytearray()
        while len(out) < n:
            if not select.select([self.fd], [], [], READ_S)[0]:
                sys.exit(f"the device stopped answering after {len(out)} of {n} bytes")
            out += os.read(self.fd, n - len(out))
        return bytes(out)

    def line(self):
        out = bytearray()
        while not out.endswith(b"\n"):
            out += self.read(1)
        return out[:-1].decode()

    def ask(self, command):
        os.write(self.fd, command.encode() + b"\n")
        lines = []
        while True:
            line = self.line()
            if line == "ok":
                return lines
            if line == "err":
                sys.exit(f"the device refused: {command}")
            lines.append(line)


def to_rgb(hi, lo):
    c = hi << 8 | lo
    return bytes(((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31))


def write_png(path, frame):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + bytes(frame[y]) for y in range(240))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 240, 240, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def screenshot(port, path):
    frame = [bytearray(240 * 3) for _ in range(240)]
    os.write(port.fd, b"shot\n")
    while True:
        kind = port.read(1)
        if kind == b"E":
            break
        if kind not in (b"F", b"R"):
            sys.exit(f"bad screen record {kind!r}")
        x, y, w, h = port.read(4)
        if kind == b"F":
            rgb = to_rgb(*port.read(2)) * w
            for row in range(y, y + h):
                frame[row][x * 3:(x + w) * 3] = rgb
        else:
            data = port.read(w * h * 2)
            for row in range(h):
                line = data[row * w * 2:(row + 1) * w * 2]
                frame[y + row][x * 3:(x + w) * 3] = b"".join(to_rgb(line[i], line[i + 1]) for i in range(0, w * 2, 2))
    if port.line() != "ok":
        sys.exit("the screenshot did not end with ok")
    write_png(path, frame)


def audio_device():
    listing = subprocess.run(["ffmpeg", "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
                             capture_output=True, text=True).stderr
    audio = listing.split("audio devices", 1)[1]
    m = re.search(r"\[(\d+)\] (Felucca|FM-1|Skiff)", audio)
    if not m:
        sys.exit("no FM-1 audio input:\n" + audio)
    return m.group(1)


def record(path, seconds):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "avfoundation", "-i", ":" + audio_device(),
                    "-t", seconds, "-c:a", "pcm_s16le", path], check=True)


def run(port, names, command):
    word, *args = command.split()
    args = [str(names.get(a, a)) for a in args]
    if word == "tap":
        port.ask(f"btn {args[0]} 1")
        time.sleep(PRESS_MS / 1000)
        port.ask(f"btn {args[0]} 0")
        time.sleep(PRESS_MS / 1000)
    elif word in ("hold", "free"):
        port.ask(f"btn {args[0]} {int(word == 'hold')}")
        time.sleep(PRESS_MS / 1000)
    elif word in ("down", "up"):
        port.ask(f"key {args[0]} {int(word == 'down')}")
        time.sleep(PRESS_MS / 1000)
    elif word == "knob":
        port.ask(f"enc {names[KNOB_ENC[int(args[0])]]} {args[1]}")
        time.sleep(PRESS_MS / 1000)
    elif word == "wait":
        time.sleep(int(args[0]) / 1000)
    elif word == "shot":
        screenshot(port, args[0])
    elif word == "record":
        record(args[0], args[1])
    else:
        for line in port.ask(" ".join([word, *args])):
            print(line)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-f", "--file", help="commands, one a line; # starts a comment")
    parser.add_argument("commands", nargs="*")
    o = parser.parse_args()
    commands = list(o.commands)
    if o.file:
        commands = [c for c in (l.split("#")[0].strip() for l in Path(o.file).read_text().splitlines()) if c] + commands
    port, names = Port(), enum_names()
    for command in commands:
        run(port, names, command)


if __name__ == "__main__":
    main()
