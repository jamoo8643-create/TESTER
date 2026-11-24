#!/bin/bash
# ================================================================
#  PiCam Remote Pi bridge  (picam-remote)  —  for the M5Stack Cardputer
# ================================================================
#  Flask API on :8091 that the PiCam Remote firmware on the Cardputer
#  talks to. It drives the existing pi-setup camera stack:
#
#    picam-fx / mediamtx  (livestream)   picam-hub (:8080)
#    picam-recorder (:8090) clips        timelapse command
#
#  and never grabs the camera itself, so the Pi 3 one-consumer
#  CSI rule is respected. Preview frames are tapped from whatever
#  service already owns the camera.
#
#  Also:  ~/cardputer-apps   drop .bin files here and install them
#                            from the Cardputer (Library > Pi apps)
#
#  Run:  chmod +x setup-cardputer-remote.sh && ./setup-cardputer-remote.sh
#  Re-running it is safe (keeps your token; adds new features like ROTATE).
#        ./setup-cardputer-remote.sh --no-token   (LAN-only, no auth)
# ================================================================
set -e

APP_DIR="$HOME/picam-remote"
CONF=/etc/default/picam-remote
PORT=8091

echo "╔══════════════════════════════════════════╗"
echo "║   PiCam Remote bridge — Setup            ║"
echo "╚══════════════════════════════════════════╝"

echo "[1/6] Installing deps..."
sudo apt-get install -y -qq python3-flask ffmpeg avahi-daemon >/dev/null
mkdir -p "$APP_DIR" "$HOME/recordings" "$HOME/cardputer-apps"

echo "[2/6] Writing config..."
if [ ! -f "$CONF" ]; then
  if [ "$1" = "--no-token" ]; then TOKEN=""; else TOKEN=$(head -c 6 /dev/urandom | od -An -tx1 | tr -d ' \n'); fi
  sudo tee "$CONF" > /dev/null << CONFEOF
# PiCam Remote Pi bridge. Enter this token in PiCam Remote > Settings > Pi token.
# Leave TOKEN empty to turn auth off (anyone on your LAN can control the camera).
TOKEN=$TOKEN
PORT=$PORT
# Live preview uses the Pi's hardware H.264 decoder when available (auto|off)
PREVIEW_HWDEC=auto
CONFEOF
fi
TOKEN=$(grep -E '^TOKEN=' "$CONF" | cut -d= -f2)

echo "[3/6] Writing the bridge app..."
cat > "$APP_DIR/app.py" << 'PYEOF'
#!/usr/bin/env python3
"""
PiCam Remote Pi bridge  (picam-remote)
==================================
A small JSON + JPEG API on :8091 that lets the M5Stack Cardputer (PiCam Remote
firmware) drive the PiCam setup from the pi-setup repo:

  * live preview frames sized for the Cardputer's 240x135 screen
  * photo, clip recording (via picam-recorder :8090), timelapse control
  * colour / fisheye tuning (/etc/default/picam-fx) and camera mode switching
  * gallery: list, thumbnail, download and delete recordings
  * Pi health (temp, load, memory, disk, throttling) and service control
  * an "app drop" folder (~/cardputer-apps) the Cardputer can install from

It never holds the camera itself. Preview frames come from whatever already
owns the camera: the mediamtx RTSP path (picam-fx / livestream), the picam-hub
MJPEG stream, or the newest timelapse frame. That keeps the Pi 3's
one-consumer-at-a-time CSI camera rule intact.

Root actions go through /usr/local/bin/picam-remote-ctl, which whitelists
every service, config key and value it will touch.
"""

import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import threading
import time
import urllib.request
from datetime import datetime
from pathlib import Path

from flask import Flask, Response, abort, jsonify, request, send_file

VERSION = "1.0.0"
HOME = Path(os.path.expanduser("~"))
CONF_FILE = Path("/etc/default/picam-remote")
FX_CONF = Path("/etc/default/picam-fx")
TL_CONF = Path("/etc/default/picam-timelapse")
CTL = "/usr/local/bin/picam-remote-ctl"
TIMELAPSE = "/usr/local/bin/timelapse"
RTSP_URL = "rtsp://127.0.0.1:8554/cam"
HUB_URL = "http://127.0.0.1:8080"
RECORDER_URL = "http://127.0.0.1:8090"
CACHE = HOME / ".cache" / "picam-remote"
CACHE.mkdir(parents=True, exist_ok=True)

# Folders the Cardputer may browse. key -> path
DIRS = {
    "recordings": HOME / "recordings",
    "captures": HOME / "picam-hub" / "captures",
    "timelapse": HOME / "timelapse",
    "apps": HOME / "cardputer-apps",
}
for _d in (DIRS["recordings"], DIRS["apps"]):
    _d.mkdir(parents=True, exist_ok=True)

SERVICES = ["mediamtx", "picam-fx", "picam-hub", "picam-recorder", "tv-dashboard"]


def read_kv(path):
    out = {}
    try:
        for line in Path(path).read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"')
    except Exception:
        pass
    return out


