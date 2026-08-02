"""Hoppy Clock USB CDC test tool.

Frame: [0xA5][cmd][len][payload...][crc8]   crc8 poly 0x07 over cmd+len+payload.
Response echoes cmd; payload[0] is a status byte (0 = OK).

Usage:
  python main.py [-p PORT] COMMAND [args...]

Examples:
  python main.py ping                       # Ping the device (default port).
  python main.py -p COM3 ping               # Ping on a specific port.
  python main.py -p /dev/ttyACM0 get-time   # Read the clock (Linux port).
  python main.py set-time                   # Sync device to local "now".
  python main.py set-led 0 60 0 0           # LED index 0 -> red (r g b, 0-255).
  python main.py set-led 0 green            # LED index 0 -> named colour.

  # End-to-end alarm test (fires ~1 min out, plays sound id 0):
  python main.py upload-sound 0 --tone 880 --seconds 1 --gain 0.05
  python main.py set-alarm --in 1 --sound 0 --brightness 10 --ramp 10 --timeout 20
  python main.py list-alarms

  # Upload a WAV (converted to 8-bit 8 kHz mono) or a raw .u8 file:
  python main.py upload-sound 2 chime.wav
  python main.py set-alarm --at 06:30 --days weekdays --sound 2 --brightness 200
"""

import argparse
import datetime
import math
import os
import struct
import sys
import time
import wave
import zlib

import serial

DEFAULT_PORT = "COM1"

SOF = 0xA5
STATUS_OK = 0x00

# Command IDs (must match usb_cmd.h).
CMD_PING = 0x01
CMD_SET_TIME = 0x10
CMD_GET_TIME = 0x11
CMD_SET_LED = 0x20
CMD_CFG_BEGIN = 0x30
CMD_CFG_SET_ALARM = 0x31
CMD_CFG_COMMIT = 0x32
CMD_CFG_GET_COUNT = 0x33
CMD_CFG_GET_ALARM = 0x34
CMD_SND_BEGIN = 0x40
CMD_SND_DATA = 0x41
CMD_SND_END = 0x42
CMD_SND_INFO = 0x43

# Alarm record (must match manifest.h): 12 bytes, little-endian.
ALARM_STRUCT = "<BBBBBHHBBB"  # flags,day,h,m,s,ramp,timeout,sound,bright,rsv.
ALARM_FLAG_ENABLED = 0x01
ALARM_FLAG_MONTHLY = 0x02
DAY_BITS = {
    "mon": 0,
    "tue": 1,
    "wed": 2,
    "thu": 3,
    "fri": 4,
    "sat": 5,
    "sun": 6,
}
DAY_GROUPS = {
    "daily": "mon,tue,wed,thu,fri,sat,sun",
    "weekdays": "mon,tue,wed,thu,fri",
    "weekends": "sat,sun",
}

SND_FORMAT_PCM_U8 = 0x00
SND_RATE_HZ = 8000  # Firmware plays PCM_U8; 8 kHz is the design rate.
SND_CHUNK = 32  # USB payload cap (USB_CMD_MAX_PAYLOAD).

# Convenience colour names for `set-led`.
COLORS = {
    "warm": (40, 40, 30),
    "red": (60, 0, 0),
    "green": (0, 60, 0),
    "blue": (0, 0, 60),
    "white": (60, 60, 60),
    "off": (0, 0, 0),
}


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (
                ((crc << 1) ^ 0x07) & 0xFF
                if (crc & 0x80)
                else (crc << 1) & 0xFF
            )
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = bytes([cmd, len(payload)]) + payload
    return bytes([SOF]) + body + bytes([crc8(body)])


def read_frame(ser, timeout=1.0):
    """Sync on SOF and read one full, CRC-checked frame.

    Returns:
        (cmd, payload) or None.
    """
    end = time.time() + timeout
    # Find SOF.
    while time.time() < end:
        b = ser.read(1)
        if b and b[0] == SOF:
            break
    else:
        return None
    hdr = ser.read(2)  # (cmd, len).
    if len(hdr) < 2:
        return None
    cmd, length = hdr[0], hdr[1]
    payload = ser.read(length)
    crc = ser.read(1)
    if len(payload) != length or len(crc) != 1:
        return None
    if crc[0] != crc8(bytes([cmd, length]) + payload):
        print("  ! bad CRC")
        return None
    return cmd, payload


def txn(ser, cmd, payload=b"", timeout=1.0):
    ser.reset_input_buffer()
    ser.write(build_frame(cmd, payload))
    return read_frame(ser, timeout)


def ok(payload) -> bool:
    return len(payload) >= 1 and payload[0] == STATUS_OK


