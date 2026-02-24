# ttxd — Setup Instructions

## 1. Install dependencies

```bash
sudo apt install libzvbi-dev ffmpeg build-essential
```

`ffmpeg` is only needed once for PID discovery (step 3).
It is not a runtime dependency of the service.

## 2. Find your HDHomeRun IP

```bash
hdhomerun_config discover
# or check your router — the device announces itself via mDNS
```

## 3. Find the teletext PID for your channel

```bash
ffprobe http://<hdhomerun-ip>/auto/v<channel> 2>&1 | grep -i teletext
```

Output will look like:
```
Stream #0:3[0x199]: Subtitle: dvb_teletext
```
`0x199` in hex = **409** in decimal — that is your PID.

If the above does not show it clearly:
```bash
ffprobe -show_streams http://<hdhomerun-ip>/auto/v<channel> 2>&1 \
  | grep -E "codec_name|codec_type|id=" | head -40
```

## 4. Build

```bash
make
```

## 5. Quick test before installing as a service

```bash
# Terminal 1 — listen for UDP output
nc -ulk 5555

# Terminal 2 — run the service
./ttxd 192.168.1.50 21 409 5555
```

You should see JSON objects appearing in terminal 1 within 30 seconds
as teletext pages arrive.

## 6. Install as a systemd service

```bash
# Install binary
make install

# Create a dedicated system user (no login shell, no home directory)
sudo useradd -r -s /usr/sbin/nologin ttxd

# Install the service file
sudo cp ttxd.service /etc/systemd/system/

# Edit the ExecStart line with your actual IP, channel, PID and port
sudo nano /etc/systemd/system/ttxd.service

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable ttxd
sudo systemctl start ttxd

# Check it is running
sudo systemctl status ttxd

# Watch the log live
journalctl -u ttxd -f
```

## 7. Node-RED setup

Add a **udp in** node with:
- Listen on port: `5555`
- Bind to: `127.0.0.1`
- Output: `a String`

Connect it to a **JSON** node (set to auto-parse), then to your flow.

`msg.payload` will be an object like:
```json
{
  "page":    100,
  "subpage": 0,
  "ts":      1708789200,
  "lines":   ["line 0 text", "line 1 text", ..., "line 24 text"]
}
```

## Notes

- One UDP datagram = one complete teletext page
- Pages arrive as the broadcast cycle delivers them — typically a full
  rotation of all pages takes 10–30 seconds
- Row 0 is always the page header (contains page number and clock on
  most broadcasters)
- To receive multiple channels simultaneously, run one ttxd instance
  per channel on a different UDP port
- The service reconnects automatically if the HDHomeRun stream drops