CONF = read_kv(CONF_FILE)
TOKEN = CONF.get("TOKEN", "")
PORT = int(CONF.get("PORT", "8091"))

app = Flask(__name__)


@app.before_request
def _auth():
    if not TOKEN:
        return None
    if request.path in ("/", "/api/ping"):
        return None
    got = request.headers.get("X-Deck-Token") or request.args.get("token", "")
    if got != TOKEN:
        return jsonify(ok=False, error="bad token"), 401
    return None


# ------------------------------------------------------------------ helpers
def sh(args, timeout=20):
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).strip()
    except Exception as e:  # noqa: BLE001
        return 1, str(e)


def ctl(*args, timeout=30):
    return sh(["sudo", "-n", CTL, *args], timeout=timeout)


def svc_active(name):
    rc, _ = sh(["systemctl", "is-active", "--quiet", name], timeout=5)
    return rc == 0


def port_open(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3):
            return True
    except OSError:
        return False


def http_json(url, method="GET", timeout=6):
    req = urllib.request.Request(url, method=method, data=b"" if method == "POST" else None)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode() or "{}")


def tl_status():
    rc, out = sh([TIMELAPSE, "status"], timeout=10) if Path(TIMELAPSE).exists() else (1, "not installed")
    st = {"installed": Path(TIMELAPSE).exists(), "active": out.startswith("RUNNING"),
          "session": None, "frames": 0, "covering_s": 0, "text": out}
    m = re.search(r"session=(\S+)\s+frames=(\d+)\s+covering (\d+)s", out)
    if m:
        st.update(session=m.group(1), frames=int(m.group(2)), covering_s=int(m.group(3)))
    cfg = read_kv(TL_CONF)
    st["interval"] = int(float(cfg.get("INTERVAL", "3") or 3))
    st["out_fps"] = int(float(cfg.get("OUT_FPS", "24") or 24))
    st["rendering"] = bool(_render["proc"] and _render["proc"].poll() is None)
    return st


def latest_tl_frame():
    tl = DIRS["timelapse"]
    try:
        name = (tl / ".current").read_text().strip()
        frames = sorted((tl / name).glob("f_*.jpg"))
        return frames[-1] if frames else None
    except Exception:
        return None


_mode_cache = {"t": 0.0, "v": "off"}


def camera_mode(max_age=2.0):
    """Which service owns the camera. Cached briefly: preview polls this per frame."""
    now = time.time()
    if now - _mode_cache["t"] < max_age:
        return _mode_cache["v"]
    _mode_cache.update(t=now, v=_camera_mode())
    return _mode_cache["v"]


def _camera_mode():
    if tl_status()["active"]:
        return "timelapse"
    if svc_active("picam-fx"):
        return "fx"
    if svc_active("picam-hub"):
        return "hub"
    if svc_active("mediamtx"):
        return "live"  # plain mediamtx rpiCamera source, if configured that way
    return "off"


def vitals():
    v = {}
    try:
        v["temp"] = round(int(open("/sys/class/thermal/thermal_zone0/temp").read()) / 1000, 1)
    except Exception:
        v["temp"] = None
    try:
        mem = {}
        for line in open("/proc/meminfo"):
            k, val = line.split(":")
            mem[k] = int(val.strip().split()[0])
        v["mem"] = round((1 - mem["MemAvailable"] / mem["MemTotal"]) * 100)
    except Exception:
        v["mem"] = None
    du = shutil.disk_usage(str(HOME))
    v["disk_pct"] = round(du.used / du.total * 100)
    v["disk_free_gb"] = round(du.free / 1e9, 1)
    try:
        v["load"] = float(open("/proc/loadavg").read().split()[0])
        v["uptime_s"] = int(float(open("/proc/uptime").read().split()[0]))
    except Exception:
        v["load"], v["uptime_s"] = None, None
    rc, out = sh(["vcgencmd", "get_throttled"], timeout=3)
    v["throttled"] = out.split("=")[-1] if rc == 0 else None
    try:
        for line in open("/proc/net/wireless"):
            if "wlan" in line:
                v["wifi"] = int(float(line.split()[3]))
    except Exception:
        pass
    return v


