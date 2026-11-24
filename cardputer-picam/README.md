# PiCam Remote: a Cardputer controller for the Pi camera

Use your M5Stack Cardputer (or Cardputer ADV) as a handheld remote for the Pi camera stack in this repo.

| On the Cardputer | What happens on the Pi |
|---|---|
| **Live view** at up to ~12 fps on the 240×135 screen, with digital zoom (z/x) and pan (arrow keys) | Tapped from whatever already owns the camera (picam-fx RTSP, picam-hub MJPEG, or the newest timelapse frame) |
| **Photo** (Enter in live view, `p` on home) | Full-res still saved to `~/recordings/photo_*.jpg` |
| **Record clip** (`r`) | picam-recorder (:8090) records the RTSP stream to `~/recordings/clip_*.mp4` |
| **Timelapse**: start (with name and interval), live frame count, stop, render to MP4 | Runs the `timelapse` command. The livestream stops and restarts automatically, because the camera only allows one consumer |
| **Image & fisheye** tuning: **rotation (0/90/180/270)**, white balance, EV, brightness, contrast, saturation, sharpness, denoise, fisheye K1/K2, FPS, bitrate, plus presets (Vivid, Night, Mono, Max fisheye, Flat) | Writes `/etc/default/picam-fx` and restarts picam-fx. Rotation also applies to photos, clips and timelapses (180° is done in the camera ISP for free) |
| **Camera mode**: Livestream (fisheye fx), PiCam Hub, or Off | Starts and stops the right services in the right order |
| **Gallery**: browse recordings, hub captures and `~/cardputer-apps`, show thumbnails, **save any file to the Cardputer's SD** (`/DCIM/PiCam`), delete | ffmpeg makes the thumbnails, cached on the Pi |
| **Save frame** (`s` in live view) | The frame on screen goes straight to the Cardputer's SD card |
| **Watch on phone (QR)** | Shows a QR code for the WebRTC page, recorder app, RTSP URL, hub and TV dashboard |
| **Pi system**: CPU temp, under-voltage/throttle flags, load, memory, disk, uptime, service status, restart services, Pi WiFi mode, reboot, shut down | |

## 1. Set up the Pi (once)

```bash
cd ~/pi-setup && git pull
./setup-cardputer-remote.sh      # the API on :8091, prints a TOKEN
./setup-picam-ap.sh              # optional, recommended: own WiFi fallback
```

`setup-cardputer-remote.sh` installs a small Flask app (`picam-remote`, port 8091). It also installs a root helper that can only run a whitelisted set of actions, plus an mDNS advert so the Cardputer finds the Pi by itself. It never opens the camera itself.

## 2. Networking: hotspot or the Pi's own WiFi

* **Phone hotspot (what you do now):** the Pi and the Cardputer both join it. The app finds the Pi by itself, trying the last address first, then mDNS, then a scan of the hotspot's subnet.
* **PiCam fallback (recommended):** after `setup-picam-ap.sh`, the Pi still joins your hotspot when it's around. If no saved WiFi appears within ~45 s of boot, the Pi hosts its own network instead: **SSID `PiCam`, password `picam1234`, Pi at `10.42.0.1`**. The Cardputer app already knows this network, so with no phone around you just turn both on and it works. While hosting, the Pi checks for your hotspot every 2 minutes and moves back onto it when it appears. You can also flip it by hand under *Pi system > Pi WiFi*.

## 3. Install the app on the Cardputer

1. Get `PiCamRemote.bin` from the GitHub Actions run (artifact **PiCamRemote**), or build it yourself with `pio run -d cardputer-picam/firmware`.
2. Copy it to the SD card, for example `/apps/00 Quick Launch/PiCamRemote.bin`.
3. In M5Launcher choose SD, then the file, then Install. This keeps M5Launcher as your launcher. Don't burn the .bin with M5Burner, because that replaces the launcher.

The quickest first-time setup is a text file on the SD card instead of typing on the keyboard. Create `/picam.cfg`:

```
wifi=MyHotspot,YOUR_HOTSPOT_PASSWORD
token=THE_TOKEN_THE_PI_PRINTED
# pi=10.42.0.1        (optional; leave out for auto-discovery)
```

The app reads it on boot, saves the settings, and renames the file to `/picam.cfg.done`. Everything can also be changed later under *Settings*.

## Keys

| Where | Keys |
|---|---|
| Everywhere | `;` `.` `,` `/` are up, down, left, right. Enter selects. `` ` `` (Esc) or Del goes back |
| Home | `l` live, `p` photo, `r` record, `t` timelapse, `g` gallery |
| Live view | Enter photo, `r` record, `t` timelapse, `i` image tuning, `z`/`x` zoom, arrows pan, `s` save frame to SD, `q` quality (Smooth/Balanced/Sharp), `h` hide HUD |
| Gallery viewer | `,` `/` previous and next, `d` save to SD, `x` delete |
| Text boxes | Enter OK, Del erase, Tab cancel |

## API (for scripts too)

All endpoints take the `X-Deck-Token` header (or `?token=`) when a token is set.

`GET /api/ping`, `GET /api/status`, `GET /api/frame.jpg?after=SEQ`, `GET /api/live.mjpeg`, `GET|POST /api/view`, `POST /api/photo`, `POST /api/record/{start,stop,toggle}`, `GET /api/timelapse`, `POST /api/timelapse/{start,stop,render}`, `GET|POST /api/image`, `POST /api/mode`, `POST /api/service`, `GET|POST /api/net`, `POST /api/system`, `GET /api/files?dir=recordings|captures|timelapse|apps`, `GET /api/thumb?f=`, `GET|DELETE /api/file?f=`, `GET /api/links`.
