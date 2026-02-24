# ttxd — Implementation Documentation

## Overview

ttxd is a single-channel DVB teletext acquisition service written in C.
It connects to an HDHomeRun network tuner using a plain TCP socket,
extracts the teletext elementary stream from the MPEG Transport Stream,
decodes teletext pages using libzvbi, and emits one JSON object per
complete page over UDP to Node-RED or any other consumer on the same
machine.

Dependencies are intentionally minimal: libzvbi and libc only.

---

## Architecture

```
HDHomeRun device
  │
  │  HTTP/1.1 over TCP port 80
  ▼
http_connect()                    (plain TCP socket + minimal HTTP GET)
  │
  │  raw bytes from recv(), arbitrary chunk size
  ▼
process_chunk()                   (TS alignment, carry buffer)
  │
  │  188-byte TS packets, filtered by PID
  ▼
process_ts_packet()               (header parse, adaptation field skip)
  │
  │  PES payload bytes
  ▼
PES reassembler                   (g_pes[], g_pes_target)
  │
  │  complete PES packets
  ▼
dispatch_pes()
  │
  │  PES data payload (past header)
  ▼
feed_pes_data() → vbi_dvb_demux_cor()    (libzvbi DVB demuxer)
  │
  │  vbi_sliced[]  (line-sliced VBI data)
  ▼
vbi_decode()                      (libzvbi teletext decoder)
  │
  │  VBI_EVENT_TTX_PAGE event per complete page
  ▼
ttx_event_cb()
  │
  │  vbi_fetch_vt_page() → vbi_page (40×25 Unicode grid)
  ▼
JSON serialiser
  │
  │  UDP datagram  →  127.0.0.1:<port>
  ▼
Node-RED udp-in node
```

---

## Dependencies

| Library   | Ubuntu package  | Purpose                                 |
|-----------|-----------------|-----------------------------------------|
| libzvbi   | `libzvbi-dev`   | DVB demux, teletext decode, page export |
| libc      | (system)        | sockets, signal handling, time          |

Tested against libzvbi 0.2.41 (Ubuntu 22.04 / 24.04).

`ffmpeg` is used once at setup time to identify the teletext PID. It
is not linked against and plays no role at runtime.

---

## Data Flow in Detail

### 1. HTTP Connection — http_connect()

The HDHomeRun exposes each channel as a raw MPEG-TS stream over plain
HTTP (no HTTPS, no authentication):

```
http://<hdhomerun-ip>/auto/v<channel>
```

`http_connect()` implements the minimum required to open this stream:

1. `socket()` + `connect()` to the HDHomeRun IP on port 80.
2. `send()` an HTTP/1.1 GET request with `Connection: close`.
3. `recv()` in a loop accumulating response bytes into a header buffer
   (up to 4096 bytes) until the `\r\n\r\n` header terminator is found.
4. Checks that the HTTP status code is 200. Any other status (404, 503
   etc.) causes an immediate close and retry.
5. Any body bytes already read into the header buffer beyond the
   `\r\n\r\n` boundary are immediately passed to `process_chunk()`.
6. Returns the open socket fd.

This replaces libcurl entirely. The HDHomeRun HTTP implementation is
straightforward and does not use chunked transfer encoding, compression,
redirects, or authentication, so a minimal implementation is sufficient.

The stream is open-ended — the HDHomeRun sends MPEG-TS bytes
continuously until the connection is closed. `recv()` blocks waiting for
data, which is the correct behaviour for a streaming source.

### 2. TS Packet Alignment — process_chunk()

`recv()` returns chunks of arbitrary size with no relationship to the
188-byte MPEG-TS packet boundary. `process_chunk()` re-aligns the stream
using a carry buffer (`g_carry[]`, `g_carry_len`):

1. If bytes are waiting in the carry buffer from the previous call, fill
   it to 188 bytes from the start of the new chunk and process the
   completed packet.
2. Process complete 188-byte packets directly from the chunk pointer,
   advancing by 188 bytes each iteration.
3. Copy any remaining bytes (0–187) into the carry buffer for the next
   call.

No heap allocation occurs. The carry buffer is a static 188-byte array.

### 3. TS Packet Processing — process_ts_packet()