# ------------------------------------------------------------------ preview
class Preview:
    """One shared ffmpeg that turns the current camera source into small
    JPEGs. Started on demand, stopped after IDLE seconds with no viewers."""

    IDLE = 20

    def __init__(self):
        self.lock = threading.Condition()
        self.frame = None
        self.seq = 0
        self.proc = None
        self.src = None
        self.last_req = 0
        self.params = {"w": 240, "h": 135, "fps": 8, "q": 7, "zoom": 1.0, "px": 0.5, "py": 0.5}
        self.error = ""
        # Pi 3 software H.264 decode competes with picam-fx for CPU; use the
        # VideoCore decoder when ffmpeg has it, and drop back if it misbehaves.
        rc, out = sh(["ffmpeg", "-hide_banner", "-decoders"], timeout=10)
        self.hwdec = CONF.get("PREVIEW_HWDEC", "auto") != "off" and "h264_v4l2m2m" in out
        self.started_at = 0.0
        self._seq_at_start = 0
        threading.Thread(target=self._watchdog, daemon=True).start()

    def source(self):
        mode = camera_mode()
        if mode in ("fx", "live") and port_open(8554):
            return ("rtsp", RTSP_URL)
        if mode == "hub" and port_open(8080):
            return ("mjpeg", HUB_URL + "/stream")
        if mode == "timelapse":
            return ("timelapse", None)
        return (None, None)

    def vf(self):
        p = self.params
        z = max(1.0, float(p["zoom"]))
        f = []
        if z > 1.0:
            # crop a 1/z window centred on (px,py), clamped to the frame
            f.append(
                f"crop=iw/{z}:ih/{z}:"
                f"(iw-iw/{z})*{min(max(p['px'], 0), 1)}:(ih-ih/{z})*{min(max(p['py'], 0), 1)}")
        # letterbox into exactly w x h so the Cardputer can blit 1:1
        f.append(f"scale={p['w']}:{p['h']}:force_original_aspect_ratio=decrease")
        f.append(f"pad={p['w']}:{p['h']}:(ow-iw)/2:(oh-ih)/2")
        f.append(f"fps={p['fps']}")
        return ",".join(f)

    def _start(self, kind, url):
        self._stop()
        if kind == "rtsp":
            dec = ["-c:v", "h264_v4l2m2m"] if self.hwdec else []
            inp = ["-rtsp_transport", "tcp", *dec, "-i", url]
        else:
            inp = ["-f", "mpjpeg", "-i", url]
        cmd = ["ffmpeg", "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
               *inp, "-an", "-vf", self.vf(), "-q:v", str(self.params["q"]),
               "-f", "image2pipe", "-c:v", "mjpeg", "-"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     bufsize=0)
        self.src = (kind, url)
        self.started_at = time.time()
        threading.Thread(target=self._reader, args=(self.proc,), daemon=True).start()

    def _stop(self):
        p, self.proc, self.src = self.proc, None, None
        if p and p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(timeout=2)
            except Exception:
                p.kill()

    def _reader(self, proc):
        buf = b""
        while True:
            chunk = proc.stdout.read(16384)
            if not chunk:
                break
            buf += chunk
            while True:
                s = buf.find(b"\xff\xd8")
                e = buf.find(b"\xff\xd9", s + 2) if s >= 0 else -1
                if s < 0 or e < 0:
                    if s > 0:
                        buf = buf[s:]
                    break
                jpg = buf[s:e + 2]
                buf = buf[e + 2:]
                with self.lock:
                    self.frame = jpg
                    self.seq += 1
                    self.lock.notify_all()

    def _tl_frame(self):
        f = latest_tl_frame()
        if not f:
            return None
        key = CACHE / f"tl_{f.parent.name}_{f.name}_{self.params['w']}x{self.params['h']}.jpg"
        if not key.exists():
            for old in CACHE.glob("tl_*.jpg"):
                old.unlink(missing_ok=True)
            self.params_vf_once(f, key)
        return key.read_bytes() if key.exists() else None

    def params_vf_once(self, src, dst):
        p = dict(self.params)
        p["fps"] = 1
        vf = self.vf().replace(f",fps={self.params['fps']}", "")
        sh(["ffmpeg", "-loglevel", "error", "-y", "-i", str(src), "-vf", vf,
            "-q:v", str(p["q"]), "-frames:v", "1", str(dst)], timeout=20)

    def ensure(self):
        self.last_req = time.time()
        kind, url = self.source()
        if kind in ("rtsp", "mjpeg"):
            if self.proc and self.proc.poll() is not None and self.hwdec and kind == "rtsp" \
                    and self.seq == self._seq_at_start and time.time() - self.started_at < 10:
                self.hwdec = False  # hardware decoder died before a single frame
            if not self.proc or self.proc.poll() is not None or self.src != (kind, url):
                self._seq_at_start = self.seq
                self._start(kind, url)
            self.error = ""
        else:
            self._stop()
            self.error = "timelapse" if kind == "timelapse" else "camera off"
        return kind

    def get(self, after_seq=0, wait=3.0):
        kind = self.ensure()
        if kind == "timelapse":
            data = self._tl_frame()
            with self.lock:
                if data and data != self.frame:
                    self.frame, self.seq = data, self.seq + 1
                return self.frame if data else None, self.seq, kind
        if kind is None:
            return None, self.seq, None
        deadline = time.time() + wait
        with self.lock:
            while self.seq <= after_seq and time.time() < deadline:
                self.lock.wait(timeout=0.25)
            return self.frame, self.seq, kind

    def set_params(self, **kw):
        changed = False
        for k, v in kw.items():
            if k in self.params and v is not None:
                v = type(self.params[k])(v)
                if self.params[k] != v:
                    self.params[k] = v
                    changed = True
        p = self.params
        p["w"] = min(max(p["w"], 32), 1280)
        p["h"] = min(max(p["h"], 32), 720)
        p["fps"] = min(max(p["fps"], 1), 25)
        p["q"] = min(max(p["q"], 2), 31)
        p["zoom"] = min(max(p["zoom"], 1.0), 8.0)
        if changed and self.proc:
            self._stop()
        return dict(p)

    def _watchdog(self):
        while True:
            time.sleep(2)
            if self.proc and time.time() - self.last_req > self.IDLE:
                self._stop()


