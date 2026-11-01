#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Drive an FM-1 that runs an INOP_PROBE=1 build over its USB serial port. macOS, standard library only.

  tools/inop_probe.py 'tap B_LFO' 'knob 1 5' 'shot /tmp/a.png' stat
  tools/inop_probe.py -f pattern.txt

A command is a device command (firmware/inop/probe.c), or one of these:
  tap BUTTON | hold BUTTON | free BUTTON      press and release, press, release
  down KEY | up KEY                           a note key, 0 to 26
  knob N DELTA                                N: 0 SELECT, 1 to 4, 5 ALGO
  wait MS
  shot FILE.png
  record FILE.wav SECONDS                     the master as the USB audio input sends it
Device commands that load a pattern:
  step PATTERN TRACK STEP FIELD VALUE         FIELD: 0 on, 1 note, 2 velocity, 3 length, 4 sound, 5 chance, 6 hits, 7 time
  lock PATTERN TRACK STEP VALUE_ID VALUE
  len|base|swing|human PATTERN TRACK VALUE
  snd SOUND VALUE_ID VALUE
  clear PATTERN
Names from the firmware's enums (PV_FLT_CUT, G_DELAY_FB, B_LFO, E_K1, MOD_SRC_LFO1) stand for their numbers."""
import argparse
import ctypes
import ctypes.util
import glob
import os
import re
import select
import struct
import sys
import termios
import time
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENUM_FILES = ("firmware/inop/model.h", "firmware/inop/ui.c", "firmware/inop/voice_chain.c", "firmware/hal/fm1_input.h")
PRESS_MS = 40                # longer than the release debounce, about 9 ms, and a main loop pass, 15 ms
READ_S = 5.0
RECORD_RATE = 44100.0
RECORD_BYTES_PER_FRAME = 4
RECORD_BUFFERS = 4
RECORD_BUFFER_BYTES = 8192
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


class AudioBuffer(ctypes.Structure):
    _fields_ = [("capacity", ctypes.c_uint32), ("data", ctypes.c_void_p), ("size", ctypes.c_uint32)]


class StreamFormat(ctypes.Structure):
    _fields_ = [("rate", ctypes.c_double), ("format_id", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("bytes_per_packet", ctypes.c_uint32),
                ("frames_per_packet", ctypes.c_uint32), ("bytes_per_frame", ctypes.c_uint32), ("channels", ctypes.c_uint32),
                ("bits", ctypes.c_uint32), ("reserved", ctypes.c_uint32)]


class PropertyAddress(ctypes.Structure):
    _fields_ = [("selector", ctypes.c_uint32), ("scope", ctypes.c_uint32), ("element", ctypes.c_uint32)]


def four_cc(text):
    return struct.unpack(">I", text.encode())[0]


def fm1_device_uid(audio, cf):
    """the CFString UID of the FM-1's audio input"""
    cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
    audio.AudioObjectGetPropertyData.argtypes = [ctypes.c_uint32, ctypes.POINTER(PropertyAddress), ctypes.c_uint32, ctypes.c_void_p,
                                                 ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]

    def get(device, selector, out):
        size = ctypes.c_uint32(ctypes.sizeof(out))
        address = PropertyAddress(four_cc(selector), four_cc("glob"), 0)
        status = audio.AudioObjectGetPropertyData(device, ctypes.byref(address), 0, None, ctypes.byref(size), ctypes.byref(out))
        if status:
            sys.exit(f"CoreAudio error {status} reading {selector}")
        return size.value

    devices = (ctypes.c_uint32 * 64)()
    count = get(1, "dev#", devices) // 4       # 1: the system object
    for device in devices[:count]:
        name, uid, text = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.create_string_buffer(256)
        get(device, "lnam", name)
        cf.CFStringGetCString(name, text, 256, 0x08000100)     # UTF-8
        if re.match(r"Felucca|FM-1|Skiff", text.value.decode()):
            get(device, "uid ", uid)
            return uid
    sys.exit("no FM-1 audio input: is an INOP_PROBE build installed?")


def record(path, seconds):
    """CoreAudio directly: ffmpeg's avfoundation input drops buffers, which moves every later hit in the file"""
    audio = ctypes.CDLL(ctypes.util.find_library("CoreAudio"))
    toolbox = ctypes.CDLL(ctypes.util.find_library("AudioToolbox"))
    cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))
    uid = fm1_device_uid(audio, cf)
    frames, next_sample, gaps = [], [None], [0]
    callback_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(AudioBuffer), ctypes.POINTER(ctypes.c_double),
                                     ctypes.c_uint32, ctypes.c_void_p)

    def on_buffer(_user, queue, buffer, start_time, _packets, _descriptions):
        size = buffer.contents.size
        if not size:                           # the queue stopping
            return
        sample_time = start_time[0]            # mSampleTime is the first field
        if next_sample[0] is not None and abs(sample_time - next_sample[0]) >= 1:
            gaps[0] += 1
        next_sample[0] = sample_time + size / RECORD_BYTES_PER_FRAME
        frames.append(ctypes.string_at(buffer.contents.data, size))
        toolbox.AudioQueueEnqueueBuffer(queue, buffer, 0, None)

    callback = callback_type(on_buffer)
    wanted = StreamFormat(RECORD_RATE, four_cc("lpcm"), 12, RECORD_BYTES_PER_FRAME, 1, RECORD_BYTES_PER_FRAME, 2, 16, 0)   # 12: signed, packed
    queue = ctypes.c_void_p()
    toolbox.AudioQueueEnqueueBuffer.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    toolbox.AudioQueueNewInput.argtypes = [ctypes.POINTER(StreamFormat), callback_type, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                           ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]
    toolbox.AudioQueueSetProperty.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32]
    toolbox.AudioQueueAllocateBuffer.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]
    toolbox.AudioQueueStart.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    toolbox.AudioQueueStop.argtypes = [ctypes.c_void_p, ctypes.c_bool]
    status = toolbox.AudioQueueNewInput(ctypes.byref(wanted), callback, None, None, None, 0, ctypes.byref(queue))
    status = status or toolbox.AudioQueueSetProperty(queue, four_cc("aqcd"), ctypes.byref(uid), ctypes.sizeof(uid))
    for _ in range(RECORD_BUFFERS):
        buffer = ctypes.c_void_p()
        status = status or toolbox.AudioQueueAllocateBuffer(queue, RECORD_BUFFER_BYTES, ctypes.byref(buffer))
        status = status or toolbox.AudioQueueEnqueueBuffer(queue, buffer, 0, None)
    status = status or toolbox.AudioQueueStart(queue, None)
    if status:
        sys.exit(f"CoreAudio error {status} starting the recording")
    time.sleep(float(seconds))
    toolbox.AudioQueueStop(queue, True)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(RECORD_RATE))
        w.writeframes(b"".join(frames))
    if gaps[0]:
        sys.exit(f"the recording has {gaps[0]} gaps: its hit positions are not valid")


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
