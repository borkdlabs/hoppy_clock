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
  python main.py set-led-count 12           # Set LED count to 12.

  # Define light looks (effect + params) and the lamp idle:
  python main.py set-light 0 --effect solid --color warm --brightness 1
  python main.py set-light 1 --effect solid --color warm --brightness 5 --curve flicker
  python main.py set-lamp --on 1 --off 0         # off idle = dim ambient.

  # Animated effects (loop until the ring/lamp ends):
  python main.py set-light 3 --effect rainbow --period 4000 --spread 24 --brightness 5
  python main.py set-light 4 --effect breathe --color blue --period 3000 --brightness 5

  # Sounds default to 16-bit PCM @ 16 kHz (2 slots, ~4 min each):
  python main.py upload-sound 0 --tone 880 --seconds 1 --gain 0.05
  python main.py upload-sound 1 song.mp3 --gain 0.6  # needs ffmpeg
  python main.py upload-sound 1 song.mp3 --gain 0.6 --trim 60  # first 60s
  python main.py sound-info 1
  python main.py play-sound 1                     # play it now
  python main.py stop-sound                       # stop playback

  # End-to-end alarm test (fires ~1 min out, sunrise look + song):
  python main.py set-light 2 --effect solid --color amber --brightness 200 --period 10000
  python main.py set-alarm --in 1 --sound 1 --light 2 --timeout 60
  python main.py list-alarms