preview = Preview()


@app.route("/api/frame.jpg")
def api_frame():
    after = int(request.args.get("after", 0))
    jpg, seq, kind = preview.get(after_seq=after)
    if not jpg:
        return jsonify(ok=False, error=preview.error or "no frame", mode=camera_mode()), 503
    r = Response(jpg, mimetype="image/jpeg")
    r.headers["X-Seq"] = str(seq)
    r.headers["X-Source"] = kind or ""
    r.headers["Cache-Control"] = "no-store"
    return r


@app.route("/api/live.mjpeg")
def api_live():
    def gen():
        seq = 0
        while True:
            jpg, s, kind = preview.get(after_seq=seq, wait=5.0)
            if not jpg:
                time.sleep(1.0)
                continue
            if s == seq:
                time.sleep(0.5 if kind == "timelapse" else 0.05)
                continue
            seq = s
            yield (b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                   + str(len(jpg)).encode() + b"\r\nX-Seq: " + str(s).encode()
                   + b"\r\n\r\n" + jpg + b"\r\n")
    return Response(gen(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/api/view", methods=["GET", "POST"])
def api_view():
    if request.method == "POST":
        d = request.get_json(force=True, silent=True) or {}
        return jsonify(ok=True, view=preview.set_params(**d))
    return jsonify(ok=True, view=preview.params)


# ------------------------------------------------------------------ status
@app.route("/api/ping")
def api_ping():
    return jsonify(ok=True, name="picam-remote", version=VERSION, host=socket.gethostname(),
                   auth=bool(TOKEN))


def rec_status():
    mode = camera_mode()
    if mode == "hub":
        return {"active": _hub_rec["active"], "elapsed": int(time.time() - _hub_rec["t0"])
                if _hub_rec["active"] else 0, "file": _hub_rec["file"], "via": "hub"}
    if port_open(8090):
        try:
            r = http_json(RECORDER_URL + "/record/status", timeout=3)
            r["via"] = "recorder"
            return r
        except Exception:
            pass
    return {"active": False, "elapsed": 0, "file": None, "via": None}


@app.route("/api/status")
def api_status():
    svcs = {s: svc_active(s) for s in SERVICES}
    return jsonify(ok=True, version=VERSION, host=socket.gethostname(), time=int(time.time()),
                   mode=camera_mode(), services=svcs, rec=rec_status(), tl=tl_status(),
                   sys=vitals(), view=preview.params)


@app.route("/api/links")
def api_links():
    host = request.host.split(":")[0]
    return jsonify(ok=True, links=[
        {"name": "WebRTC (phone/PC)", "url": f"http://{host}:8889/cam"},
        {"name": "Recorder app", "url": f"http://{host}:8090/"},
        {"name": "RTSP (OBS/VLC)", "url": f"rtsp://{host}:8554/cam"},
        {"name": "PiCam Hub", "url": f"http://{host}:8080/"},
        {"name": "TV dashboard", "url": f"http://{host}:8085/"},
    ])


# ------------------------------------------------------------------ capture
_hub_rec = {"active": False, "t0": 0, "file": None}


@app.route("/api/photo", methods=["POST"])
def api_photo():
    mode = camera_mode()
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out = DIRS["recordings"] / f"photo_{ts}.jpg"
    if mode == "hub":
        try:
            r = http_json(HUB_URL + "/photo", method="POST", timeout=10)
            return jsonify(ok=bool(r.get("ok")), file="captures/" + str(r.get("file")))
        except Exception as e:  # noqa: BLE001
            return jsonify(ok=False, error=str(e))
    if mode == "timelapse":
        f = latest_tl_frame()
        if not f:
            return jsonify(ok=False, error="no timelapse frame yet")
        shutil.copy(f, out)
        return jsonify(ok=True, file="recordings/" + out.name)
    if mode in ("fx", "live") and port_open(8554):
        rc, msg = sh(["ffmpeg", "-loglevel", "error", "-y", "-rtsp_transport", "tcp", "-i",
                      RTSP_URL, "-frames:v", "1", "-q:v", "2", str(out)], timeout=20)
        if rc == 0 and out.exists():
            return jsonify(ok=True, file="recordings/" + out.name)
        return jsonify(ok=False, error=msg[-200:] or "capture failed")
    return jsonify(ok=False, error="camera is off")


@app.route("/api/record/<action>", methods=["POST"])
def api_record(action):
    if action not in ("start", "stop", "toggle"):
        abort(404)
    mode = camera_mode()
    if action == "toggle":
        action = "stop" if rec_status()["active"] else "start"
    if mode == "hub":
        try:
            r = http_json(f"{HUB_URL}/record/{action}", method="POST", timeout=10)
            if r.get("ok"):
                _hub_rec.update(active=(action == "start"), t0=time.time(), file=r.get("file"))
            return jsonify(ok=bool(r.get("ok")), error=r.get("error"), file=r.get("file"))
        except Exception as e:  # noqa: BLE001
            return jsonify(ok=False, error=str(e))
    if mode not in ("fx", "live"):
        return jsonify(ok=False, error="start the livestream first (mode fx)")
    if not port_open(8090):
        rc, msg = ctl("svc", "start", "picam-recorder")
        time.sleep(2)
    try:
        r = http_json(f"{RECORDER_URL}/record/{action}", method="POST", timeout=10)
        return jsonify(ok=bool(r.get("ok")), error=r.get("error"), file=r.get("file"))
    except Exception as e:  # noqa: BLE001
        return jsonify(ok=False, error=f"recorder unreachable: {e}")


# ------------------------------------------------------------------ timelapse
_render = {"proc": None}
NAME_RE = re.compile(r"^[A-Za-z0-9_\-]{1,40}$")


@app.route("/api/timelapse", methods=["GET"])
def api_tl():
    st = tl_status()
    sessions = []
    tl = DIRS["timelapse"]
    if tl.exists():
        for d in sorted(tl.iterdir(), reverse=True):
            if d.is_dir():
                n = len(list(d.glob("f_*.jpg")))
                sessions.append({"name": d.name, "frames": n,
                                 "rendered": (DIRS["recordings"] / f"timelapse_{d.name}.mp4").exists()})
    st["sessions"] = sessions[:30]
    return jsonify(ok=True, **st)


@app.route("/api/timelapse/<action>", methods=["POST"])
def api_tl_action(action):
    d = request.get_json(force=True, silent=True) or {}
    if action == "start":
        name = d.get("name") or datetime.now().strftime("%Y%m%d_%H%M%S")
        if not NAME_RE.match(name):
            return jsonify(ok=False, error="name: letters, digits, _ and - only")
        if "interval" in d:
            rc, msg = ctl("set", "tl", "INTERVAL", str(int(d["interval"])))
            if rc != 0:
                return jsonify(ok=False, error=msg)
        if rec_status()["active"]:
            return jsonify(ok=False, error="stop the recording first")
        preview._stop()
        rc, out = sh([TIMELAPSE, "start", name], timeout=30)
        _mode_cache["t"] = 0
        return jsonify(ok=rc == 0, text=out)
    if action == "stop":
        rc, out = sh([TIMELAPSE, "stop"], timeout=30)
        _mode_cache["t"] = 0
        return jsonify(ok=rc == 0, text=out)
    if action == "render":
        if _render["proc"] and _render["proc"].poll() is None:
            return jsonify(ok=False, error="already rendering")
        args = [TIMELAPSE, "render"]
        if d.get("name"):
            if not NAME_RE.match(d["name"]):
                return jsonify(ok=False, error="bad name")
            args.append(d["name"])
        _render["proc"] = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                           stderr=subprocess.DEVNULL)
        return jsonify(ok=True, text="rendering in the background")
    abort(404)


# ------------------------------------------------------------------ image / mode
FX_KEYS = {
    "AWB": ["auto", "incandescent", "tungsten", "fluorescent", "indoor", "daylight", "cloudy"],
    "SATURATION": (0.0, 2.0), "CONTRAST": (0.0, 2.0), "BRIGHTNESS": (-1.0, 1.0),
    "SHARPNESS": (0.0, 2.0), "EV": (-10.0, 10.0),
    "DENOISE": ["auto", "off", "cdn_off", "cdn_fast", "cdn_hq"],
    "FX_K1": (-1.0, 1.0), "FX_K2": (-1.0, 1.0), "FPS": (1, 30),
    "OUT_SIZE": (240, 1080), "BITRATE": ["1M", "2M", "3M", "4M", "6M"],
    "ROTATE": ["0", "90", "180", "270"],
}


@app.route("/api/image", methods=["GET", "POST"])
def api_image():
    if request.method == "GET":
        cfg = read_kv(FX_CONF)
        return jsonify(ok=True, exists=FX_CONF.exists(),
                       values={k: cfg.get(k) for k in FX_KEYS}, keys={
                           k: (v if isinstance(v, list) else {"min": v[0], "max": v[1]})
                           for k, v in FX_KEYS.items()})
    d = request.get_json(force=True, silent=True) or {}
    errors = []
    for k, v in d.items():
        if k not in FX_KEYS:
            errors.append(f"{k}: unknown")
            continue
        rc, msg = ctl("set", "fx", k, str(v))
        if rc != 0:
            errors.append(f"{k}: {msg}")
    restarted = False
    if not errors and svc_active("picam-fx"):
        preview._stop()
        ctl("svc", "restart", "picam-fx")
        restarted = True
    return jsonify(ok=not errors, errors=errors, restarted=restarted)


@app.route("/api/mode", methods=["POST"])
def api_mode():
    d = request.get_json(force=True, silent=True) or {}
    target = d.get("mode")
    if tl_status()["active"]:
        return jsonify(ok=False, error="timelapse is running: stop it first")
    preview._stop()
    steps = []
    if target == "fx":
        steps = [("stop", "picam-hub"), ("start", "mediamtx"), ("start", "picam-fx")]
    elif target == "hub":
        steps = [("stop", "picam-fx"), ("stop", "mediamtx"), ("start", "picam-hub")]
    elif target == "off":
        steps = [("stop", "picam-fx"), ("stop", "picam-hub")]
    else:
        return jsonify(ok=False, error="mode must be fx, hub or off")
    log = []
    for act, name in steps:
        rc, msg = ctl("svc", act, name)
        log.append(f"{act} {name}: {'ok' if rc == 0 else msg}")
    time.sleep(2)
    return jsonify(ok=True, mode=camera_mode(max_age=0), log=log)


@app.route("/api/service", methods=["POST"])
def api_service():
    d = request.get_json(force=True, silent=True) or {}
    rc, msg = ctl("svc", str(d.get("action")), str(d.get("name")))
    return jsonify(ok=rc == 0, text=msg)


@app.route("/api/net", methods=["GET", "POST"])
def api_net():
    """Which WiFi the Pi is on: its own PiCam AP or a saved network (hotspot)."""
    if not Path("/usr/local/bin/picam-wifi").exists():
        return jsonify(ok=True, installed=False, mode="client")
    if request.method == "POST":
        d = request.get_json(force=True, silent=True) or {}
        mode = d.get("mode")
        if mode not in ("ap", "client"):
            return jsonify(ok=False, error="mode must be ap or client")
        # Answer first: the switch drops this very connection.
        threading.Timer(1.0, lambda: ctl("wifi", mode, timeout=90)).start()
        return jsonify(ok=True, text=f"switching to {mode}; reconnect in ~30s")
    rc, out = sh(["/usr/local/bin/picam-wifi", "status"], timeout=10)
    parts = out.split()
    return jsonify(ok=True, installed=True, mode=parts[0] if parts else "down",
                   ssid=parts[1] if len(parts) > 1 else None,
                   ip=parts[2] if len(parts) > 2 else None)


@app.route("/api/system", methods=["POST"])
def api_system():
    d = request.get_json(force=True, silent=True) or {}
    act = d.get("action")
    if act not in ("reboot", "poweroff"):
        return jsonify(ok=False, error="action must be reboot or poweroff")
    threading.Timer(1.0, lambda: ctl("power", act)).start()
    return jsonify(ok=True)


# ------------------------------------------------------------------ files
def resolve(f):
    """'recordings/clip.mp4' -> Path, only inside the whitelisted folders."""
    if not f or "/" not in f:
        abort(400)
    key, rest = f.split("/", 1)
    base = DIRS.get(key)
    if base is None or ".." in rest.split("/") or rest.startswith("/"):
        abort(400)
    p = (base / rest).resolve()
    if not str(p).startswith(str(base.resolve())):
        abort(400)
    return p


EXT = {".jpg": "photo", ".jpeg": "photo", ".mp4": "video", ".h264": "video", ".bin": "app"}


@app.route("/api/files")
def api_files():
    key = request.args.get("dir", "recordings")
    base = DIRS.get(key)
    if base is None:
        abort(400)
    sub = request.args.get("sub", "")
    folder = resolve(f"{key}/{sub}") if sub else base
    items = []
    if folder.exists():
        entries = sorted(folder.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True)
        for p in entries:
            if p.name.startswith("."):
                continue
            st = p.stat()
            kind = "dir" if p.is_dir() else EXT.get(p.suffix.lower())
            if not kind:
                continue
            items.append({"name": p.name, "kind": kind, "size": st.st_size,
                          "mtime": int(st.st_mtime)})
    off = int(request.args.get("offset", 0))
    lim = int(request.args.get("limit", 40))
    return jsonify(ok=True, dir=key, sub=sub, total=len(items), items=items[off:off + lim])


@app.route("/api/thumb")
def api_thumb():
    p = resolve(request.args.get("f"))
    if not p.is_file():
        abort(404)
    w = min(int(request.args.get("w", 240)), 640)
    h = min(int(request.args.get("h", 135)), 480)
    digest = hashlib.sha1(f"{p}|{p.stat().st_mtime}|{w}x{h}".encode()).hexdigest()[:16]
    key = CACHE / f"th_{digest}.jpg"
    if not key.exists():
        vf = f"scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2"
        pre = ["-ss", "1"] if p.suffix.lower() in (".mp4",) else []
        rc, _ = sh(["ffmpeg", "-loglevel", "error", "-y", *pre, "-i", str(p), "-vf", vf,
                    "-frames:v", "1", "-q:v", "6", str(key)], timeout=30)
        if rc != 0 and pre:
            sh(["ffmpeg", "-loglevel", "error", "-y", "-i", str(p), "-vf", vf,
                "-frames:v", "1", "-q:v", "6", str(key)], timeout=30)
        if not key.exists():
            abort(500)
    return send_file(key, mimetype="image/jpeg")


@app.route("/api/file", methods=["GET", "DELETE"])
def api_file():
    p = resolve(request.args.get("f"))
    if not p.is_file():
        abort(404)
    if request.method == "DELETE":
        p.unlink()
        return jsonify(ok=True)
    return send_file(p, as_attachment=True, download_name=p.name, conditional=True)


@app.route("/")
def index():
    return (f"PiCam Remote Pi bridge {VERSION} on {socket.gethostname()}. "
            "Point PiCam Remote on your Cardputer at this host, port "
            f"{PORT}.\n"), 200, {"Content-Type": "text/plain"}


if __name__ == "__main__":
    print(f"[picam-remote] {VERSION} on :{PORT} (auth {'on' if TOKEN else 'off'})")
    app.run(host="0.0.0.0", port=PORT, threaded=True)
PYEOF

echo "[4/6] Installing the root helper (whitelisted actions only)..."
sudo tee /usr/local/bin/picam-remote-ctl > /dev/null << 'CTLEOF'
#!/bin/bash
# picam-remote-ctl — the ONLY thing the PiCam Remote bridge may run as root.
# Every service, config key and value is whitelisted here.
#
#   picam-remote-ctl svc <start|stop|restart> <service>
#   picam-remote-ctl set <fx|tl> <KEY> <VALUE>
#   picam-remote-ctl wifi <ap|client>
#   picam-remote-ctl power <reboot|poweroff>
set -euo pipefail

die() { echo "$*" >&2; exit 1; }

SERVICES="mediamtx picam-fx picam-hub picam-recorder tv-dashboard"
FX_CONF=/etc/default/picam-fx
TL_CONF=/etc/default/picam-timelapse

num_in() { # value min max
  [[ "$1" =~ ^-?[0-9]+(\.[0-9]+)?$ ]] || die "not a number: $1"
  awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN{exit !(v>=lo && v<=hi)}' || die "$1 out of range $2..$3"
}
one_of() { # value list...
  local v="$1"; shift
  for x in "$@"; do [ "$v" = "$x" ] && return 0; done
  die "must be one of: $*"
}

set_key() { # file key value
  local f="$1" k="$2" v="$3"
  [ -f "$f" ] || die "$f missing (run the matching pi-setup script first)"
  if grep -qE "^${k}=" "$f"; then
    # keep any trailing comment on the line
    sed -i -E "s|^(${k}=)[^ #]*|\1${v}|" "$f"
  else
    echo "${k}=${v}" >> "$f"
  fi
}

case "${1:-}" in
  svc)
    one_of "${2:-}" start stop restart
    one_of "${3:-}" $SERVICES
    systemctl "$2" "$3"
    ;;
  set)
    which="${2:-}"; key="${3:-}"; val="${4:-}"
    if [ "$which" = fx ]; then
      case "$key" in
        AWB) one_of "$val" auto incandescent tungsten fluorescent indoor daylight cloudy ;;
        SATURATION|CONTRAST|SHARPNESS) num_in "$val" 0 2 ;;
        BRIGHTNESS) num_in "$val" -1 1 ;;
        EV) num_in "$val" -10 10 ;;
        DENOISE) one_of "$val" auto off cdn_off cdn_fast cdn_hq ;;
        FX_K1|FX_K2) num_in "$val" -1 1 ;;
        FPS) num_in "$val" 1 30 ;;
        OUT_SIZE) num_in "$val" 240 1080 ;;
        BITRATE) one_of "$val" 1M 2M 3M 4M 6M ;;
        ROTATE) one_of "$val" 0 90 180 270 ;;
        *) die "fx key not allowed: $key" ;;
      esac
      set_key "$FX_CONF" "$key" "$val"
    elif [ "$which" = tl ]; then
      case "$key" in
        INTERVAL) [[ "$val" =~ ^[0-9]+$ ]] && num_in "$val" 1 3600 || die "bad interval" ;;
        OUT_FPS) num_in "$val" 1 60 ;;
        *) die "tl key not allowed: $key" ;;
      esac
      set_key "$TL_CONF" "$key" "$val"
    else
      die "set fx|tl"
    fi
    echo "ok $which $key=$val"
    ;;
  wifi)
    one_of "${2:-}" ap client
    [ -x /usr/local/bin/picam-wifi ] || die "run setup-picam-ap.sh first"
    /usr/local/bin/picam-wifi "$2"
    ;;
  power)
    one_of "${2:-}" reboot poweroff
    systemctl "$2"
    ;;
  *)
    die "usage: picam-remote-ctl svc|set|power ..."
    ;;
