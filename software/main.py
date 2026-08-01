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
"""

import argparse
import datetime
import sys
import time

import serial

DEFAULT_PORT = "COM1"

SOF = 0xA5
STATUS_OK = 0x00

# Command IDs (must match usb_cmd.h).
CMD_PING = 0x01
CMD_SET_TIME = 0x10
CMD_GET_TIME = 0x11
CMD_SET_LED = 0x20

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


def txn(ser, cmd, payload=b""):
    ser.reset_input_buffer()
    ser.write(build_frame(cmd, payload))
    return read_frame(ser)


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