Each 188-byte packet is inspected at the transport layer:

- Byte 0 must be `0x47` (sync byte). Misaligned or corrupt packets are
  silently dropped.
- The transport error indicator (bit 7 of byte 1) causes the packet to
  be dropped.
- The PID is extracted from bits 12–0 of bytes 1–2. Packets not
  matching `g_pid` are discarded immediately — this is the only
  filtering performed.
- The payload_unit_start_indicator (PUSI, bit 6 of byte 1) signals
  the start of a new PES packet.
- If an adaptation field is present (bit 5 of byte 3), its length is
  read from byte 4 and the field is skipped. The payload begins
  immediately after.

### 4. PES Reassembly

A single PES (Packetised Elementary Stream) packet carrying teletext
data is typically spread across multiple TS packets. The reassembler
accumulates payload bytes from successive TS packets into `g_pes[]`:

- When PUSI is set, the previously accumulated PES is dispatched via
  `dispatch_pes()` and accumulation restarts with the new packet.
- The expected total PES length is read from bytes 4–5 of the PES
  header (`PES_packet_length`). When non-zero, the PES is dispatched
  as soon as `g_pes_len >= g_pes_target` (6 + PES_packet_length),
  without waiting for the next PUSI. This is more correct for bounded
  PES packets.
- If the accumulation buffer would overflow (> 65548 bytes), the state
  is reset and an error is logged. This should not occur with valid
  teletext streams.

### 5. PES Dispatch — dispatch_pes()

Validates the three-byte PES start code (`0x00 0x00 0x01`). Reads
`PES_header_data_length` from byte 8 to calculate the offset of the
data payload (9 + `PES_header_data_length`). Passes the payload slice
to `feed_pes_data()`.

### 6. DVB Demuxing — feed_pes_data() → vbi_dvb_demux_cor()

The PES data payload contains a data_identifier byte followed by
data units in the DVB EBU format (ETSI EN 301 775). This is passed to
`vbi_dvb_demux_cor()` which:

- Parses the DVB EBU data units.
- Performs bit-slicing and Hamming 8/4 error correction on each
  teletext byte.
- Returns an array of `vbi_sliced` structures, one per decoded VBI
  line, with an associated PTS timestamp.

The function is called in a loop until all bytes in the PES payload
have been consumed or no progress is made.

### 7. Teletext Decoding — vbi_decode() → ttx_event_cb()

`vbi_decode()` runs the teletext page assembly state machine. A
teletext page arrives as 26 rows (row 0 = header, rows 1–25 = body,
row 26 = enhancement packets) broadcast in sequence over many seconds.
libzvbi accumulates rows until a page is complete, then fires a
`VBI_EVENT_TTX_PAGE` event via the registered callback `ttx_event_cb()`.

The event delivers:
- `ev->ev.ttx_page.pgno` — page number (100–899 decimal BCD)
- `ev->ev.ttx_page.subno` — subpage number (0 for single-subpage pages)

`vbi_fetch_vt_page()` retrieves the decoded page as a `vbi_page`
structure. `VBI_WST_LEVEL_1p5` enables Level 1.5 enhanced character
sets (national character variants beyond basic Latin). The `rows`
parameter is 25 (full page). The `reset` flag `TRUE` clears navigation
link tracking which is not needed here.

### 8. Page Content Export

The `vbi_page.text[]` array holds `vbi_char` elements in row-major
order: `text[row * columns + col]`. Typical dimensions are 40 × 25.

`vbi_char.unicode` contains the Unicode codepoint for each cell. The
following are mapped to space (U+0020) before output:

- Codepoints below U+0020 (C0 control characters used by teletext
  internally for colour and display attributes)
- U+00AD (soft hyphen)
- Codepoints ≥ U+EE00 (libzvbi private range for mosaic and block
  graphics characters that have no Unicode equivalent)

The resulting string per row is encoded to UTF-8 using a small inline
encoder (`utf8_encode()`) and trailing spaces are stripped.

### 9. JSON Serialisation

Each page is serialised into a static 8 KB buffer:

```json
{
  "page":    100,
  "subpage": 0,
  "ts":      1708789200,
  "lines":   ["row 0 text", "row 1 text", ..., "row 24 text"]
}
```