esac
CTLEOF
sudo chmod 0755 /usr/local/bin/picam-remote-ctl
SUDO_LINE="$USER ALL=(root) NOPASSWD: /usr/local/bin/picam-remote-ctl"
echo "$SUDO_LINE" | sudo tee /etc/sudoers.d/picam-remote > /dev/null
sudo chmod 0440 /etc/sudoers.d/picam-remote
sudo visudo -c -f /etc/sudoers.d/picam-remote >/dev/null || {
  echo "sudoers rule invalid, removing"; sudo rm -f /etc/sudoers.d/picam-remote; }

echo "[5/6] Advertising on the LAN (mDNS) so PiCam Remote finds the Pi by itself..."
sudo tee /etc/avahi/services/picam-remote.service > /dev/null << AVEOF
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">PiCam on %h</name>
  <service>
    <type>_picam._tcp</type>
    <port>$PORT</port>
    <txt-record>v=1</txt-record>
  </service>
</service-group>
AVEOF
sudo systemctl restart avahi-daemon 2>/dev/null || true

echo "[+] Adding rotation support (ROTATE in /etc/default/picam-fx)..."
# Camera mounted sideways or upside down? ROTATE=0|90|180|270.
# 180 is done free in the Pi's ISP; 90/270 use an ffmpeg transpose.
if [ -f /etc/default/picam-fx ] && ! grep -qE '^ROTATE=' /etc/default/picam-fx; then
  printf '\n# Rotation of the whole picture: 0 90 180 270 (set from the Cardputer too)\nROTATE=0\n' \
    | sudo tee -a /etc/default/picam-fx > /dev/null