def _status(r) -> str:
    return "OK" if r and ok(r[1]) else f"FAIL ({r})"


def _time_payload(now: datetime.datetime) -> bytes:
    return bytes(
        [
            now.year % 100,
            now.month,
            now.day,
            now.isoweekday(),
            now.hour,
            now.minute,
            now.second,
        ]
    )


# Command handlers: each takes the open serial port and parsed args.


def cmd_ping(ser, args):
    r = txn(ser, CMD_PING)
    print("PING     ->", _status(r))
    return 0 if r and ok(r[1]) else 1


def cmd_set_time(ser, args):
    r = txn(ser, CMD_SET_TIME, _time_payload(datetime.datetime.now()))
    print("SET_TIME ->", _status(r))
    return 0 if r and ok(r[1]) else 1


def cmd_get_time(ser, args):
    r = txn(ser, CMD_GET_TIME)
    if r and ok(r[1]) and len(r[1]) >= 8:
        yr, mo, dd, wd, hh, mm, ss = r[1][1:8]
        print(
            f"GET_TIME -> 20{yr:02d}-{mo:02d}-{dd:02d} (wd {wd}) "
            f"{hh:02d}:{mm:02d}:{ss:02d}"
        )
        return 0
    print("GET_TIME ->", _status(r))
    return 1


def cmd_set_led(ser, args):
    vals = args.rgb
    if len(vals) == 1:  # Colour name, e.g. `set-led 0 red`.
        name = vals[0].lower()
        if name not in COLORS:
            print(
                f"unknown colour '{name}'; choose from {', '.join(COLORS)}",
                file=sys.stderr,
            )
            return 2
        rgb = COLORS[name]
    elif len(vals) == 3:  # Explicit r g b, e.g. `set-led 0 60 0 0`.
        try:
            rgb = tuple(int(x) for x in vals)
        except ValueError:
            print("r g b must be integers 0-255", file=sys.stderr)
            return 2
        if any(not 0 <= c <= 255 for c in rgb):
            print("r g b must be in range 0-255", file=sys.stderr)
            return 2
    else:
        print(
            "set-led needs either a colour name or three r g b values",
            file=sys.stderr,
        )
        return 2
    r = txn(ser, CMD_SET_LED, bytes([args.index, *rgb]))
    print(f"SET_LED  {args.index} {tuple(rgb)} ->", _status(r))
    return 0 if r and ok(r[1]) else 1


# Alarm config


def _parse_days(spec: str) -> int:
    """Turn a day spec ("weekdays", "mon,wed,fri") into a weekday bit mask."""
    spec = DAY_GROUPS.get(spec.lower(), spec)
    mask = 0
    for name in spec.split(","):
        name = name.strip().lower()
        if name not in DAY_BITS:
            raise ValueError(f"unknown day '{name}'")
        mask |= 1 << DAY_BITS[name]
    return mask


def _build_alarm(args) -> bytes:
    """Build a 12-byte alarm record from parsed set-alarm args."""
    if args.in_minutes is not None:
        target = datetime.datetime.now() + datetime.timedelta(
            minutes=args.in_minutes
        )
        hh, mm, ss = target.hour, target.minute, target.second
        day_sel = 1 << (target.isoweekday() - 1)  # Mon=1 -> bit 0.
        monthly = False
    else:
        parts = args.at.split(":")
        hh, mm = int(parts[0]), int(parts[1])
        ss = int(parts[2]) if len(parts) > 2 else 0
        if args.dom is not None:
            day_sel, monthly = args.dom, True
        else:
            day_sel, monthly = _parse_days(args.days), False

    flags = ALARM_FLAG_ENABLED | (ALARM_FLAG_MONTHLY if monthly else 0)
    return struct.pack(
        ALARM_STRUCT,
        flags,
        day_sel,
        hh,
        mm,
        ss,
        args.ramp,
        args.timeout,
        args.sound,
        args.brightness,
        0,
    )


def cmd_set_alarm(ser, args):
    if args.in_minutes is None and args.at is None:
        print("set-alarm needs --in MINUTES or --at HH:MM", file=sys.stderr)
        return 2
    if args.at is not None and args.days is None and args.dom is None:
        print("--at needs --days or --dom", file=sys.stderr)
        return 2

    try:
        record = _build_alarm(args)
    except (ValueError, IndexError) as e:
        print(f"bad alarm spec: {e}", file=sys.stderr)
        return 2

    # Replace the whole table with this single alarm (begin / set / commit).
    for label, r in (
        ("BEGIN", txn(ser, CMD_CFG_BEGIN)),
        ("SET", txn(ser, CMD_CFG_SET_ALARM, bytes([0]) + record)),
        ("COMMIT", txn(ser, CMD_CFG_COMMIT, bytes([1]), timeout=5.0)),
    ):
        if not (r and ok(r[1])):
            print(f"CFG_{label} ->", _status(r))
            return 1
    print("set-alarm -> OK (1 alarm committed)")
    return 0


