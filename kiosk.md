# Kiosk Display on Ubuntu 24.04 with cage

## Overview

This setup replaces the GNOME display manager with `cage`, a minimal Wayland compositor
designed for single-application kiosk use. The machine boots directly into a fullscreen
Chromium window showing your HTML page. No desktop, no login screen, no way to escape
to a shell from the display.

Stack:
```
Hardware → Kernel (DRM/KMS) → cage → Chromium (fullscreen)
```

---

## Prerequisites

- Ubuntu 24.04 LTS
- GNOME installed (and GDM running) — this setup disables GDM
- SSH access for configuration (the display port will be taken over by cage)
- `sudo` privileges

---

## Step 1 — Install cage
```bash
sudo apt install cage
```

---

## Step 2 — Create a dedicated kiosk user
```bash
sudo useradd -m -s /bin/bash kiosk
```

No password, no sudo access. This user owns the display session and nothing else.

---

## Step 3 — Install Chromium
```bash
sudo apt install chromium-browser
```

Verify the binary path:
```bash
which chromium-browser
```

On some Ubuntu 24.04 installations the snap version is installed instead, with the binary
at `/snap/bin/chromium`. Use whichever path your system returns.

---

## Step 4 — Place your HTML page
```bash
sudo mkdir -p /home/kiosk/display
sudo cp /path/to/your/index.html /home/kiosk/display/index.html
sudo chown -R kiosk:kiosk /home/kiosk/display
```

---

## Step 5 — Create the systemd service
```bash
sudo nano /etc/systemd/system/kiosk.service
```
```ini
[Unit]
Description=Kiosk Display
After=systemd-logind.service

[Service]
User=kiosk
PAMName=login
TTYPath=/dev/tty1
StandardInput=tty
EnvironmentFile=-/etc/environment
ExecStart=/usr/bin/cage -- chromium-browser --kiosk --noerrdialogs \
  --disable-infobars --no-first-run \
  file:///home/kiosk/display/index.html
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Adjust the `file://` path if you placed your HTML page elsewhere.
If loading a network URL, replace the `file://` argument with the full URL.

---

## Step 6 — Disable GDM and enable the kiosk service
```bash
sudo systemctl disable gdm
sudo systemctl enable kiosk
```

---

## Step 7 — Reboot
```bash
sudo reboot
```

The machine will boot directly into cage with Chromium fullscreen on tty1.

---

## Managing the service over SSH

Check status:
```bash
sudo systemctl status kiosk
```

View logs:
```bash
journalctl -u kiosk -f
```

Restart (e.g. after updating the HTML page):
```bash
sudo systemctl restart kiosk
```

Stop:
```bash
sudo systemctl stop kiosk
```

---

## Updating the HTML page
```bash
sudo cp /path/to/new/index.html /home/kiosk/display/index.html
sudo chown kiosk:kiosk /home/kiosk/display/index.html
sudo systemctl restart kiosk
```

---

## Reverting to normal GNOME
```bash
sudo systemctl disable kiosk
sudo systemctl enable gdm
sudo reboot
```

---

## Troubleshooting

**Black screen on boot**
Check logs: `journalctl -u kiosk --boot`. Most common cause is a wrong Chromium binary path.

**cage not found**
Verify: `which cage`. If missing, `sudo apt install cage` did not complete successfully.

**Chromium shows first-run UI despite flags**
Delete the kiosk user's Chromium profile and restart:
```bash
sudo rm -rf /home/kiosk/.config/chromium
sudo systemctl restart kiosk
```

**Page loads before network is ready (network URL only)**
Add to the `[Unit]` section of the service:
```ini
After=systemd-logind.service network-online.target
Wants=network-online.target
```

**Display turns off / screensaver activates**
Add before `ExecStart` in the service:
```ini
ExecStartPre=/usr/bin/xset s off
ExecStartPre=/usr/bin/xset -dpms
```
Or handle it in a cage startup wrapper script that calls `wlopm` (Wayland equivalent):
```bash
sudo apt install wlopm
```
Then in the service:
```ini
ExecStartPost=/usr/bin/wlopm --on all
```