fi
if [ -f "$HOME/picam-fx.sh" ] && ! grep -q 'picam-remote rotation' "$HOME/picam-fx.sh"; then
  cp "$HOME/picam-fx.sh" "$HOME/picam-fx.sh.bak"
  python3 - "$HOME/picam-fx.sh" << 'ROTPY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("source /etc/default/picam-fx\n",
    "source /etc/default/picam-fx\n"
    "# picam-remote rotation\n"
    "ROT_ISP=\"\"; ROT_VF=\"\"\n"
    "case \"${ROTATE:-0}\" in 180) ROT_ISP=\"--rotation 180\";; 90) ROT_VF=\",transpose=1\";; 270) ROT_VF=\",transpose=2\";; esac\n", 1)
s = s.replace("    --nopreview \\\n    -o - \\\n", "    --nopreview $ROT_ISP \\\n    -o - \\\n", 1)
s = s.replace("setsar=1\" \\\n", "setsar=1${ROT_VF}\" \\\n", 1)
open(p, "w").write(s)
ROTPY
  grep -q 'ROT_ISP \\' "$HOME/picam-fx.sh" && grep -q 'ROT_VF}' "$HOME/picam-fx.sh" \
    || { echo "  picam-fx.sh looks different than expected, restored it (rotation off)"; mv "$HOME/picam-fx.sh.bak" "$HOME/picam-fx.sh"; }
