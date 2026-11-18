#!/usr/bin/env python3
"""Minimal mock motor controller on a SocketCAN interface.

Listens for gateway commands on CAN id 1002 and replies with motor
status frames on CAN id 1001, mimicking the original STM32 gateway test.
"""
import argparse
import fcntl
import socket
import struct
import sys
import time

CAN_RAW = 1
SIOCGIFINDEX = 0x8933
CAN_SFF_MASK = 0x7FF
CAN_ID_MOTOR = 1001
CAN_ID_GATEWAY = 1002

# struct can_frame: can_id, dlc, pad, res0, res1, data[8]
FRAME_FMT = "=IBBBB8s"


def ifindex(name: str) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        ifr = struct.pack("16sH", name.encode(), 0)
        res = fcntl.ioctl(s.fileno(), SIOCGIFINDEX, ifr)
        return struct.unpack("16sH", res)[1]


def pack_frame(can_id: int, data: bytes) -> bytes:
    data = data[:8]
    return struct.pack(FRAME_FMT, can_id & CAN_SFF_MASK, len(data), 0, 0, 0,
                       data.ljust(8, b"\x00"))


def unpack_frame(raw: bytes):
    can_id, dlc, _pad, _r0, _r1, data = struct.unpack(FRAME_FMT, raw[:16])
    return can_id & CAN_SFF_MASK, data[:dlc]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--can", default="vcan0")
    args = parser.parse_args()

    sock = socket.socket(socket.PF_CAN, socket.SOCK_RAW, CAN_RAW)
    idx = ifindex(args.can)
    sock.bind((args.can, idx))

    print(f"mock motor listening on {args.can}, rx id={CAN_ID_GATEWAY}, "
          f"tx id={CAN_ID_MOTOR}", flush=True)

    while True:
        raw = sock.recv(16)
        can_id, data = unpack_frame(raw)
        if can_id != CAN_ID_GATEWAY or len(data) < 4:
            continue

        target = (data[0] << 8) | data[1]
        speed = (data[2] << 8) | data[3]
        print(f"command: target={target} mm, max_speed={speed}", flush=True)
        if target == 0 or speed == 0:
            continue

        current = 0
        step = max(1, speed)
        while current < target:
            current = min(target, current + step)
            payload = struct.pack(">HH", current, 1)
            sock.send(pack_frame(CAN_ID_MOTOR, payload))
            time.sleep(0.05)

        sock.send(pack_frame(CAN_ID_MOTOR, struct.pack(">HH", current, 2)))
        print("motor stopped", flush=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