"""

import argparse
import datetime
import math
import os
import shutil
import struct
import subprocess
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
CMD_CFG_SET_LIGHT = 0x35
CMD_CFG_SET_LAMP = 0x36
CMD_CFG_GET_LIGHT = 0x37
CMD_CFG_SET_LEDS = 0x38
CMD_SND_BEGIN = 0x40
CMD_SND_DATA = 0x41
CMD_SND_END = 0x42
CMD_SND_INFO = 0x43
CMD_SND_PLAY = 0x44
CMD_SND_STOP = 0x45

# Alarm record (must match manifest.h): 12 bytes, little-endian.
# flags, day, h, m, s, timeout, sound, light, reserved(3).
ALARM_STRUCT = "<BBBBBHBBBBB"
ALARM_FLAG_ENABLED = 0x01
ALARM_FLAG_MONTHLY = 0x02

# Light sequence (must match manifest.h): 12 bytes, little-endian.
# effect, r, g, b, brightness, period_ms, curve, spread, reserved(3).
LIGHT_STRUCT = "<BBBBBHBBBBB"
LIGHT_FX = {"solid": 0, "rainbow": 1, "sweep": 2, "breathe": 3}
LIGHT_CURVES = {"linear": 0, "ease": 1, "flicker": 2}
# Base colours for light looks (full-intensity hues; brightness scales them).
BASE_COLORS = {
    "warm": (255, 200, 120),
    "white": (255, 255, 255),
    "red": (255, 0, 0),
    "green": (0, 255, 0),
    "blue": (0, 0, 255),
    "amber": (255, 140, 0),
}
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

# Audio formats (must match sound.h).
SND_FORMAT_PCM_U8 = 0x00
SND_FORMAT_PCM_S16 = 0x01
SND_FORMATS = {"s16": SND_FORMAT_PCM_S16, "u8": SND_FORMAT_PCM_U8}
SND_BYTES_PER_SAMPLE = {SND_FORMAT_PCM_U8: 1, SND_FORMAT_PCM_S16: 2}
SND_RATE_HZ = 16000  # Default rate; 16-bit PCM @ 16 kHz is the song target.
SND_CHUNK = 64  # Data chunk size; must be <= firmware USB_CMD_MAX_PAYLOAD.
SND_SLOT_BYTES = 7680 * 1024  # Per-sound flash slot (firmware SOUND_SLOT_SIZE).

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


def _parse_color(vals) -> tuple:
    """Parse a colour: a name, or three 0-255 ints. Default warm."""
    if not vals:
        return BASE_COLORS["warm"]
    if len(vals) == 1:
        name = vals[0].lower()
        if name not in BASE_COLORS:
            raise ValueError(f"unknown colour '{name}'")
        return BASE_COLORS[name]
    if len(vals) == 3:
        rgb = tuple(int(x) for x in vals)
        if any(not 0 <= c <= 255 for c in rgb):
            raise ValueError("r g b must be 0-255")
        return rgb
    raise ValueError("colour must be a name or three r g b values")


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
        args.timeout,
        args.sound,
        args.light,
        0,
        0,
        0,
    )


# The manifest is one atomic image, so every editing command reads the whole
# config, changes one part, and writes it back (a bare BEGIN/COMMIT would wipe
# the parts it did not resend).


def _read_config(ser):
    """Fetch (alarms, lights, lamp_on/off, led_count) or None on failure."""
    r = txn(ser, CMD_CFG_GET_COUNT)
    if not (r and ok(r[1]) and len(r[1]) >= 6):
        print("GET_COUNT ->", _status(r))
        return None
    n_alarm, n_light = r[1][1], r[1][2]
    lamp_on, lamp_off, led_count = r[1][3], r[1][4], r[1][5]

    alarms = []
    for i in range(n_alarm):
        r = txn(ser, CMD_CFG_GET_ALARM, bytes([i]))
        if not (r and ok(r[1]) and len(r[1]) >= 13):
            print(f"GET_ALARM {i} ->", _status(r))
            return None
        alarms.append(bytes(r[1][1:13]))

    lights = []
    for i in range(n_light):
        r = txn(ser, CMD_CFG_GET_LIGHT, bytes([i]))
        if not (r and ok(r[1]) and len(r[1]) >= 13):
            print(f"GET_LIGHT {i} ->", _status(r))
            return None
        lights.append(bytes(r[1][1:13]))

    return {
        "alarms": alarms,
        "lights": lights,
        "lamp_on": lamp_on,
        "lamp_off": lamp_off,
        "led_count": led_count,
    }


def _write_config(ser, cfg) -> bool:
    """Push a whole config back atomically (begin / set* / commit)."""
    r = txn(ser, CMD_CFG_BEGIN)
    if not (r and ok(r[1])):
        print("CFG_BEGIN ->", _status(r))
        return False
    for i, rec in enumerate(cfg["alarms"]):
        r = txn(ser, CMD_CFG_SET_ALARM, bytes([i]) + rec)
        if not (r and ok(r[1])):
            print(f"CFG_SET_ALARM {i} ->", _status(r))
            return False
    for i, seq in enumerate(cfg["lights"]):
        r = txn(ser, CMD_CFG_SET_LIGHT, bytes([i]) + seq)
        if not (r and ok(r[1])):
            print(f"CFG_SET_LIGHT {i} ->", _status(r))
            return False
    r = txn(ser, CMD_CFG_SET_LAMP, bytes([cfg["lamp_on"], cfg["lamp_off"]]))
    if not (r and ok(r[1])):
        print("CFG_SET_LAMP ->", _status(r))
        return False
    r = txn(ser, CMD_CFG_SET_LEDS, bytes([cfg["led_count"]]))
    if not (r and ok(r[1])):
        print("CFG_SET_LEDS ->", _status(r))
        return False
    r = txn(
        ser,
        CMD_CFG_COMMIT,
        bytes([len(cfg["alarms"]), len(cfg["lights"])]),
        timeout=5.0,
    )
    if not (r and ok(r[1])):
        print("CFG_COMMIT ->", _status(r))
        return False
    return True


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

    cfg = _read_config(ser)
    if cfg is None:
        return 1
    cfg["alarms"] = [record]  # Replace the table with this one alarm.
    if not _write_config(ser, cfg):
        return 1
    print("set-alarm -> OK (1 alarm, lights/lamp preserved)")
    return 0


def cmd_set_light(ser, args):
    try:
        r, g, b = _parse_color(args.color)
        effect = LIGHT_FX[args.effect]
        curve = LIGHT_CURVES[args.curve]
    except (ValueError, KeyError) as e:
        print(f"bad light spec: {e}", file=sys.stderr)
        return 2

    seq = struct.pack(
        LIGHT_STRUCT,
        effect,
        r,
        g,
        b,
        args.brightness,
        args.period,
        curve,
        args.spread,
        0,
        0,
        0,
    )
    cfg = _read_config(ser)
    if cfg is None:
        return 1
    off = struct.pack(LIGHT_STRUCT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    while len(cfg["lights"]) <= args.id:  # Grow with blanks up to the id.
        cfg["lights"].append(off)
    cfg["lights"][args.id] = seq
    if not _write_config(ser, cfg):
        return 1
    print(f"set-light -> OK (light {args.id}, {args.effect})")
    return 0


def cmd_set_lamp(ser, args):
    if args.on is None and args.off is None:
        print("set-lamp needs --on ID and/or --off ID", file=sys.stderr)
        return 2
    cfg = _read_config(ser)
    if cfg is None:
        return 1
    if args.on is not None:
        cfg["lamp_on"] = args.on
    if args.off is not None:
        cfg["lamp_off"] = args.off
    if not _write_config(ser, cfg):
        return 1
    print(f"set-lamp -> OK (on={cfg['lamp_on']} off={cfg['lamp_off']})")
    return 0


def cmd_set_led_count(ser, args):
    cfg = _read_config(ser)
    if cfg is None:
        return 1
    cfg["led_count"] = args.count
    if not _write_config(ser, cfg):
        return 1
    print(f"set-led-count -> OK ({args.count} LEDs)")
    return 0


def cmd_list_alarms(ser, args):
    cfg = _read_config(ser)
    if cfg is None:
        return 1

    print(f"{len(cfg['alarms'])} alarm(s):")
    for i, rec in enumerate(cfg["alarms"]):
        flags, day, hh, mm, ss, timeout, sound, light, *_ = struct.unpack(
            ALARM_STRUCT, rec
        )
        if flags & ALARM_FLAG_MONTHLY:
            when = f"day-of-month {day}"
        else:
            days = ",".join(n for n, b in DAY_BITS.items() if day & (1 << b))
            when = days or "(no days)"
        state = "on " if flags & ALARM_FLAG_ENABLED else "off"
        print(
            f"  [{i}] {state} {hh:02d}:{mm:02d}:{ss:02d} {when} "
            f"sound={sound} light={light} timeout={timeout}s"
        )

    fx = {v: k for k, v in LIGHT_FX.items()}
    curves = {v: k for k, v in LIGHT_CURVES.items()}
    print(f"{len(cfg['lights'])} light(s):")
    for i, seq in enumerate(cfg["lights"]):
        effect, r, g, b, bright, period, curve, spread, *_ = struct.unpack(
            LIGHT_STRUCT, seq
        )
        name = fx.get(effect, effect)
        extra = f"{curves.get(curve, curve)} " if name == "solid" else ""
        print(
            f"  [{i}] {name} rgb=({r},{g},{b}) bright={bright} "
            f"{period}ms {extra}spread={spread}"
        )
    print(f"lamp: on=light {cfg['lamp_on']}  off=light {cfg['lamp_off']}")
    print(f"led-count: {cfg['led_count']}")
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


def _to_s16(samples, gain=1.0) -> bytes:
    """Map floats in [-1, 1] to signed 16-bit little-endian PCM."""
    vals = [max(-32768, min(32767, round(s * 32767 * gain))) for s in samples]
    return struct.pack(f"<{len(vals)}h", *vals)


def _scale_s16(data: bytes, gain: float) -> bytes:
    """Apply a volume gain to already-16-bit signed PCM."""
    if gain == 1.0:
        return data
    n = len(data) // 2
    vals = [
        max(-32768, min(32767, round(v * gain)))
        for v in struct.unpack(f"<{n}h", data[: n * 2])
    ]
    return struct.pack(f"<{n}h", *vals)


def _encode(samples, gain, fmt) -> bytes:
    """Encode float samples to the target PCM format."""
    if fmt == SND_FORMAT_PCM_S16:
        return _to_s16(samples, gain)
    return _to_u8(samples, gain)


def _scale_pcm(data: bytes, gain: float, fmt: int) -> bytes:
    """Apply a volume gain to already-encoded PCM of the given format."""
    if fmt == SND_FORMAT_PCM_S16:
        return _scale_s16(data, gain)
    return _scale_u8(data, gain)


def _wav_to_pcm(path, dst_rate, gain, fmt) -> bytes:
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
    return _encode(_resample(vals, sr, dst_rate), gain, fmt)


def _ffmpeg_to_pcm(path, dst_rate, gain, fmt) -> bytes:
    """Decode any ffmpeg-readable file (mp3, ogg, flac, m4a, ...) to mono PCM
    at dst_rate in the target format -- exactly what the firmware plays."""
    exe = shutil.which("ffmpeg")
    if not exe:
        raise RuntimeError(
            "ffmpeg not found; install it (https://ffmpeg.org) or convert to "
            "a WAV first"
        )
    codec = "s16le" if fmt == SND_FORMAT_PCM_S16 else "u8"
    cmd = [exe, "-v", "error", "-i", path, "-ac", "1", "-ar", str(dst_rate)]
    if gain != 1.0:
        cmd += ["-af", f"volume={gain}"]  # ffmpeg applies gain (fast).
    cmd += ["-f", codec, "-"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0 or not proc.stdout:
        msg = proc.stderr.decode(errors="replace").strip()
        raise RuntimeError(msg or "ffmpeg failed to decode the file")
    return proc.stdout


def _load_sound(args):
    """Return (pcm_bytes, format_code). Raw file extensions pin the format."""
    fmt = SND_FORMATS[args.format]
    if args.file:
        ext = os.path.splitext(args.file)[1].lower()
        if ext == ".u8":  # Raw unsigned 8-bit at --rate.
            with open(args.file, "rb") as f:
                return _scale_u8(f.read(), args.gain), SND_FORMAT_PCM_U8
        if ext == ".s16":  # Raw signed 16-bit LE at --rate.
            with open(args.file, "rb") as f:
                return _scale_s16(f.read(), args.gain), SND_FORMAT_PCM_S16
        if ext in (".raw", ".pcm"):  # Raw bytes in the chosen --format.
            with open(args.file, "rb") as f:
                return _scale_pcm(f.read(), args.gain, fmt), fmt
        if ext == ".wav":  # Decoded in-process, no external tools.
            return _wav_to_pcm(args.file, args.rate, args.gain, fmt), fmt
        # mp3 and other compressed formats: decode via ffmpeg.
        return _ffmpeg_to_pcm(args.file, args.rate, args.gain, fmt), fmt
    # Synthesized test tone.
    n = int(args.seconds * args.rate)
    samples = [
        math.sin(2 * math.pi * args.tone * i / args.rate) for i in range(n)
    ]
    return _encode(samples, args.gain, fmt), fmt


def cmd_upload_sound(ser, args):
    if not args.file and args.tone is None:
        print("upload-sound needs a FILE or --tone HZ", file=sys.stderr)
        return 2
    try:
        data, fmt = _load_sound(args)
    except (OSError, ValueError, wave.Error, RuntimeError) as e:
        print(f"could not load sound: {e}", file=sys.stderr)
        return 2

    bps = SND_BYTES_PER_SAMPLE[fmt]  # Bytes per sample (u8 = 1, s16 = 2).
    rate_bytes = args.rate * bps  # Bytes per second at this rate/format.

    if args.trim is not None:  # Keep only the first N seconds (whole samples).
        data = data[: max(bps, int(args.trim * args.rate) * bps)]

    total = len(data)
    if total > SND_SLOT_BYTES:
        limit_s = SND_SLOT_BYTES / rate_bytes
        print(
            f"sound is {total / rate_bytes:.1f}s ({total} bytes); the per-sound "
            f"limit is {limit_s:.1f}s ({SND_SLOT_BYTES} bytes at {args.rate} "
            f"Hz, {args.format}). Use --trim SECONDS or shorten the file.",
            file=sys.stderr,
        )
        return 2

    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(
        f"uploading sound {args.id}: {total} bytes "
        f"({total / rate_bytes:.1f}s, {args.format} @ {args.rate} Hz)"
    )

    # SND_BEGIN erases the slot in 64 KB blocks (tBE2 ~2 s each), so scale the
    # wait to the erase size with margin.
    blocks = (total + 65535) // 65536
    begin_timeout = max(10.0, blocks * 2.5)
    begin = struct.pack("<BBHI", args.id, fmt, args.rate, total)
    r = txn(ser, CMD_SND_BEGIN, begin, timeout=begin_timeout)
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
        names = {v: k for k, v in SND_FORMATS.items()}
        bps = SND_BYTES_PER_SAMPLE.get(fmt, 1)
        secs = length / (rate * bps) if rate else 0
        print(
            f"sound {args.id}: format={names.get(fmt, fmt)} rate={rate} "
            f"length={length}B ({secs:.1f}s) crc=0x{crc:08X}"
        )
        return 0
    print("SND_INFO ->", _status(r))
    return 1


def cmd_play_sound(ser, args):
    r = txn(ser, CMD_SND_PLAY, bytes([args.id]))
    print(f"play-sound {args.id} ->", _status(r))
    return 0 if r and ok(r[1]) else 1


def cmd_stop_sound(ser, args):
    r = txn(ser, CMD_SND_STOP)
    print("stop-sound ->", _status(r))
    return 0 if r and ok(r[1]) else 1


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
    al.add_argument("--light", type=int, default=0, help="light id (default 0)")
    al.add_argument(
        "--timeout", type=int, default=60, help="auto-quiet seconds (0 = never)"
    )
    al.set_defaults(func=cmd_set_alarm)

    lt = sub.add_parser("set-light", help="define a light look (id)")
    lt.add_argument("id", type=int, help="light id")
    lt.add_argument(
        "--effect",
        choices=list(LIGHT_FX),
        default="solid",
        help="solid, rainbow, sweep, or breathe",
    )
    lt.add_argument(
        "--color",
        nargs="*",
        default=[],
        metavar="NAME|R G B",
        help="base colour name or r g b (default warm)",
    )
    lt.add_argument(
        "--brightness", type=int, default=160, help="master level 0-255"
    )
    lt.add_argument(
        "--period",
        type=int,
        default=1500,
        help="solid: fade ms; animated: cycle ms",
    )
    lt.add_argument(
        "--curve",
        choices=list(LIGHT_CURVES),
        default="linear",
        help="solid ramp shape: linear, ease, flicker",
    )
    lt.add_argument(
        "--spread",
        type=int,
        default=40,
        help="flicker amplitude / rainbow hue-step / sweep width",
    )
    lt.set_defaults(func=cmd_set_light)

    lp = sub.add_parser("set-lamp", help="set the lamp on/off idle light ids")
    lp.add_argument("--on", type=int, help="light id for lamp-on idle")
    lp.add_argument("--off", type=int, help="light id for lamp-off idle")
    lp.set_defaults(func=cmd_set_lamp)

    lc = sub.add_parser(
        "set-led-count", help="set the number of LEDs in the chain"
    )
    lc.add_argument(
        "count", type=int, help="active LED count (1..LED_COUNT_MAX)"
    )
    lc.set_defaults(func=cmd_set_led_count)

    sub.add_parser(
        "list-alarms", help="read back alarms, lights, lamp, led-count"
    ).set_defaults(func=cmd_list_alarms)

    up = sub.add_parser(
        "upload-sound", help="store a sound (WAV/MP3/raw file or --tone)"
    )
    up.add_argument("id", type=int, help="sound id")
    up.add_argument(
        "file",
        nargs="?",
        help="WAV, MP3/etc (needs ffmpeg), or raw .u8/.s16/.pcm at --rate",
    )
    up.add_argument(
        "--format",
        choices=list(SND_FORMATS),
        default="s16",
        help="s16 = 16-bit quality (default), u8 = 8-bit half-size",
    )
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
    up.add_argument(
        "--trim",
        type=float,
        help=f"keep only the first N seconds (~{SND_SLOT_BYTES // (SND_RATE_HZ * 2)}s max at 16-bit)",
    )
    up.set_defaults(func=cmd_upload_sound)

    si = sub.add_parser("sound-info", help="query a stored sound")
    si.add_argument("id", type=int, help="sound id")
    si.set_defaults(func=cmd_sound_info)

    ps = sub.add_parser("play-sound", help="play a stored sound now")
    ps.add_argument("id", type=int, help="sound id")
    ps.set_defaults(func=cmd_play_sound)

    sub.add_parser("stop-sound", help="stop playback").set_defaults(
        func=cmd_stop_sound
    )
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