fi
if [ -f /usr/local/bin/timelapse ] && ! grep -q 'picam-remote rotation' /usr/local/bin/timelapse; then
  sudo cp /usr/local/bin/timelapse /usr/local/bin/timelapse.bak
  sudo python3 - /usr/local/bin/timelapse << 'ROTPY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("source /etc/default/picam-timelapse\n",
    "source /etc/default/picam-timelapse\n"
    "# picam-remote rotation (follows ROTATE in /etc/default/picam-fx)\n"
    "ROTATE=$(grep -E '^ROTATE=' /etc/default/picam-fx 2>/dev/null | cut -d= -f2 | cut -d' ' -f1)\n"
    "ROT_ISP=\"\"; ROT_VF=\"\"\n"
    "case \"${ROTATE:-0}\" in 180) ROT_ISP=\"--rotation 180\";; 90) ROT_VF=\"transpose=1,\";; 270) ROT_VF=\"transpose=2,\";; esac\n", 1)
s = s.replace("--ev \"$TL_EV\" --nopreview \\\n", "--ev \"$TL_EV\" --nopreview $ROT_ISP \\\n", 1)
s = s.replace("-vf \"$VF\"", "-vf \"${ROT_VF}$VF\"", 1)
open(p, "w").write(s)
ROTPY
fi

echo "[6/6] systemd service..."
sudo tee /etc/systemd/system/picam-remote.service > /dev/null << SVCEOF
[Unit]
Description=PiCam Remote Pi bridge (picam-remote)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$USER
WorkingDirectory=$APP_DIR
ExecStart=/usr/bin/python3 $APP_DIR/app.py
Restart=always
RestartSec=3
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
SVCEOF
sudo systemctl daemon-reload
sudo systemctl enable picam-remote >/dev/null 2>&1
sudo systemctl restart picam-remote
sleep 2

IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║   ✅ PiCam Remote bridge online                   ║"
echo "╠══════════════════════════════════════════════════╣"
echo "  Address : $IP  port $PORT  ($(hostname).local)"
if [ -n "$TOKEN" ]; then
echo "  Token   : $TOKEN"
echo "            (PiCam Remote > Settings > Pi token)"
else
echo "  Token   : none (auth off)"
fi
echo ""
echo "  Test    : curl http://$IP:$PORT/api/ping"
echo "  Apps    : drop .bin files in ~/cardputer-apps"
echo "  Logs    : journalctl -u picam-remote -f"
echo "╚══════════════════════════════════════════════════╝"