def cmd_list_alarms(ser, args):
    r = txn(ser, CMD_CFG_GET_COUNT)
    if not (r and ok(r[1]) and len(r[1]) >= 2):
        print("GET_COUNT ->", _status(r))
        return 1

    count = r[1][1]
    print(f"{count} alarm(s):")
    for i in range(count):
        r = txn(ser, CMD_CFG_GET_ALARM, bytes([i]))
        if not (r and ok(r[1]) and len(r[1]) >= 13):
            print(f"  [{i}] ->", _status(r))
            continue
        flags, day, hh, mm, ss, ramp, timeout, sound, bright, _rsv = (
            struct.unpack(ALARM_STRUCT, r[1][1:13])
        )
        if flags & ALARM_FLAG_MONTHLY:
            when = f"day-of-month {day}"
        else:
            days = ",".join(n for n, b in DAY_BITS.items() if day & (1 << b))
            when = days or "(no days)"
        state = "on " if flags & ALARM_FLAG_ENABLED else "off"
        print(
            f"  [{i}] {state} {hh:02d}:{mm:02d}:{ss:02d} {when} "
            f"sound={sound} ramp={ramp}s timeout={timeout}s bright={bright}"
        )
    return 0


# --- Sound assets ------------------------------------------------------------


def _resample(samples, src_rate, dst_rate):
    """Linear-interpolate a list of floats from src_rate to dst_rate."""
    if src_rate == dst_rate or not samples:
        return samples
    n_out = int(len(samples) * dst_rate / src_rate)
    out = []
    for i in range(n_out):
        pos = i * src_rate / dst_rate
        i0 = int(pos)
        frac = pos - i0
        s0 = samples[i0]
        s1 = samples[i0 + 1] if i0 + 1 < len(samples) else s0
        out.append(s0 + (s1 - s0) * frac)
    return out


def _to_u8(samples, gain=1.0) -> bytes:
    """Map floats in [-1, 1] to unsigned 8-bit PCM (128 = silence).

    gain scales amplitude about mid-scale (volume): 1.0 = as-is, 0.5 = half.
    """
    return bytes(max(0, min(255, round(s * 127 * gain) + 128)) for s in samples)


def _scale_u8(data: bytes, gain: float) -> bytes:
    """Apply a volume gain to already-8-bit PCM, about mid-scale (128)."""
    if gain == 1.0:
        return data
    return bytes(max(0, min(255, round((b - 128) * gain) + 128)) for b in data)


def _wav_to_u8(path, dst_rate, gain=1.0) -> bytes:
    with wave.open(path, "rb") as w:
        ch, sw, sr = w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(w.getnframes())

    if sw == 1:  # WAV 8-bit is unsigned.
        vals = [(b - 128) / 128.0 for b in raw]
    elif sw == 2:  # 16-bit signed little-endian.
        n = len(raw) // 2
        vals = [v / 32768.0 for v in struct.unpack(f"<{n}h", raw[: n * 2])]
    else:
        raise ValueError(
            f"unsupported WAV width {sw * 8}-bit; use 8- or 16-bit PCM"
        )

    if ch > 1:  # Downmix to mono by averaging channels.
        vals = [sum(vals[i : i + ch]) / ch for i in range(0, len(vals), ch)]
    return _to_u8(_resample(vals, sr, dst_rate), gain)


def _load_sound(args) -> bytes:
    if args.file:
        ext = os.path.splitext(args.file)[1].lower()
        if ext in (".u8", ".raw", ".pcm"):  # Already 8-bit mono at the rate.
            with open(args.file, "rb") as f:
                return _scale_u8(f.read(), args.gain)
        return _wav_to_u8(args.file, args.rate, args.gain)
    # Synthesized test tone.
    n = int(args.seconds * args.rate)
    return _to_u8(
        [math.sin(2 * math.pi * args.tone * i / args.rate) for i in range(n)],
        args.gain,
    )


