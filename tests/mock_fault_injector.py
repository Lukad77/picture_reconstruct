#!/usr/bin/env python3
"""Lightweight fault-injection harness for the reliable transfer plan.

Usage examples:
  python3 tests/mock_fault_injector.py receiver --port 9001
  python3 tests/mock_fault_injector.py sender --host 127.0.0.1 --port 9001 --frames 3
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from typing import Iterable


INFO = "[MOCK][INFO]"
FAULT = "[MOCK][FAULT]"
ASSERT = "[MOCK][ASSERT]"


def emit(msg: str, prefix: str = INFO) -> None:
    print(f"{prefix} {msg}")


def build_fake_frame(frame_id: int, payload_size: int = 4096) -> bytes:
    payload = bytes((frame_id + i) % 256 for i in range(payload_size))
    return struct.pack("<Q", frame_id) + payload


def run_receiver(args: argparse.Namespace) -> int:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(1)
    emit(f"listening on {args.host}:{args.port}")

    conn, addr = server.accept()
    emit(f"accepted connection from {addr}")

    while True:
        try:
            data = conn.recv(1024)
        except socket.timeout:
            emit("recv timeout", prefix=FAULT)
            break

        if not data:
            emit("client disconnect")
            break

        if args.disconnect_after_frame is not None and b"frame_id" in data:
            # Very lightweight simulation of a forced disconnect after a specific frame.
            frame_marker = data.split(b"frame_id")
            if len(frame_marker) > 1:
                try:
                    frame_num = int(frame_marker[1].split(b"\x00", 1)[0])
                except ValueError:
                    frame_num = -1
                if frame_num >= args.disconnect_after_frame:
                    emit(f"forcing disconnect after frame {frame_num}", prefix=FAULT)
                    conn.close()
                    server.close()
                    return 0

        if args.drop_ack:
            emit(f"drop_ack={args.drop_ack} enabled; not sending ACK", prefix=FAULT)
            conn.close()
            server.close()
            return 0

        if args.echo:
            conn.sendall(data)

    conn.close()
    server.close()
    return 0


def run_sender(args: argparse.Namespace) -> int:
    emit(f"sending {args.frames} fake frames to {args.host}:{args.port}")
    for frame_id in range(args.frames):
        payload = build_fake_frame(frame_id, payload_size=args.payload_size)
        with socket.create_connection((args.host, args.port), timeout=3.0) as sock:
            sock.sendall(payload)
            emit(f"sent frame {frame_id}")
            if args.delay_ms:
                time.sleep(args.delay_ms / 1000.0)
            if args.disconnect_mid_frame and frame_id == args.disconnect_mid_frame:
                emit(f"disconnecting mid-frame {frame_id}", prefix=FAULT)
                break
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Fault injection for reliable transfer tests")
    subparsers = parser.add_subparsers(dest="mode", required=True)

    receiver = subparsers.add_parser("receiver", help="simulate a receiver that disconnects or drops ACKs")
    receiver.add_argument("--host", default="127.0.0.1")
    receiver.add_argument("--port", type=int, default=9001)
    receiver.add_argument("--disconnect-after-frame", type=int, default=None)
    receiver.add_argument("--drop-ack", action="store_true")
    receiver.add_argument("--echo", action="store_true")

    sender = subparsers.add_parser("sender", help="send synthetic frame payloads")
    sender.add_argument("--host", default="127.0.0.1")
    sender.add_argument("--port", type=int, default=9001)
    sender.add_argument("--frames", type=int, default=3)
    sender.add_argument("--payload-size", type=int, default=4096)
    sender.add_argument("--delay-ms", type=int, default=0)
    sender.add_argument("--disconnect-mid-frame", type=int, default=None)

    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.mode == "receiver":
        return run_receiver(args)
    if args.mode == "sender":
        return run_sender(args)

    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
