# Httpstream to decoded teletekst
## Please Note: This i swork in progress, when up and running this deposit will be updated

DVB teletext acquisition service for Linux. Reads a live MPEG Transport
Stream from an [HDHomeRun](https://www.silicondust.com/) network tuner,
decodes teletext pages, and emits one JSON object per page over UDP —
designed to feed [Node-RED](https://nodered.org/) or any other local
consumer.

[HDHomeRun DVB tuners](https://shop.silicondust.com/shop/product-category/dvb/?scrollto=737097)

```
HDHomeRun  →  HTTP/TCP  →  ttxd  →  UDP 127.0.0.1  →  Node-RED
```

## Features

- Single C source file, ~370 lines
- Two dependencies only: **libzvbi** and **libc**
- No ffmpeg, no libcurl, **no external tools at runtime**
- Minimal HTTP/1.1 client using plain TCP sockets
- Full MPEG-TS demux and PES reassembly built in
- Teletext decoded per ETSI 300 706 via libzvbi
- One UDP datagram per complete teletext page
- Automatic reconnection if the stream drops
- Runs as a hardened systemd service

> NEED TO ADD: When called, request stream, start service. Drop and stop after fullrun

## Requirements

- Linux (tested on Ubuntu 22.04 / 24.04)
- HDHomeRun network tuner on the same LAN
- `libzvbi` (≥ 0.2.35)
- `gcc` and `make`
- `ffmpeg` — **once only**, for teletext PID discovery (not a runtime dependency)

## Quick Start

### 1. Install build dependencies

```bash
sudo apt install libzvbi-dev build-essential
```

### 2. Fix your HDHomeRun IP
Assign fixed IP in DHCP IP-binding table of router

### 3. Find the teletext PID for your channel
First use VLC (on whatever system): Easiest and then you know the TT-stream is available.


> Media → Open Network Stream → http://192.168.1.50/auto/v21
> Then: Tools → Media Information → Codec


It lists all elementary streams including their PIDs. You are looking for a stream described as "Teletext" or "DVB Teletext".

Then ffprobe on Linux, Download from https://gyan.dev/ffmpeg/builds.

```bash
sudo apt install ffmpeg
ffprobe http://192.168.1.50/auto/v<channel> 2>&1 | grep -i teletext
# optional — remove after use
sudo apt remove ffmpeg
```

Comes also in WIndows version if you want to keep the Linuxbox squeakyclean (the essential build is enough): just extract, and run from PowerShell or cmd (Identical output to the Linux version) This is probably the cleanest option since it gives you the exact same hex PID string you would get on Linux.
```powershell
ffprobe.exe http://192.168.1.50/auto/v21 2>&1 | findstr /i teletext
```
> Look for a line like: Stream #0:3[0x199]: Subtitle: dvb_teletext

```bash
# Convert the hex PID to decimal: `0x199` = 409
printf '%d\n' 0x199
```

### 4. Build
```bash
mkdir -p ~/ttxd
cd ~/ttxd
```

> Then copy or create each file there:
> - ttxd.c
> - Makefile
> - ttxd.service

~/ttxd means /home/<youruser>/ttxd — a standard place for a user project on Ubuntu. Not /opt or /usr/local/src which are more for system-wide software, and not the home directory root which gets cluttered.

```bash
make
```

### 5. Test

In Node-RED:
UDP-IN: 127.0.0.1:5555 ; Output=String
JSON node (optional) => DEBUG

```bash
# Optional if not using NodeRed yet : Terminal 1 — watch UDP output
nc -ulk 5555

# Terminal 2 — start the service
./ttxd 192.168.1.50 21 409 5555
```

JSON objects will appear in terminal 1 as pages are received.

## Usage

```
ttxd <hdhomerun-ip> <channel> <teletext-pid> <udp-port>
```

| Argument | Example | Description |
|---|---|---|
| `hdhomerun-ip` | `192.168.1.50` | IP address of the HDHomeRun device |
| `channel` | `21` | Channel number |
| `teletext-pid` | `409` | Teletext PID in decimal (find with ffprobe) |
| `udp-port` | `5555` | UDP port to send JSON to on 127.0.0.1 |

## Output Format

One UDP datagram per complete teletext page. Each datagram is a JSON
object terminated with a newline:

```json
{
  "page":    101,
  "subpage": 0,
  "ts":      1708789312,
  "lines": [
    "P101 ORF TEXT    Di 25.02.  14:37:08",
    "",
    " NACHRICHTEN",
    "",
    " Regierung einigt sich auf Budgetpfad",
    "...",
    " Weiteres auf Seite 102"
  ]
}
```

| Field | Type | Description |
|---|---|---|
| `page` | integer | Teletext page number (100–899) |
| `subpage` | integer | Subpage number (0 for single-subpage pages) |
| `ts` | integer | Unix timestamp at time of decode |
| `lines` | array of strings | 25 rows, row 0 first. UTF-8, trailing spaces stripped |

Row 0 is always the page header (page number and clock on most
broadcasters). Lines with no content are empty strings.

Pages with multiple subpages (e.g. rotating news pages) each produce a
separate datagram. Filter or aggregate by `page` + `subpage` in
Node-RED as needed.

## Running as a systemd Service

```bash
# Install binary
make install

# Create a dedicated system user
sudo useradd -r -s /usr/sbin/nologin ttxd

# Install and configure the service
sudo cp ttxd.service /etc/systemd/system/
sudo nano /etc/systemd/system/ttxd.service   # edit ExecStart line

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable ttxd
sudo systemctl start ttxd

# Monitor
journalctl -u ttxd -f
```

The service restarts automatically on failure. For multiple channels,
run one instance per channel with a different UDP port.

## Node-RED Integration

Add a **udp in** node:
- Port: `5555`
- Bind: `127.0.0.1`
- Output: `a String`

Connect to a **JSON** node (auto-parse) to get `msg.payload` as a
JavaScript object, then filter by `msg.payload.page` in a **switch**
node to route individual pages to your flows.

## How It Works

```
recv() chunks
    ↓
carry buffer → 188-byte TS packet alignment
    ↓
PID filter
    ↓
PES reassembler
    ↓
vbi_dvb_demux_cor()   — libzvbi: EBU data units → sliced VBI lines
    ↓
vbi_decode()          — libzvbi: page assembly state machine
    ↓
VBI_EVENT_TTX_PAGE callback
    ↓
vbi_fetch_vt_page()   — libzvbi: 40×25 Unicode grid
    ↓
JSON serialiser → UDP sendto()
```

See [IMPLEMENTATION.md](IMPLEMENTATION.md) for a full description of
each stage.

## Files

| File | Description |
|---|---|
| `ttxd.c` | C source, single compilation unit |
| `Makefile` | Build rules |
| `ttxd.service` | systemd unit file |
| `SETUP.md` | Step-by-step installation guide |
| `IMPLEMENTATION.md` | Technical implementation documentation |

## Background

Teletext is still broadcast by most European television networks. The
original motivation for this project was to consume teletext news pages
from a local DVB signal in Node-RED without the overhead of ffmpeg or a
full TV server stack like TVHeadend.

Inspired by [ttxd by Tobias Girstmair](https://git.gir.st/ttxd.git),
which solved the same problem for DVB-T2 using Perl and a different
tool chain.

## License

GPL-3.0. See [LICENSE](LICENSE).

libzvbi is LGPL-2.0.
