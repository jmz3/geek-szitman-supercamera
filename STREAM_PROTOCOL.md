# Supercamera Stream Protocol (v1)

This document specifies the TCP on-wire format used by `out_stream_sender`.
It supports multiplexing frames from multiple USB cameras on one TCP connection.

## Transport

- Current implementation: TCP only.
- `udp` is accepted as a CLI value but intentionally rejected as not implemented.

## Frame message layout

Each message is one fixed-size header followed by JPEG payload bytes.
All integer fields are serialized in network byte order (big-endian).

### Header (28 bytes)

1. `uint32_t magic` = `0x47535643` (`GSVC`)
2. `uint8_t version` = `1`
3. `uint8_t codec` = `1` (JPEG)
4. `uint16_t flags` = `0` (reserved)
5. `uint16_t source_id` (USB camera index on sender)
6. `uint16_t reserved` = `0`
7. `uint32_t frame_id` (per-source frame sequence)
8. `uint64_t timestamp_us` (microseconds since Unix epoch)
9. `uint32_t payload_size`

### Payload

- `payload_size` bytes of JPEG data immediately follow the header.

## Receiver parsing rules

1. Read exactly 28 bytes for the header.
2. Validate `magic`, `version`, and `codec`.
3. Validate `payload_size <= 1048576` (1 MiB).
4. Read exactly `payload_size` bytes for the JPEG payload.
5. Decode payload as JPEG.

If validation fails, close the connection or resynchronize according to the receiver's policy.

## Sender behavior notes

- Sender accepts one TCP client at a time.
- Capture continues even when no client is connected.
- For each source/camera, the sender keeps only the latest frame in memory; stale unsent frames are overwritten.
