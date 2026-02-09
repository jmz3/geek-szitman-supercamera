#!/usr/bin/env python3

import argparse
import socket
import struct
import sys
import time

import cv2
import numpy as np

STREAM_MAGIC = 0x47535643  # GSVC
STREAM_VERSION = 1
STREAM_CODEC_JPEG = 1
HEADER_FORMAT = "!IBBHHHIQI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MAX_PAYLOAD_SIZE = 1024 * 1024


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining > 0:
        data = sock.recv(remaining)
        if not data:
            raise ConnectionError("peer closed the connection")
        chunks.append(data)
        remaining -= len(data)
    return b"".join(chunks)


def parse_header(raw_header: bytes) -> tuple[int, int, int, int]:
    magic, version, codec, _flags, source_id, _reserved, frame_id, timestamp_us, payload_size = struct.unpack(
        HEADER_FORMAT, raw_header
    )

    if magic != STREAM_MAGIC:
        raise ValueError(f"bad magic: 0x{magic:08x}")
    if version != STREAM_VERSION:
        raise ValueError(f"unsupported version: {version}")
    if codec != STREAM_CODEC_JPEG:
        raise ValueError(f"unsupported codec: {codec}")
    if payload_size > MAX_PAYLOAD_SIZE:
        raise ValueError(f"payload too large: {payload_size}")

    return source_id, frame_id, timestamp_us, payload_size


def run_receiver(host: str, port: int, timeout: float, log_every: int, window_name: str) -> int:
    frame_count = 0
    start = time.time()

    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(None)
        print(f"Connected to {host}:{port}")

        while True:
            raw_header = recv_exact(sock, HEADER_SIZE)
            source_id, frame_id, timestamp_us, payload_size = parse_header(raw_header)
            payload = recv_exact(sock, payload_size)

            jpeg = np.frombuffer(payload, dtype=np.uint8)
            image = cv2.imdecode(jpeg, cv2.IMREAD_COLOR)
            if image is None:
                print(f"Warning: failed to decode JPEG source_id={source_id} frame_id={frame_id}")
                continue

            current_window = f"{window_name} [source {source_id}]"
            cv2.imshow(current_window, image)
            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):
                break

            frame_count += 1
            if log_every > 0 and frame_count % log_every == 0:
                elapsed = max(time.time() - start, 1e-6)
                fps = frame_count / elapsed
                print(
                    f"frames={frame_count} fps={fps:.2f} "
                    f"last_source={source_id} last_frame_id={frame_id} timestamp_us={timestamp_us}"
                )

    cv2.destroyAllWindows()
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Supercamera TCP stream receiver")
    parser.add_argument("--host", default="127.0.0.1", help="Sender host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9000, help="Sender port (default: 9000)")
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="Connection timeout in seconds (default: 5.0)",
    )
    parser.add_argument(
        "--log-every",
        type=int,
        default=120,
        help="Print stats every N displayed frames (default: 120)",
    )
    parser.add_argument(
        "--window-name",
        default="Supercamera TCP Receiver",
        help="OpenCV window base name",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        return run_receiver(
            host=args.host,
            port=args.port,
            timeout=args.timeout,
            log_every=args.log_every,
            window_name=args.window_name,
        )
    except KeyboardInterrupt:
        cv2.destroyAllWindows()
        print("\nInterrupted")
        return 0
    except (ConnectionError, OSError, ValueError) as exc:
        cv2.destroyAllWindows()
        print(f"Receiver error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