- `page` — decimal page number
- `subpage` — decimal subpage number (0 for single-subpage pages)
- `ts` — Unix timestamp in seconds at time of callback
- `lines` — 25 strings, row 0 first. Row 0 is the page header on all
  standard broadcasters (contains page number and clock). Strings are
  UTF-8, trailing-space stripped, JSON-escaped via `json_escape()`.

A newline is appended as a record delimiter. The maximum possible
teletext content (25 rows × 40 chars × 3 bytes UTF-8) is under 4 KB,
well within the 8 KB buffer.

### 10. UDP Transmission

The JSON string is sent as a single datagram via `sendto()` to
`127.0.0.1` on the configured port. On loopback, packet loss and IP
fragmentation are not practical concerns. Datagram size is well within
the 65507-byte UDP payload limit.

### 11. Reconnect Loop

`recv()` returns 0 (connection closed by server) or negative (network
error) when the stream ends. This causes the inner stream loop to exit.
The outer reconnect loop waits `RECONNECT_DELAY` seconds (5) then calls
`http_connect()` again.

On each reconnection:
- The carry buffer and PES accumulation state are zeroed.
- The libzvbi demuxer and decoder are destroyed and recreated via
  `zvbi_init()`. This is necessary to clear the page assembly state
  machine — otherwise rows buffered from the previous connection could
  combine with rows from the new one and produce corrupt pages.

---

## Signal Handling

`SIGINT` and `SIGTERM` set `g_running = 0`. The inner `recv()` loop
checks `g_running` on each iteration and exits cleanly. `SIGPIPE` is
ignored to prevent the process being killed if a UDP write fails.

---

## Global State Summary

| Variable        | Type                 | Purpose                                      |
|-----------------|----------------------|----------------------------------------------|
| `g_demux`       | `vbi_dvb_demux *`    | libzvbi DVB demultiplexer instance           |
| `g_dec`         | `vbi_decoder *`      | libzvbi teletext decoder instance            |
| `g_udp_fd`      | `int`                | UDP socket file descriptor                   |
| `g_udp_dest`    | `struct sockaddr_in` | UDP destination address (127.0.0.1:<port>)   |
| `g_pid`         | `int`                | Target teletext PID                          |
| `g_running`     | `volatile int`       | Set to 0 by signal handler to stop loops     |
| `g_carry[]`     | `uint8_t[188]`       | TS alignment carry buffer                    |
| `g_carry_len`   | `int`                | Bytes currently in carry buffer              |
| `g_pes[]`       | `uint8_t[65548]`     | PES accumulation buffer                      |
| `g_pes_len`     | `int`                | Bytes currently in PES buffer                |
| `g_pes_target`  | `int`                | Expected total PES size (0 = wait for PUSI)  |

---

## Known Limitations

- **Single channel only.** One process instance per channel by design.
  Run multiple instances on different UDP ports for multiple channels.

- **PID must be known in advance.** The service does not parse PAT/PMT
  to auto-discover the teletext PID. Use `ffprobe` once per channel.

- **No subpage aggregation.** Each subpage fires a separate callback and
  produces a separate UDP datagram. Pages with rotating subpages will
  each appear independently. Aggregate in Node-RED if needed.

- **Timestamp is wall clock, not PTS.** The `ts` field is system time
  at callback time, not the stream PTS. PTS is available from libzvbi
  but not currently included in the JSON output.

- **No page filtering.** All decoded pages 100–899 are emitted.
  Filter by `msg.payload.page` in Node-RED for specific pages.

- **libzvbi version.** `vbi_dvb_demux_cor()` returns `unsigned int`
  (line count) in libzvbi ≥ 0.2.35. Ubuntu 22.04 and 24.04 ship
  0.2.41 — no issue.

---

## Source Files

| File                | Purpose                                  |
|---------------------|------------------------------------------|
| `ttxd.c`            | Full C source, single compilation unit   |
| `Makefile`          | Build rules using pkg-config             |
| `ttxd.service`      | systemd unit file                        |
| `SETUP.md`          | Installation and operational guide       |
| `IMPLEMENTATION.md` | This document                            |