def cmd_upload_sound(ser, args):
    if not args.file and args.tone is None:
        print("upload-sound needs a FILE or --tone HZ", file=sys.stderr)
        return 2
    try:
        data = _load_sound(args)
    except (OSError, ValueError, wave.Error) as e:
        print(f"could not load sound: {e}", file=sys.stderr)
        return 2

    total = len(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(f"uploading sound {args.id}: {total} bytes @ {args.rate} Hz")

    begin = struct.pack("<BBHI", args.id, SND_FORMAT_PCM_U8, args.rate, total)
    r = txn(ser, CMD_SND_BEGIN, begin, timeout=5.0)
    if not (r and ok(r[1])):
        print("SND_BEGIN ->", _status(r))
        return 1

    for off in range(0, total, SND_CHUNK):
        r = txn(ser, CMD_SND_DATA, data[off : off + SND_CHUNK])
        if not (r and ok(r[1])):
            print(f"SND_DATA @ {off} ->", _status(r))
            return 1
        if off % (SND_CHUNK * 64) == 0:
            print(f"  {off}/{total}", end="\r", flush=True)

    r = txn(ser, CMD_SND_END, struct.pack("<I", crc), timeout=5.0)
    print(" " * 24, end="\r")  # Clear the progress line.
    print("upload-sound ->", _status(r))
    return 0 if r and ok(r[1]) else 1


def cmd_sound_info(ser, args):
    r = txn(ser, CMD_SND_INFO, bytes([args.id]))
    if r and ok(r[1]) and len(r[1]) >= 12:
        _st, fmt, rate, length, crc = struct.unpack("<BBHII", r[1][:12])
        print(
            f"sound {args.id}: format={fmt} rate={rate} "
            f"length={length} crc=0x{crc:08X}"
        )
        return 0
    print("SND_INFO ->", _status(r))
    return 1


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Hoppy Clock USB CDC test tool.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "-p",
        "--port",
        default=DEFAULT_PORT,
        help=f"serial (CDC) port (default: {DEFAULT_PORT})",
    )
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("ping", help="ping the device").set_defaults(func=cmd_ping)
    sub.add_parser("set-time", help="set the RTC to local now").set_defaults(
        func=cmd_set_time
    )
    sub.add_parser("get-time", help="read the RTC back").set_defaults(
        func=cmd_get_time
    )

    led = sub.add_parser("set-led", help="set an LED colour")
    led.add_argument("index", type=int, help="LED index")
    led.add_argument(
        "rgb",
        nargs="*",
        metavar="COLOUR|R G B",
        help="a colour name (e.g. red) or three values r g b (0-255)",
    )
    led.set_defaults(func=cmd_set_led)

    al = sub.add_parser(
        "set-alarm", help="replace the alarm table with one alarm"
    )
    al.add_argument(
        "--in",
        dest="in_minutes",
        type=int,
        help="fire this many minutes from now (weekly, today)",
    )
    al.add_argument("--at", help="fire time HH:MM[:SS]")
    al.add_argument("--days", help="weekly days: mon,tue,... or weekdays/daily")
    al.add_argument("--dom", type=int, help="monthly day-of-month (1-31)")
    al.add_argument("--sound", type=int, default=0, help="sound id (default 0)")
    al.add_argument("--ramp", type=int, default=0, help="LED ramp seconds")
    al.add_argument(
        "--timeout", type=int, default=60, help="auto-quiet seconds (0 = never)"
    )
    al.add_argument(
        "--brightness", type=int, default=128, help="LED target 0-255"
    )
    al.set_defaults(func=cmd_set_alarm)

    sub.add_parser(
        "list-alarms", help="read back the alarm table"
    ).set_defaults(func=cmd_list_alarms)

    up = sub.add_parser(
        "upload-sound", help="store a sound (WAV/raw file or --tone)"
    )
    up.add_argument("id", type=int, help="sound id")
    up.add_argument("file", nargs="?", help="WAV, or raw .u8/.pcm at --rate")
    up.add_argument(
        "--tone", type=float, help="synthesize a sine of HZ instead"
    )
    up.add_argument(
        "--seconds", type=float, default=2.0, help="tone length (default 2)"
    )
    up.add_argument(
        "--rate",
        type=int,
        default=SND_RATE_HZ,
        help=f"sample rate (default {SND_RATE_HZ})",
    )
    up.add_argument(
        "--gain",
        type=float,
        default=1.0,
        help="volume 0.0-1.0 (default 1.0 = full scale)",
    )
    up.set_defaults(func=cmd_upload_sound)

    si = sub.add_parser("sound-info", help="query a stored sound")
    si.add_argument("id", type=int, help="sound id")
    si.set_defaults(func=cmd_sound_info)
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    try:
        ser = serial.Serial(args.port, 115200, timeout=0.2)
    except serial.SerialException as e:
        print(f"could not open {args.port}: {e}", file=sys.stderr)
        return 1

    with ser:
        time.sleep(0.2)  # Let the CDC link settle.
        return args.func(ser, args)


if __name__ == "__main__":
    sys.exit(main())
