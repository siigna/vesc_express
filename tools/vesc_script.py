#!/usr/bin/env python3
"""Talk to a VESC Express board over serial: upload scripts, run them, watch output.

Exists because VESC Tool cannot always be used -- a build of it may be missing
its firmware resources, and on some boards the only port is a USB-to-UART
bridge that needs careful handling. Everything awkward about that is handled
here rather than rediscovered:

  - Opening the port can reset the board. DTR and RTS drive BOOT and EN on
    these bridges, and pyserial asserts them on open with some drivers. A tool
    that then sends a command half a second later is talking to a chip still
    in its bootloader, which looks exactly like a board that has stopped
    answering. So the reset is done deliberately and followed by a real wait.

  - The protocol has two frame formats: a short one led by 0x02 with a single
    length byte, and a long one led by 0x03 with a 16-bit length. A parser
    that only understands the short form silently misses long replies, which
    is enough to conclude the board said nothing.

  - Script output arrives within milliseconds of the run command, so anything
    that waits for a specific reply has to keep the output it passes over
    rather than discarding it.

  - A container built by luapack.py is uploaded verbatim. Re-wrapping it the
    way a source file is wrapped would nest one container inside another and
    reset the language flag to LispBM.

Usage:
    vesc_script.py PORT ping
    vesc_script.py PORT upload main.luapkg
    vesc_script.py PORT run | stop | erase
    vesc_script.py PORT listen [seconds]
    vesc_script.py PORT console [seconds]
    vesc_script.py PORT stats
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial, "
             "or nix-shell -p python3Packages.pyserial")

# Command ids, from bldc/datatypes.h. They are named COMM_LISP_* for history;
# nothing about them is lisp-specific and a Lua build answers the same ones.
COMM_FW_VERSION = 0
COMM_LISP_WRITE_CODE = 131
COMM_LISP_ERASE_CODE = 132
COMM_LISP_SET_RUNNING = 133
COMM_LISP_GET_STATS = 134
COMM_LISP_PRINT = 135
COMM_LISP_REPL_CMD = 138

CHUNK = 384          # What VESC Tool uses; the firmware accepts it happily.

# Rates to probe when none is given, in the order they are tried. 115200 is
# every board's default and comes first so the common case costs nothing; the
# rest are what COMM_UART_BAUD is plausibly set to on a board where a 217 KB
# package upload at 115200 was the bottleneck.
BAUD_CANDIDATES = [115200, 921600, 460800, 1500000, 2000000]
BOOT_WAIT = 3.0      # Time from reset to the firmware answering packets.

_TAB = []
for _i in range(256):
    _c = _i << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x1021) & 0xFFFF if _c & 0x8000 else (_c << 1) & 0xFFFF
    _TAB.append(_c)


def crc16(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _TAB[((crc >> 8) ^ b) & 0xFF]
    return crc


def frame(payload: bytes) -> bytes:
    if len(payload) <= 255:
        head = bytes([2, len(payload)])
    else:
        head = bytes([3]) + struct.pack(">H", len(payload))
    return head + payload + struct.pack(">H", crc16(payload)) + bytes([3])


def unframe(buf: bytes):
    """Yield payloads, skipping anything that is not a valid frame.

    Console text and packets can share one wire, so junk between frames is
    normal rather than an error.
    """
    i = 0
    while i < len(buf):
        if buf[i] == 2 and i + 1 < len(buf):
            n = buf[i + 1]
            end = i + 2 + n + 3
            if end <= len(buf) and buf[end - 1] == 3:
                yield buf[i + 2:i + 2 + n]
                i = end
                continue
        elif buf[i] == 3 and i + 2 < len(buf):
            n = struct.unpack(">H", buf[i + 1:i + 3])[0]
            end = i + 3 + n + 3
            if end <= len(buf) and buf[end - 1] == 3:
                yield buf[i + 3:i + 3 + n]
                i = end
                continue
        i += 1


class Board:
    def __init__(self, port, baud=115200, reset=True):
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = baud
        self.s.timeout = 0.2
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.s.dtr = False
        self.s.rts = False
        self.prints = []
        if reset:
            self.reset()

    def set_baud(self, baud):
        """Change rate on an open port, without touching the reset lines.

        Re-opening would pulse DTR/RTS and reset the board, which is the whole
        reason probing has to work this way: a reset per candidate would cost
        three seconds each and lose any output in between.
        """
        self.s.baudrate = baud
        self.s.reset_input_buffer()

    def reset(self):
        """Pulse EN, then wait for the firmware to be ready."""
        self.s.setDTR(False)     # BOOT released: run the app, not the ROM loader
        self.s.setRTS(True)      # EN low
        time.sleep(0.1)
        self.s.setRTS(False)
        time.sleep(BOOT_WAIT)
        self.s.reset_input_buffer()

    def send(self, payload):
        self.s.write(frame(payload))
        self.s.flush()

    def collect(self, secs):
        """Read for a while, sorting script output from everything else."""
        buf = b""
        others = []
        end = time.time() + secs
        while time.time() < end:
            buf += self.s.read(1024) or b""
        for p in unframe(buf):
            if not p:
                continue
            if p[0] == COMM_LISP_PRINT:
                self.prints.append(p[1:].split(b"\0")[0].decode("utf-8", "replace"))
            else:
                others.append(p)
        return others

    def request(self, payload, want, secs=8.0):
        """Send, then wait for one reply id, keeping any output seen on the way."""
        self.send(payload)
        buf = b""
        end = time.time() + secs
        while time.time() < end:
            buf += self.s.read(1024) or b""
            found = None
            for p in unframe(buf):
                if not p:
                    continue
                if p[0] == COMM_LISP_PRINT:
                    line = p[1:].split(b"\0")[0].decode("utf-8", "replace")
                    if line not in self.prints:
                        self.prints.append(line)
                elif p[0] == want and found is None:
                    found = p
            if found is not None:
                return found
        return None

    def close(self):
        self.s.close()


def probe_baud(b, candidates=None):
    """Find the rate the board is talking at, by asking it its version.

    A board built with -DCOMM_UART_BAUD=921600 is silent at 115200 and vice
    versa, and the failure looks identical to a board that is not running:
    "no reply". Probing turns that into an answer rather than a flag the user
    has to remember matching to a build.

    Returns the rate that answered, or None. The port is left at whatever rate
    worked, so the caller can carry on using it.
    """
    for baud in (candidates or BAUD_CANDIDATES):
        b.set_baud(baud)
        if b.request(bytes([COMM_FW_VERSION]), COMM_FW_VERSION, 2):
            return baud

    return None


def cmd_ping(b, args):
    r = b.request(bytes([COMM_FW_VERSION]), COMM_FW_VERSION, 4)
    if not r or len(r) < 4:
        print("no reply")
        return 1
    name = r[3:].split(b"\0")[0].decode("ascii", "replace")
    print("firmware %d.%02d   hw %s" % (r[1], r[2], name))
    return 0


def cmd_erase(b, args, size=None):
    n = size if size is not None else 16
    r = b.request(bytes([COMM_LISP_ERASE_CODE]) + struct.pack(">i", n),
                  COMM_LISP_ERASE_CODE, 12)
    ok = bool(r and len(r) >= 2 and r[1] == 1)
    print("erase: %s" % ("ok" if ok else "failed"))
    return 0 if ok else 1


def cmd_upload(b, args):
    blob = open(args.file, "rb").read()

    # A container carries its language in bit 0 of the flags word at offset 6.
    if len(blob) > 8:
        flags = struct.unpack(">H", blob[6:8])[0]
        print("container: %d bytes, %s" %
              (len(blob), "Lua" if flags & 1 else "LispBM"))

    b.request(bytes([COMM_LISP_SET_RUNNING, 0]), COMM_LISP_SET_RUNNING, 4)
    time.sleep(0.2)

    if cmd_erase(b, args, size=len(blob) + 100) != 0:
        return 1

    off = 0
    while off < len(blob):
        part = blob[off:off + CHUNK]
        for _ in range(5):
            r = b.request(bytes([COMM_LISP_WRITE_CODE]) + struct.pack(">I", off) + part,
                          COMM_LISP_WRITE_CODE, 3)
            if r and len(r) >= 2 and r[1] == 1:
                break
        else:
            print("write failed at offset %d" % off)
            return 1
        off += len(part)
    print("uploaded %d bytes" % len(blob))

    if not args.no_run:
        time.sleep(0.3)
        b.request(bytes([COMM_LISP_SET_RUNNING, 1]), COMM_LISP_SET_RUNNING, 4)
        print("running")
        b.collect(args.seconds)
        show_prints(b)
    return 0


def cmd_run(b, args):
    r = b.request(bytes([COMM_LISP_SET_RUNNING, 1]), COMM_LISP_SET_RUNNING, 4)
    print("run: %s" % ("ok" if r else "no reply"))
    b.collect(args.seconds)
    show_prints(b)
    return 0


def cmd_stop(b, args):
    r = b.request(bytes([COMM_LISP_SET_RUNNING, 0]), COMM_LISP_SET_RUNNING, 4)
    print("stop: %s" % ("ok" if r else "no reply"))
    return 0


def cmd_listen(b, args):
    b.collect(args.seconds)
    show_prints(b)
    return 0


def cmd_stats(b, args):
    r = b.request(bytes([COMM_LISP_GET_STATS, 1]), COMM_LISP_GET_STATS, 5)
    if not r or len(r) < 10:
        print("no stats reply -- the engine is not running")
        return 1
    cpu, a, c = struct.unpack(">hhh", r[1:7])
    print("cpu %.2f%%  mem %.2f%%  heap %.2f%%" % (cpu / 100.0, a / 100.0, c / 100.0))
    rest = r[10:]
    while rest:
        end = rest.find(b"\0")
        if end <= 0 or len(rest) < end + 2:
            break
        name = rest[:end].decode("ascii", "replace")
        print("  %s" % name)
        rest = rest[end + 1:]
        rest = rest[4:] if len(rest) >= 4 else b""
    return 0


def cmd_repl(b, args):
    """Evaluate one expression on the board.

    The firmware rate-limits this to one command every 0.5 s and silently
    ignores anything sooner, so the wait below is not politeness -- without it
    a second command disappears with no error.
    """
    expr = args.file
    if not expr:
        print("repl needs an expression, e.g. repl '(bl-set 1)'")
        return 1

    time.sleep(0.6)
    b.send(bytes([COMM_LISP_REPL_CMD]) + expr.encode("utf-8"))
    b.collect(args.seconds)
    show_prints(b)
    return 0


def cmd_console(b, args):
    """Raw console text, for a build whose console is on this port.

    Resets and reads from the moment the chip is released, because the
    interesting output -- the bootloader, PSRAM, and whatever the firmware
    says about starting up -- is all in the first second. The generic reset
    waits for boot and then flushes, which is right for talking packets and
    exactly wrong here.
    """
    b.s.setDTR(False)
    b.s.setRTS(True)
    time.sleep(0.1)
    b.s.reset_input_buffer()
    b.s.setRTS(False)

    buf = b""
    end = time.time() + args.seconds
    while time.time() < end:
        buf += b.s.read(1024) or b""
    sys.stdout.write("".join(
        chr(c) if 32 <= c < 127 or c == 10 else "." for c in buf))
    print()
    return 0


def show_prints(b):
    print("--- script output:")
    for line in b.prints:
        print("  " + line)
    if not b.prints:
        print("  (none)")
        print("  Note: script output is delivered over whichever comm port last")
        print("  received a packet. A script printing at boot, before anything")
        print("  connected, prints into nowhere -- use a console build to see it.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("action", choices=["ping", "upload", "run", "stop", "erase",
                                       "listen", "stats", "console", "repl"])
    ap.add_argument("file", nargs="?",
                    help="container for upload, or the expression for repl")
    ap.add_argument("-b", "--baud", type=int, default=None,
                    help="skip probing and use this rate. Without it the "
                         "common rates are tried, starting at 115200.")
    ap.add_argument("-s", "--seconds", type=float, default=8.0,
                    help="how long to listen for output")
    ap.add_argument("--no-reset", action="store_true",
                    help="do not reset on connect (see the note in this file)")
    ap.add_argument("--no-run", action="store_true",
                    help="upload without starting the script")
    args = ap.parse_args()

    if args.action == "upload" and not args.file:
        ap.error("upload needs a container file (build one with luapack.py)")

    # console does its own reset so it can capture the boot; everything else
    # wants the board already up before it speaks.
    do_reset = not args.no_reset and args.action != "console"
    b = Board(args.port, args.baud or BAUD_CANDIDATES[0], reset=do_reset)
    try:
        # With no -b, find the rate rather than assuming one. Skipped for
        # console, which is raw text and has no request to probe with, and for
        # an explicit -b, which is the user saying they already know.
        if args.baud is None and args.action != "console":
            found = probe_baud(b)
            if found is None:
                print("no reply at any of %s"
                      % ", ".join(str(x) for x in BAUD_CANDIDATES))
                return 1
            if found != BAUD_CANDIDATES[0]:
                print("board is at %d baud" % found)
        return {
            "ping": cmd_ping, "upload": cmd_upload, "run": cmd_run,
            "stop": cmd_stop, "erase": cmd_erase, "listen": cmd_listen,
            "stats": cmd_stats, "console": cmd_console, "repl": cmd_repl,
        }[args.action](b, args)
    finally:
        b.close()


if __name__ == "__main__":
    sys.exit(main())
