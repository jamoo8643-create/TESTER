// =====================================================================
//  PiCam Remote  —  a Raspberry Pi camera controller for the Cardputer
// =====================================================================
//  Talks to the "picam-remote" bridge (setup-cardputer-remote.sh) on the
//  Pi, port 8091. Live view, photos, clips, timelapse, image tuning,
//  camera mode, gallery with download-to-SD, Pi health, stream QR codes.
//
//  Keys (everywhere):  ; . , /  = up down left right  (arrow keys)
//                      Enter = select     ` (Esc) or Del = back
// =====================================================================
#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <sys/time.h>
#include <vector>
#include <functional>

#define APP_VERSION "1.0.1"

// ------------------------------------------------------------- theme
// HAL-red on near-black, matching the pi-setup recorder web UI.
static const uint16_t C_BG = 0x0841;      // #0a0a0c-ish
static const uint16_t C_PANEL = 0x18E4;   // #1c1c22
static const uint16_t C_LINE = 0x3166;    // #2a2a35
static const uint16_t C_TEXT = 0xF79B;    // cream
static const uint16_t C_DIM = 0x8410;     // grey
static const uint16_t C_HAL = 0xF9C6;     // HAL red #ff3b30
static const uint16_t C_OK = 0x07AE;      // green
static const uint16_t C_WARN = 0xFD20;    // amber
static const uint16_t C_BLUE = 0x4D7E;    // #4dabf7

static const int W = 240, H = 135, HDR = 16, FTR = 11;

M5Canvas cv(&M5Cardputer.Display);
Preferences prefs;
bool sdOk = false;

// ------------------------------------------------------------- keys
struct Keys {
  bool any = false, up = false, down = false, left = false, right = false;
  bool enter = false, back = false, del = false, tab = false, fn = false;
  char ch = 0;  // printable char (for shortcuts / text entry)
};

static Keys parseKeys() {
  Keys k;
  auto st = M5Cardputer.Keyboard.keysState();
  k.enter = st.enter;
  k.del = st.del;
  k.tab = st.tab;
  k.fn = st.fn;
  if (st.space) k.ch = ' ';
  for (char c : st.word) {
    if (c == ';') k.up = true;
    else if (c == '.') k.down = true;
    else if (c == ',') k.left = true;
    else if (c == '/') k.right = true;
    else if (c == '`') k.back = true;
    k.ch = c;
  }
  k.any = k.enter || k.del || k.tab || k.ch || k.up || k.down || k.left || k.right || k.back;
  return k;
}

// Edge-triggered keys with auto-repeat for the arrows.
static Keys readKeys() {
  static Keys held;
  static uint32_t pressT = 0, repT = 0;
  M5Cardputer.update();
  uint32_t now = millis();
  if (M5.BtnA.wasPressed()) {  // G0 button doubles as Enter
    Keys k; k.any = k.enter = true; return k;
  }
  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      held = parseKeys();
      pressT = repT = now;
      return held;
    }
    held = Keys();
    return Keys();
  }
  if (held.any && (held.up || held.down || held.left || held.right) &&
      M5Cardputer.Keyboard.isPressed() && now - pressT > 380 && now - repT > 85) {
    repT = now;
    return held;
  }
  return Keys();
}

static bool isBack(const Keys& k) { return k.back || k.del; }

// ------------------------------------------------------------- settings
struct Net { String ssid, pass; };
struct Settings {
  std::vector<Net> nets;
  String piHost;       // "" = auto discover
  String lastHost;     // last address that answered
  uint16_t piPort = 8091;
  String token;
  uint8_t bright = 160;
  uint8_t quality = 1; // 0 smooth/low, 1 balanced, 2 sharp
  uint8_t rot = 1;     // display rotation (1 = normal Cardputer landscape)
} cfg;

static void saveSettings() {
  prefs.begin("picam", false);
  prefs.putUChar("nnet", cfg.nets.size());
  for (size_t i = 0; i < cfg.nets.size() && i < 8; i++) {
    prefs.putString(("s" + String(i)).c_str(), cfg.nets[i].ssid);
    prefs.putString(("p" + String(i)).c_str(), cfg.nets[i].pass);
  }
  prefs.putString("host", cfg.piHost);
  prefs.putString("last", cfg.lastHost);
  prefs.putUShort("port", cfg.piPort);
  prefs.putString("token", cfg.token);
  prefs.putUChar("bright", cfg.bright);
  prefs.putUChar("qual", cfg.quality);
  prefs.putUChar("rot", cfg.rot);
  prefs.end();
}

static void addNet(const String& ssid, const String& pass) {
  for (auto& n : cfg.nets) if (n.ssid == ssid) { n.pass = pass; return; }
  if (cfg.nets.size() >= 8) cfg.nets.erase(cfg.nets.begin());
  cfg.nets.push_back({ssid, pass});
}

static void loadSettings() {
  prefs.begin("picam", true);
  uint8_t n = prefs.getUChar("nnet", 0);
  for (uint8_t i = 0; i < n && i < 8; i++)
    cfg.nets.push_back({prefs.getString(("s" + String(i)).c_str(), ""),
                        prefs.getString(("p" + String(i)).c_str(), "")});
  cfg.piHost = prefs.getString("host", "");
  cfg.lastHost = prefs.getString("last", "");
  cfg.piPort = prefs.getUShort("port", 8091);
  cfg.token = prefs.getString("token", "");
  cfg.bright = prefs.getUChar("bright", 160);
  cfg.quality = prefs.getUChar("qual", 1);
  cfg.rot = prefs.getUChar("rot", 1) & 3;
  prefs.end();
  // The Pi's own fallback network (setup-picam-ap.sh) is always known.
  bool hasAp = false;
  for (auto& x : cfg.nets) if (x.ssid == "PiCam") hasAp = true;
  if (!hasAp) cfg.nets.push_back({"PiCam", "picam1234"});
}

// Optional SD file /picam.cfg, easier than typing on the tiny keyboard:
//   wifi=MyHotspot,yourpassword
//   pi=10.42.0.1          (leave out for auto-discovery)
//   token=abcd1234
// It's applied once, then renamed to /picam.cfg.done.
static bool importSdConfig() {
  if (!sdOk || !SD.exists("/picam.cfg")) return false;
  File f = SD.open("/picam.cfg");
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("#") || line.indexOf('=') < 0) continue;
    String key = line.substring(0, line.indexOf('='));
    String val = line.substring(line.indexOf('=') + 1);
    key.trim(); val.trim();
    if (key == "wifi") {
      int c = val.indexOf(',');
      if (c > 0) addNet(val.substring(0, c), val.substring(c + 1));
      else addNet(val, "");
    } else if (key == "pi") cfg.piHost = val;
    else if (key == "port") cfg.piPort = val.toInt();
    else if (key == "token") cfg.token = val;
  }
  f.close();
  SD.remove("/picam.cfg.done");
  SD.rename("/picam.cfg", "/picam.cfg.done");
  saveSettings();
  return true;
}

// ------------------------------------------------------------- pi state
struct PiState {
  bool online = false;
  String host;
  String mode = "?";       // fx | hub | live | timelapse | off
  bool rec = false;
  int recElapsed = 0;
  bool tl = false, tlRendering = false;
  int tlFrames = 0, tlInterval = 3;
  float temp = 0;
  int mem = 0, diskPct = 0;
  float diskFree = 0, load = 0;
  String throttled;
  uint32_t fetched = 0;
} pi;

static String baseUrl() { return "http://" + pi.host + ":" + String(cfg.piPort); }

static int httpJson(const char* method, const String& path, const String& body,
                    JsonDocument& doc, uint32_t timeoutMs = 6000) {
  if (WiFi.status() != WL_CONNECTED || pi.host.isEmpty()) return -1;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(1500);
  if (!http.begin(client, baseUrl() + path)) return -1;
  if (cfg.token.length()) http.addHeader("X-Deck-Token", cfg.token);
  int code;
  if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body.length() ? body : String("{}"));
  } else if (strcmp(method, "DELETE") == 0) {
    code = http.sendRequest("DELETE");
  } else {
    code = http.GET();
  }
  if (code > 0) {
    String s = http.getString();
    doc.clear();
    deserializeJson(doc, s);
  }
  http.end();
  return code;
}

static bool apiGet(const String& path, JsonDocument& doc, uint32_t t = 6000) {
  return httpJson("GET", path, "", doc, t) == 200;
}
static bool apiPost(const String& path, const String& body, JsonDocument& doc, uint32_t t = 15000) {
  int c = httpJson("POST", path, body, doc, t);
  return c == 200 && (doc["ok"] | false);
}

// First useful message from a bridge reply ("error", else "text").
static String errOf(JsonDocument& d, const char* dflt = "failed") {
  const char* e = d["error"] | (const char*)nullptr;
  if (!e) e = d["text"] | dflt;
  return String(e).substring(0, 34);
}

static String urlEncode(const String& s) {
  String o;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '/') o += c;
    else { o += '%'; o += hex[(c >> 4) & 15]; o += hex[c & 15]; }
  }
  return o;
}

static bool refreshStatus() {
  JsonDocument d;
  if (!apiGet("/api/status", d, 5000)) { pi.online = false; return false; }
  pi.online = true;
  pi.fetched = millis();
  pi.mode = d["mode"] | "?";
  pi.rec = d["rec"]["active"] | false;
  pi.recElapsed = d["rec"]["elapsed"] | 0;
  pi.tl = d["tl"]["active"] | false;
  pi.tlRendering = d["tl"]["rendering"] | false;
  pi.tlFrames = d["tl"]["frames"] | 0;
  pi.tlInterval = d["tl"]["interval"] | 3;
  pi.temp = d["sys"]["temp"] | 0.0f;
  pi.mem = d["sys"]["mem"] | 0;
  pi.diskPct = d["sys"]["disk_pct"] | 0;
  pi.diskFree = d["sys"]["disk_free_gb"] | 0.0f;
  pi.load = d["sys"]["load"] | 0.0f;
  pi.throttled = d["sys"]["throttled"] | "";
  // Borrow the Pi's clock so SD filenames get real timestamps.
  time_t t = d["time"] | 0;
  if (t > 1700000000) { struct timeval tv = {t, 0}; settimeofday(&tv, nullptr); }
  return true;
}

static String modeName(const String& m) {
  if (m == "fx") return "Livestream (fisheye)";
  if (m == "live") return "Livestream";
  if (m == "hub") return "PiCam Hub";
  if (m == "timelapse") return "Timelapse";
  if (m == "off") return "Camera off";
  return m;
}

static String fmtDur(int s) {
  char b[16];
  if (s >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
  return b;
}

static String fmtSize(uint32_t b) {
  char s[16];
  if (b >= 1048576) snprintf(s, sizeof s, "%.1fMB", b / 1048576.0);
  else snprintf(s, sizeof s, "%uKB", (unsigned)(b / 1024));
  return s;
}

// ------------------------------------------------------------- drawing
static void drawWifiBars(int x, int y) {
  int bars = 0;
  if (WiFi.status() == WL_CONNECTED) {
    int r = WiFi.RSSI();
    bars = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
  }
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 2;
    cv.fillRect(x + i * 4, y + 10 - h, 3, h, i < bars ? C_TEXT : C_LINE);
  }
}

static void drawHeader(const String& title) {
  cv.fillRect(0, 0, W, HDR, C_PANEL);
  cv.drawFastHLine(0, HDR - 1, W, C_HAL);
  cv.setFont(&fonts::Font0);
  cv.setTextSize(1);
  // HAL eye
  cv.fillCircle(7, 7, 4, C_HAL);
  cv.fillCircle(7, 7, 1, C_WARN);
  cv.setTextColor(C_TEXT);
  cv.setCursor(15, 4);
  cv.print(title);
  int x = W - 4;
  int bat = M5.Power.getBatteryLevel();
  String bs = String(bat < 0 ? 0 : bat) + "%";
  x -= cv.textWidth(bs);
  cv.setTextColor(bat >= 0 && bat < 20 ? C_HAL : C_DIM);
  cv.setCursor(x, 4);
  cv.print(bs);
  x -= 20;
  drawWifiBars(x, 2);
  x -= 10;
  cv.fillCircle(x, 7, 3, pi.online ? C_OK : C_HAL);
  if (pi.rec) { x -= 26; cv.setTextColor(C_HAL); cv.setCursor(x, 4); cv.print("REC"); }
  else if (pi.tl) { x -= 20; cv.setTextColor(C_WARN); cv.setCursor(x, 4); cv.print("TL"); }
}

static void drawFooter(const String& hint) {
  cv.fillRect(0, H - FTR, W, FTR, C_BG);
  cv.drawFastHLine(0, H - FTR, W, C_LINE);
  cv.setFont(&fonts::Font0);
  cv.setTextColor(C_DIM);
  cv.setCursor(3, H - FTR + 2);
  cv.print(hint);
}

static void push() { cv.pushSprite(0, 0); }

// Rotation 1 is the normal Cardputer landscape. If the screen comes up
// turned, 'o' on the home screen (or Settings > Rotate screen) steps
// through all four orientations and remembers the choice.
static void applyRotation() {
  M5Cardputer.Display.setRotation(cfg.rot & 3);
  M5Cardputer.Display.fillScreen(0);
}

static void cycleRotation() {
  cfg.rot = (cfg.rot + 3) & 3;  // turn 90 degrees anticlockwise
  applyRotation();
  saveSettings();
}

static void centerText(const String& s, int y, uint16_t col, const lgfx::IFont* f = &fonts::Font2) {
  cv.setFont(f);
  cv.setTextColor(col);
  cv.setCursor((W - cv.textWidth(s)) / 2, y);
  cv.print(s);
}

static void toast(const String& msg, uint16_t col = C_OK, int ms = 1100) {
  cv.setFont(&fonts::Font2);
  int w = min(W - 10, (int)cv.textWidth(msg) + 16);
  int x = (W - w) / 2, y = H / 2 - 13;
  cv.fillRoundRect(x, y, w, 26, 6, C_PANEL);
  cv.drawRoundRect(x, y, w, 26, 6, col);
  cv.setTextColor(col);
  cv.setClipRect(x + 4, y, w - 8, 26);
  cv.setCursor(x + 8, y + 5);
  cv.print(msg);
  cv.clearClipRect();
  push();
  if (ms > 0) delay(ms);
}

static void busy(const String& msg) { toast(msg, C_BLUE, 0); }

static bool confirm(const String& question) {
  cv.fillScreen(C_BG);
  drawHeader("Confirm");
  centerText(question, 45, C_TEXT);
  centerText("Enter = yes    Esc = no", 75, C_DIM, &fonts::Font0);
  push();
  while (true) {
    Keys k = readKeys();
    if (k.enter || k.ch == 'y') return true;
    if (isBack(k) || k.ch == 'n') return false;
    delay(10);
  }
}

// Text entry. Enter = OK, Tab = cancel (Esc is a typeable key here).
static bool textInput(const String& title, String& value, bool secret = false) {
  String v = value;
  uint32_t blink = millis();
  bool dirty = true;
  while (true) {
    Keys k = readKeys();
    if (k.any) {
      dirty = true;
      if (k.enter) { value = v; return true; }
      if (k.tab) return false;
      if (k.del) { if (v.length()) v.remove(v.length() - 1); }
      else {
        // in text mode the arrow/esc chars are just characters
        auto st = M5Cardputer.Keyboard.keysState();
        for (char c : st.word) if (v.length() < 63) v += c;
      }
    }
    if (dirty || millis() - blink > 500) {
      if (millis() - blink > 1000) blink = millis();
      dirty = false;
      cv.fillScreen(C_BG);
      drawHeader(title);
      cv.fillRoundRect(6, 40, W - 12, 28, 5, C_PANEL);
      cv.drawRoundRect(6, 40, W - 12, 28, 5, C_HAL);
      cv.setFont(&fonts::Font2);
      cv.setTextColor(C_TEXT);
      String shown = v;
      if (secret && shown.length()) { shown = ""; for (size_t i = 0; i + 1 < v.length(); i++) shown += '*'; shown += v[v.length() - 1]; }
      while (cv.textWidth(shown) > W - 30 && shown.length()) shown.remove(0, 1);
      cv.setCursor(12, 46);
      cv.print(shown);
      if ((millis() - blink) < 500) cv.fillRect(12 + cv.textWidth(shown) + 1, 45, 2, 17, C_HAL);
      drawFooter("Enter OK   Del erase   Tab cancel");
      push();
    }
    delay(10);
  }
}

// ------------------------------------------------------------- menu
struct Item {
  String label;
  String value;
  uint16_t color;
  Item(const String& l, const String& v, uint16_t c = C_TEXT) : label(l), value(v), color(c) {}
};

// Generic scrolling list. Returns selected index, or -1 on back.
// onKey lets a screen add shortcuts; return true from it to redraw.
// onTick runs ~every second (status refresh), returns true to redraw.
static int listMenu(const String& title, std::vector<Item>& items, int& sel, const String& hint,
                    std::function<bool(const Keys&, int)> onKey = nullptr,
                    std::function<bool()> onTick = nullptr,
                    std::function<void()> refill = nullptr) {
  const int rowH = 18, rows = (H - HDR - FTR) / rowH;
  int top = 0;
  bool dirty = true;
  uint32_t lastTick = millis();
  while (true) {
    if (items.empty()) sel = 0;
    else sel = constrain(sel, 0, (int)items.size() - 1);
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    if (dirty) {
      dirty = false;
      cv.fillScreen(C_BG);
      drawHeader(title);
      cv.setFont(&fonts::Font2);
      for (int r = 0; r < rows && top + r < (int)items.size(); r++) {
        int i = top + r, y = HDR + 1 + r * rowH;
        bool s = (i == sel);
        if (s) {
          cv.fillRect(0, y, W, rowH, C_PANEL);
          cv.fillRect(0, y, 3, rowH, C_HAL);
        }
        cv.setTextColor(s ? C_TEXT : items[i].color == C_TEXT ? C_DIM : items[i].color);
        if (s && items[i].color != C_TEXT) cv.setTextColor(items[i].color);
        cv.setCursor(8, y + 1);
        cv.print(items[i].label);
        if (items[i].value.length()) {
          cv.setTextColor(s ? C_BLUE : C_DIM);
          cv.setCursor(W - 6 - cv.textWidth(items[i].value), y + 1);
          cv.print(items[i].value);
        }
      }
      if ((int)items.size() > rows) {  // scrollbar
        int track = H - HDR - FTR - 2;
        int bh = max(6, track * rows / (int)items.size());
        int by = HDR + 1 + (track - bh) * top / max(1, (int)items.size() - rows);
        cv.fillRect(W - 2, by, 2, bh, C_LINE);
      }
      if (items.empty()) centerText("(empty)", 60, C_DIM);
      drawFooter(hint);
      push();
    }
    Keys k = readKeys();
    if (k.any) {
      dirty = true;
      if (k.up) sel = sel > 0 ? sel - 1 : (int)items.size() - 1;
      else if (k.down) sel = sel + 1 < (int)items.size() ? sel + 1 : 0;
      else if (k.left) sel = max(0, sel - rows);
      else if (k.right) sel = min((int)items.size() - 1, sel + rows);
      else if (k.enter && !items.empty()) return sel;
      else if (isBack(k)) return -1;
      else if (onKey) onKey(k, sel);
      if (refill) refill();
    }
    if (onTick && millis() - lastTick > 1000) {
      lastTick = millis();
      if (onTick()) { dirty = true; if (refill) refill(); }
    }
    delay(8);
  }
}

// Pick one string from a list. Returns index or -1.
static int choose(const String& title, const std::vector<String>& opts, int cur = 0) {
  std::vector<Item> it;
  for (auto& o : opts) it.push_back({o, ""});
  int sel = max(0, cur);
  return listMenu(title, it, sel, "Enter choose   Esc back");
}

// ------------------------------------------------------------- wifi
static bool connectWifi(bool verbose) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("picam-remote");
  if (verbose) busy("Scanning WiFi...");
  int n = WiFi.scanNetworks();
  // saved networks in saved order, best signal wins among equals
  for (auto& net : cfg.nets) {
    bool seen = false;
    for (int i = 0; i < n; i++) if (WiFi.SSID(i) == net.ssid) seen = true;
    if (!seen) continue;
    if (verbose) busy("Joining " + net.ssid + "...");
    WiFi.begin(net.ssid.c_str(), net.pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(100);
    if (WiFi.status() == WL_CONNECTED) { WiFi.scanDelete(); return true; }
    WiFi.disconnect();
  }
  WiFi.scanDelete();
  return false;
}

// Is there a CardDeck/picam-remote bridge at this address?
static bool probe(const IPAddress& ip, uint16_t port, int timeoutMs) {
  WiFiClient c;
  if (!c.connect(ip, port, timeoutMs)) return false;
  c.print("GET /api/ping HTTP/1.0\r\nHost: pi\r\n\r\n");
  uint32_t t0 = millis();
  String resp;
  while (millis() - t0 < 1500 && resp.length() < 600) {
    while (c.available()) resp += (char)c.read();
    if (!c.connected() && !c.available()) break;
    delay(5);
  }
  c.stop();
  return resp.indexOf("picam-remote") >= 0;
}

static bool tryHost(const String& h) {
  IPAddress ip;
  if (!ip.fromString(h)) {
    if (!WiFi.hostByName(h.c_str(), ip)) return false;
  }
  if (probe(ip, cfg.piPort, 800)) {
    pi.host = ip.toString();
    return true;
  }
  return false;
}

// Find the Pi: manual host, last good, gateway (= Pi on its own PiCam AP),
// mDNS (_picam._tcp), then a sweep of the local /24 (phone hotspots).
static bool discoverPi(bool verbose) {
  pi.online = false;
  if (cfg.piHost.length()) {
    if (verbose) busy("Pi at " + cfg.piHost + "...");
    return tryHost(cfg.piHost);
  }
  if (cfg.lastHost.length() && tryHost(cfg.lastHost)) return true;
  if (verbose) busy("Looking for the Pi...");
  if (tryHost(WiFi.gatewayIP().toString())) goto found;
  {
    static bool mdnsUp = false;
    if (!mdnsUp) mdnsUp = MDNS.begin("picam-remote");
    int n = MDNS.queryService("picam", "tcp");
    for (int i = 0; i < n; i++) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
      IPAddress ip = MDNS.address(i);
#else
      IPAddress ip = MDNS.IP(i);
#endif
      cfg.piPort = MDNS.port(i);
      if (probe(ip, cfg.piPort, 800)) { pi.host = ip.toString(); goto found; }
    }
    if (cfg.piPort == 0) cfg.piPort = 8091;
  }
  if (tryHost("raspberrypi.local")) goto found;
  {
    IPAddress me = WiFi.localIP();
    for (int i = 1; i < 255; i++) {
      if (i == me[3]) continue;
      if (verbose && i % 16 == 1) busy("Scanning network " + String(i * 100 / 254) + "%");
      IPAddress ip(me[0], me[1], me[2], i);
      if (probe(ip, cfg.piPort, 70)) { pi.host = ip.toString(); goto found; }
    }
  }
  return false;
found:
  cfg.lastHost = pi.host;
  saveSettings();
  return true;
}

static bool ensureOnline(bool verbose = true) {
  if (!connectWifi(verbose)) {
    if (verbose) toast("No saved WiFi in range", C_HAL, 1500);
    return false;
  }
  if (pi.host.isEmpty() || !refreshStatus()) {
    if (!discoverPi(verbose)) {
      if (verbose) toast("Pi not found", C_HAL, 1500);
      return false;
    }
    if (!refreshStatus()) {
      JsonDocument d;
      int code = httpJson("GET", "/api/status", "", d);
      if (verbose) toast(code == 401 ? "Wrong token" : "Pi not answering", C_HAL, 1500);
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------- SD
static String sdName(const char* prefix, const char* ext) {
  time_t now = time(nullptr);
  char b[64];
  if (now > 1700000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tmv);
  } else {
    snprintf(b, sizeof b, "%lu", (unsigned long)millis());
  }
  return String("/DCIM/PiCam/") + prefix + b + ext;
}

static void ensureDcim() {
  if (!SD.exists("/DCIM")) SD.mkdir("/DCIM");
  if (!SD.exists("/DCIM/PiCam")) SD.mkdir("/DCIM/PiCam");
}

// ------------------------------------------------------------- live view
static uint8_t* jpgBuf = nullptr;
static const size_t JPG_MAX = 48 * 1024;

static bool readLine(WiFiClient& c, String& out, uint32_t timeoutMs) {
  out = "";
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (c.available()) {
      char ch = c.read();
      if (ch == '\n') { out.trim(); return true; }
      if (out.length() < 200) out += ch;
    }
    if (!c.connected()) return false;
    delay(1);
  }
  return false;
}

static bool readExact(WiFiClient& c, uint8_t* buf, size_t n, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < n && millis() - t0 < timeoutMs) {
    int a = c.available();
    if (a > 0) {
      int r = c.read(buf + got, min((size_t)a, n - got));
      if (r > 0) got += r;
    } else {
      if (!c.connected()) return false;
      delay(1);
    }
  }
  return got == n;
}

static void setView(float zoom, float px, float py) {
  static const int fps[] = {12, 8, 5};
  static const int q[] = {10, 7, 4};
  JsonDocument d;
  char body[128];
  snprintf(body, sizeof body,
           "{\"w\":240,\"h\":135,\"fps\":%d,\"q\":%d,\"zoom\":%.2f,\"px\":%.2f,\"py\":%.2f}",
           fps[cfg.quality % 3], q[cfg.quality % 3], zoom, px, py);
  apiPost("/api/view", body, d, 4000);
}

static void screenTimelapse();
static void screenImage();

static void screenLive() {
  if (!ensureOnline()) return;
  if (!jpgBuf) jpgBuf = (uint8_t*)malloc(JPG_MAX);
  if (!jpgBuf) { toast("Out of memory", C_HAL); return; }

  float zoom = 1.0f, px = 0.5f, py = 0.5f;
  bool hud = true;
  setView(zoom, px, py);

  WiFiClient c;
  size_t lastLen = 0;
  uint32_t frames = 0, fpsT = millis(), lastFrame = 0, lastStatus = 0;
  float fps = 0;
  String note;
  uint32_t noteT = 0;

  auto openStream = [&]() -> bool {
    c.stop();
    IPAddress ip;
    ip.fromString(pi.host);
    if (!c.connect(ip, cfg.piPort, 2000)) return false;
    c.print("GET /api/live.mjpeg HTTP/1.1\r\nHost: " + pi.host + "\r\n" +
            (cfg.token.length() ? "X-Deck-Token: " + cfg.token + "\r\n" : String("")) +
            "Connection: close\r\n\r\n");
    String line;
    if (!readLine(c, line, 4000) || line.indexOf("200") < 0) return false;
    while (readLine(c, line, 2000) && line.length()) {}
    return true;
  };

  auto drawOverlay = [&]() {
    if (!hud) return;
    cv.setFont(&fonts::Font0);
    // top strip
    cv.fillRect(0, 0, W, 11, 0x0000);
    cv.setTextColor(C_TEXT);
    cv.setCursor(3, 2);
    cv.printf("%s  %.1ffps  %.0fC", modeName(pi.mode).c_str(), fps, pi.temp);
    if (zoom > 1.01f) { cv.setTextColor(C_BLUE); cv.setCursor(W - 70, 2); cv.printf("%.1fx", zoom); }
    if (pi.rec) {
      cv.fillCircle(W - 40, 5, 3, (millis() / 500) % 2 ? C_HAL : 0x0000);
      cv.setTextColor(C_HAL);
      cv.setCursor(W - 34, 2);
      cv.print(fmtDur(pi.recElapsed + (millis() - pi.fetched) / 1000));
    } else if (pi.tl) {
      cv.setTextColor(C_WARN);
      cv.setCursor(W - 46, 2);
      cv.printf("TL %d", pi.tlFrames);
    }
    // bottom hints
    cv.fillRect(0, H - 10, W, 10, 0x0000);
    cv.setTextColor(C_DIM);
    cv.setCursor(3, H - 9);
    cv.print("ent photo r rec t tl z/x zoom s save h hud");
    if (note.length() && millis() - noteT < 1800) {
      cv.setFont(&fonts::Font2);
      int w = cv.textWidth(note) + 12;
      cv.fillRoundRect((W - w) / 2, 52, w, 22, 5, 0x0000);
      cv.drawRoundRect((W - w) / 2, 52, w, 22, 5, C_HAL);
      cv.setTextColor(C_TEXT);
      cv.setCursor((W - w) / 2 + 6, 55);
      cv.print(note);
    }
  };
  auto say = [&](const String& s) { note = s; noteT = millis(); };

  cv.fillScreen(0);
  centerText("Connecting to camera...", 60, C_DIM);
  push();
  bool streaming = openStream();

  while (true) {
    Keys k = readKeys();
    if (k.any) {
      if (isBack(k)) break;
      bool viewChanged = false;
      if (k.enter || k.ch == ' ') {
        say("Capturing...");
        JsonDocument d;
        cv.fillScreen(0); if (lastLen) cv.drawJpg(jpgBuf, lastLen, 0, 0); drawOverlay(); push();
        if (apiPost("/api/photo", "", d, 25000)) say(String("Saved ") + (const char*)(d["file"] | ""));
        else say(String("Photo: ") + (const char*)(d["error"] | "failed"));
      } else if (k.ch == 'r') {
        JsonDocument d;
        say(pi.rec ? "Stopping..." : "Starting rec...");
        if (apiPost("/api/record/toggle", "", d, 15000)) { refreshStatus(); say(pi.rec ? "Recording" : "Clip saved"); }
        else say(String("Rec: ") + (const char*)(d["error"] | "failed"));
        lastStatus = millis();
      } else if (k.ch == 't') {
        c.stop(); screenTimelapse(); setView(zoom, px, py); streaming = openStream();
      } else if (k.ch == 'i') {
        c.stop(); screenImage(); setView(zoom, px, py); streaming = openStream();
      } else if (k.ch == 'z' || k.ch == '=') { zoom = min(8.0f, zoom * 1.5f); viewChanged = true; }
      else if (k.ch == 'x' || k.ch == '-') { zoom = max(1.0f, zoom / 1.5f); if (zoom < 1.05f) { zoom = 1; px = py = 0.5f; } viewChanged = true; }
      else if (zoom > 1.01f && (k.up || k.down || k.left || k.right)) {
        float step = 0.12f;
        if (k.up) py = max(0.0f, py - step);
        if (k.down) py = min(1.0f, py + step);
        if (k.left) px = max(0.0f, px - step);
        if (k.right) px = min(1.0f, px + step);
        viewChanged = true;
      } else if (k.ch == 'h') hud = !hud;
      else if (k.ch == 'q') {
        cfg.quality = (cfg.quality + 1) % 3; saveSettings(); viewChanged = true;
        static const char* qn[] = {"Smooth", "Balanced", "Sharp"};
        say(String("Quality: ") + qn[cfg.quality]);
      } else if (k.ch == 's') {
        if (!sdOk) say("No SD card");
        else if (!lastLen) say("No frame yet");
        else {
          ensureDcim();
          String fn = sdName("live_", ".jpg");
          File f = SD.open(fn, FILE_WRITE);
          if (f) { f.write(jpgBuf, lastLen); f.close(); say("SD " + fn.substring(12)); }
          else say("SD write failed");
        }
      }
      if (viewChanged) {
        c.stop();
        setView(zoom, px, py);
        streaming = openStream();
      }
    }

    if (millis() - lastStatus > 3000) { lastStatus = millis(); refreshStatus(); }

    if (!streaming) {
      cv.fillScreen(0);
      if (lastLen) cv.drawJpg(jpgBuf, lastLen, 0, 0);
      centerText(pi.mode == "off" ? "Camera is off" : "No video", 50, C_HAL);
      centerText(pi.mode == "off" ? "Esc > Camera mode to start it" : "retrying...", 72, C_DIM, &fonts::Font0);
      drawOverlay();
      push();
      delay(800);
      streaming = openStream();
      continue;
    }

    if (c.available()) {
      String line;
      size_t len = 0;
      bool ok = false;
      // part headers: --frame / Content-Type / Content-Length / X-Seq / blank
      while (readLine(c, line, 3000)) {
        if (line.startsWith("Content-Length:")) len = line.substring(15).toInt();
        if (line.length() == 0 && len) { ok = true; break; }
      }
      if (ok && len < JPG_MAX && readExact(c, jpgBuf, len, 4000)) {
        lastLen = len;
        lastFrame = millis();
        frames++;
        cv.drawJpg(jpgBuf, len, 0, 0);
        drawOverlay();
        push();
      } else if (!ok || len >= JPG_MAX) {
        streaming = false;
      }
    } else if (!c.connected() || millis() - lastFrame > 8000) {
      if (lastFrame && millis() - lastFrame > 8000) lastFrame = millis();
      streaming = openStream();
    } else {
      delay(2);
    }

    if (millis() - fpsT >= 2000) {
      fps = frames * 1000.0f / (millis() - fpsT);
      frames = 0;
      fpsT = millis();
      if (!hud) {} else if (lastLen) { cv.drawJpg(jpgBuf, lastLen, 0, 0); drawOverlay(); push(); }
    }
  }
  c.stop();
}

// ------------------------------------------------------------- timelapse
static void screenTimelapse() {
  if (!ensureOnline()) return;
  int sel = 0;
  std::vector<Item> items;
  JsonDocument tl;
  auto load = [&]() { apiGet("/api/timelapse", tl, 8000); };
  auto fill = [&]() {
    items.clear();
    bool active = tl["active"] | false;
    if (active) {
      int fr = tl["frames"] | 0, cov = tl["covering_s"] | 0, ofps = tl["out_fps"] | 24;
      items.push_back({"Stop timelapse", String(tl["session"] | ""), C_HAL});
      items.push_back({"Frames", String(fr)});
      items.push_back({"Real time covered", fmtDur(cov)});
      items.push_back({"Video length", fmtDur(fr / max(1, ofps))});
      items.push_back({"Watch latest frame", ""});
    } else {
      items.push_back({"Start timelapse", "every " + String(tl["interval"] | 3) + "s", C_OK});
      items.push_back({"Interval", String(tl["interval"] | 3) + "s"});
    }
    items.push_back({(tl["rendering"] | false) ? "Rendering..." : "Render latest to MP4", ""});
    for (JsonObject s : tl["sessions"].as<JsonArray>()) {
      String nm = s["name"] | "";
      items.push_back({"  " + nm, String(s["frames"] | 0) + (s["rendered"] | false ? "f  mp4" : "f"), C_DIM});
    }
  };
  load(); fill();
  while (true) {
    int r = listMenu("Timelapse", items, sel, "Enter select   Esc back", nullptr,
                     [&]() { static uint32_t t = 0; if (millis() - t < 4000) return false; t = millis(); load(); return true; },
                     fill);
    if (r < 0) return;
    String lbl = items[r].label;
    JsonDocument d;
    if (lbl == "Stop timelapse") {
      busy("Stopping (livestream restarts)...");
      apiPost("/api/timelapse/stop", "", d, 40000);
      toast("Stopped", C_OK);
    } else if (lbl == "Start timelapse") {
      String name;
      if (!textInput("Session name (blank = date)", name)) continue;
      busy("Starting...");
      String body = "{\"name\":\"" + name + "\"}";
      if (apiPost("/api/timelapse/start", name.length() ? body : String("{}"), d, 40000)) toast("Capturing", C_OK);
      else toast(errOf(d), C_HAL, 2000);
    } else if (lbl == "Interval") {
      std::vector<String> opts = {"2s  smooth motion", "3s", "5s", "10s", "30s  clouds", "60s  sunsets", "300s  plants"};
      int vals[] = {2, 3, 5, 10, 30, 60, 300};
      int c = choose("Seconds between frames", opts, 1);
      if (c >= 0) {
        // the interval is written on the Pi when this timelapse starts
        String name;
        if (confirm("Start now at " + String(vals[c]) + "s?")) {
          textInput("Session name (blank = date)", name);
          busy("Starting...");
          String b2 = "{\"interval\":" + String(vals[c]) + (name.length() ? ",\"name\":\"" + name + "\"" : String("")) + "}";
          if (apiPost("/api/timelapse/start", b2, d, 40000)) toast("Capturing", C_OK);
          else toast(errOf(d), C_HAL, 2000);
        }
      }
    } else if (lbl == "Render latest to MP4") {
      if (apiPost("/api/timelapse/render", "", d)) toast("Rendering on the Pi", C_OK, 1500);
      else toast(d["error"] | "failed", C_HAL);
    } else if (lbl == "Watch latest frame") {
      screenLive();
    } else if (lbl.startsWith("  ")) {
      String nm = lbl.substring(2);
      int c = choose(nm, {"Render this session", "Cancel"});
      if (c == 0) {
        String body = "{\"name\":\"" + nm + "\"}";
        if (apiPost("/api/timelapse/render", body, d)) toast("Rendering on the Pi", C_OK, 1500);
        else toast(d["error"] | "failed", C_HAL);
      }
    }
    load(); fill();
  }
}

// ------------------------------------------------------------- image tuning
struct Param { const char* key; const char* label; float lo, hi, step; std::vector<String> opts; };

static void screenImage() {
  if (!ensureOnline()) return;
  JsonDocument cur;
  if (!apiGet("/api/image", cur, 6000)) { toast("Pi error", C_HAL); return; }
  if (!(cur["exists"] | false)) { toast("Run setup-fisheye-fx.sh on the Pi", C_WARN, 2200); return; }

  std::vector<Param> ps = {
      {"ROTATE", "Rotation", 0, 0, 0, {"0", "90", "180", "270"}},
      {"AWB", "White balance", 0, 0, 0, {"auto", "daylight", "cloudy", "indoor", "fluorescent", "tungsten", "incandescent"}},
      {"EV", "Exposure (EV)", -10, 10, 0.5, {}},
      {"BRIGHTNESS", "Brightness", -1, 1, 0.05, {}},
      {"CONTRAST", "Contrast", 0, 2, 0.1, {}},
      {"SATURATION", "Saturation", 0, 2, 0.1, {}},
      {"SHARPNESS", "Sharpness", 0, 2, 0.1, {}},
      {"DENOISE", "Denoise", 0, 0, 0, {"auto", "off", "cdn_off", "cdn_fast", "cdn_hq"}},
      {"FX_K1", "Fisheye bulge K1", -1, 1, 0.05, {}},
      {"FX_K2", "Fisheye bulge K2", -1, 1, 0.05, {}},
      {"FPS", "Stream FPS", 1, 30, 1, {}},
      {"BITRATE", "Bitrate", 0, 0, 0, {"1M", "2M", "3M", "4M", "6M"}},
  };
  std::vector<String> vals;
  for (auto& p : ps) vals.push_back(String((const char*)(cur["values"][p.key] | "")));
  std::vector<String> orig = vals;

  struct Preset { const char* name; const char* json; };
  static const Preset presets[] = {
      {"Normal", "{\"AWB\":\"auto\",\"EV\":0,\"BRIGHTNESS\":0,\"CONTRAST\":1,\"SATURATION\":1,\"SHARPNESS\":1,\"FX_K1\":-0.2,\"FX_K2\":-0.05}"},
      {"Vivid", "{\"SATURATION\":1.6,\"CONTRAST\":1.3,\"SHARPNESS\":1.4}"},
      {"Night", "{\"EV\":2,\"BRIGHTNESS\":0.1,\"DENOISE\":\"cdn_hq\",\"FPS\":10}"},
      {"Mono", "{\"SATURATION\":0,\"CONTRAST\":1.4}"},
      {"Max fisheye", "{\"FX_K1\":-0.45,\"FX_K2\":-0.15}"},
      {"Flat (no bulge)", "{\"FX_K1\":0,\"FX_K2\":0}"},
  };

  std::vector<Item> items;
  auto fill = [&]() {
    items.clear();
    for (size_t i = 0; i < ps.size(); i++)
      items.push_back({ps[i].label, (vals[i] != orig[i] ? "*" : "") + vals[i], vals[i] != orig[i] ? C_WARN : C_TEXT});
    items.push_back({"Apply changes", "", C_OK});
    items.push_back({"Presets...", ""});
  };
  auto adjust = [&](int i, int dir) {
    if (i >= (int)ps.size()) return;
    Param& p = ps[i];
    if (!p.opts.empty()) {
      int idx = 0;
      for (size_t j = 0; j < p.opts.size(); j++) if (p.opts[j] == vals[i]) idx = j;
      idx = (idx + dir + p.opts.size()) % p.opts.size();
      vals[i] = p.opts[idx];
    } else {
      float v = constrain(vals[i].toFloat() + dir * p.step, p.lo, p.hi);
      vals[i] = p.step >= 1 ? String((int)v) : String(v, 2);
    }
  };
  auto apply = [&](const String& body) {
    busy("Applying (stream restarts)...");
    JsonDocument d;
    if (apiPost("/api/image", body, d, 30000)) {
      toast(d["restarted"] | false ? "Applied" : "Saved (stream not running)", C_OK);
      return true;
    }
    String err = "failed";
    if (d["errors"].is<JsonArray>() && d["errors"].size()) err = d["errors"][0].as<String>();
    toast(err.substring(0, 32), C_HAL, 2000);
    return false;
  };
  fill();
  int sel = 0;
  while (true) {
    int r = listMenu("Image  (</> adjust)", items, sel, "</> change  Enter apply  Esc back",
                     [&](const Keys& k, int i) { return false; }, nullptr, fill);
    // listMenu treats left/right as paging; image screen wants them as +/-,
    // so we handle them here by re-entering with a small custom loop instead.
    if (r < 0) {
      bool changed = false;
      for (size_t i = 0; i < vals.size(); i++) if (vals[i] != orig[i]) changed = true;
      if (changed && confirm("Apply your changes?")) {
        JsonDocument b;
        for (size_t i = 0; i < vals.size(); i++) if (vals[i] != orig[i]) b[ps[i].key] = vals[i];
        String body; serializeJson(b, body);
        apply(body);
      }
      return;
    }
    if (r < (int)ps.size()) {
      // edit one value with left/right until Enter/Esc
      int i = r;
      while (true) {
        cv.fillScreen(C_BG);
        drawHeader(ps[i].label);
        centerText(vals[i], 45, C_TEXT, &fonts::FreeSansBold12pt7b);
        if (ps[i].opts.empty()) {
          float t = (vals[i].toFloat() - ps[i].lo) / (ps[i].hi - ps[i].lo);
          cv.drawRoundRect(20, 82, W - 40, 10, 4, C_LINE);
          cv.fillRoundRect(20, 82, max(6, (int)((W - 40) * t)), 10, 4, C_HAL);
        }
        drawFooter(", / = less/more   Enter done   Esc undo");
        push();
        Keys k;
        do { k = readKeys(); delay(8); } while (!k.any);
        if (k.left || k.down) adjust(i, -1);
        else if (k.right || k.up) adjust(i, +1);
        else if (k.enter) break;
        else if (isBack(k)) { vals[i] = orig[i]; break; }
      }
      fill();
    } else if (r == (int)ps.size()) {
      JsonDocument b;
      for (size_t i = 0; i < vals.size(); i++) if (vals[i] != orig[i]) b[ps[i].key] = vals[i];
      if (b.size() == 0) { toast("Nothing changed", C_DIM); continue; }
      String body; serializeJson(b, body);
      if (apply(body)) orig = vals;
      fill();
    } else {
      std::vector<String> names;
      for (auto& p : presets) names.push_back(p.name);
      int c = choose("Presets", names);
      if (c >= 0 && apply(presets[c].json)) {
        if (apiGet("/api/image", cur, 6000))
          for (size_t i = 0; i < ps.size(); i++) vals[i] = orig[i] = String((const char*)(cur["values"][ps[i].key] | ""));
        fill();
      }
    }
  }
}

// ------------------------------------------------------------- mode
static void screenMode() {
  if (!ensureOnline()) return;
  std::vector<String> opts = {"Livestream (fisheye fx)", "PiCam Hub (:8080)", "Camera off"};
  const char* keys[] = {"fx", "hub", "off"};
  int cur = pi.mode == "hub" ? 1 : pi.mode == "off" ? 2 : 0;
  int c = choose("Camera mode: " + modeName(pi.mode), opts, cur);
  if (c < 0) return;
  busy("Switching camera...");
  JsonDocument d;
  String body = String("{\"mode\":\"") + keys[c] + "\"}";
  if (apiPost("/api/mode", body, d, 40000)) { refreshStatus(); toast(modeName(pi.mode), C_OK); }
  else toast(String((const char*)(d["error"] | "failed")).substring(0, 34), C_HAL, 2200);
}

// ------------------------------------------------------------- gallery
static bool downloadToSd(const String& remote, const String& name, uint32_t size) {
  if (!sdOk) { toast("No SD card", C_HAL); return false; }
  ensureDcim();
  String path = "/DCIM/PiCam/" + name;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, baseUrl() + "/api/file?f=" + urlEncode(remote));
  if (cfg.token.length()) http.addHeader("X-Deck-Token", cfg.token);
  int code = http.GET();
  if (code != 200) { http.end(); toast("HTTP " + String(code), C_HAL); return false; }
  int total = http.getSize();
  if (total <= 0) total = size;
  File f = SD.open(path, FILE_WRITE);
  if (!f) { http.end(); toast("SD write failed", C_HAL); return false; }
  WiFiClient* s = http.getStreamPtr();
  static uint8_t buf[4096];
  uint32_t got = 0, t0 = millis(), lastDraw = 0, lastData = millis();
  bool cancelled = false;
  while (http.connected() && (total <= 0 || got < (uint32_t)total)) {
    size_t a = s->available();
    if (a) {
      int r = s->readBytes(buf, min(a, sizeof buf));
      if (r > 0) { f.write(buf, r); got += r; lastData = millis(); }
    } else {
      if (millis() - lastData > 10000) break;
      delay(2);
    }
    if (millis() - lastDraw > 250) {
      lastDraw = millis();
      cv.fillScreen(C_BG);
      drawHeader("Downloading to SD");
      cv.setFont(&fonts::Font2);
      cv.setTextColor(C_TEXT);
      cv.setCursor(8, 28);
      cv.print(name);
      float pct = total > 0 ? (float)got / total : 0;
      cv.drawRoundRect(8, 56, W - 16, 14, 5, C_LINE);
      cv.fillRoundRect(8, 56, max(8, (int)((W - 16) * pct)), 14, 5, C_HAL);
      cv.setTextColor(C_DIM);
      cv.setCursor(8, 78);
      float secs = (millis() - t0) / 1000.0f;
      cv.printf("%s / %s   %.0f KB/s", fmtSize(got).c_str(), fmtSize(total).c_str(), secs > 0 ? got / 1024.0f / secs : 0);
      drawFooter("Esc cancel");
      push();
      Keys k = readKeys();
      if (isBack(k)) { cancelled = true; break; }
    }
  }
  f.close();
  http.end();
  if (cancelled || (total > 0 && got < (uint32_t)total)) {
    SD.remove(path);
    toast(cancelled ? "Cancelled" : "Download failed", C_HAL);
    return false;
  }
  toast("Saved to " + path.substring(6), C_OK, 1400);
  return true;
}

static bool showRemoteJpg(const String& url) {
  if (!jpgBuf) jpgBuf = (uint8_t*)malloc(JPG_MAX);
  if (!jpgBuf) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, url);
  if (cfg.token.length()) http.addHeader("X-Deck-Token", cfg.token);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    int len = http.getSize();
    WiFiClient* s = http.getStreamPtr();
    size_t got = 0;
    uint32_t t0 = millis();
    while ((len < 0 || (int)got < len) && got < JPG_MAX && millis() - t0 < 30000) {
      size_t a = s->available();
      if (a) got += s->readBytes(jpgBuf + got, min(a, JPG_MAX - got));
      else if (!http.connected()) break;
      else delay(2);
    }
    if (got > 0) { cv.fillScreen(0); ok = cv.drawJpg(jpgBuf, got, 0, 0); }
  }
  http.end();
  return ok;
}

static void screenGallery() {
  if (!ensureOnline()) return;
  std::vector<String> dirs = {"recordings", "captures", "apps"};
  int which = choose("Gallery", {"Recordings  (clips, photos, timelapses)", "PiCam Hub captures", "Cardputer apps (~/cardputer-apps)"});
  if (which < 0) return;
  String dir = dirs[which];
  struct F { String name, kind; uint32_t size; long mtime; };
  std::vector<F> files;
  int sel = 0;
  while (true) {
    busy("Loading...");
    JsonDocument d;
    files.clear();
    if (!apiGet("/api/files?dir=" + dir + "&limit=60", d, 10000)) { toast("Pi error", C_HAL); return; }
    for (JsonObject o : d["items"].as<JsonArray>())
      if (String((const char*)(o["kind"] | "")) != "dir")
        files.push_back({o["name"] | "", o["kind"] | "", o["size"] | 0u, o["mtime"] | 0L});
    std::vector<Item> items;
    for (auto& f : files) {
      uint16_t col = f.kind == "video" ? C_BLUE : f.kind == "photo" ? C_TEXT : C_WARN;
      String lbl = f.name;
      if (lbl.length() > 24) lbl = lbl.substring(0, 23) + "~";
      items.push_back({lbl, fmtSize(f.size), col});
    }
    int r = listMenu(dir + "  (" + String(files.size()) + ")", items, sel, "Enter view  d SD  x delete  Esc",
                     [&](const Keys& k, int i) {
                       if (i >= (int)files.size()) return false;
                       if (k.ch == 'd') downloadToSd(dir + "/" + files[i].name, files[i].name, files[i].size);
                       return true;
                     });
    if (r < 0) return;
    // viewer: thumbnail + actions, < > to flip
    while (true) {
      F& f = files[sel];
      cv.fillScreen(0);
      centerText("Loading preview...", 60, C_DIM);
      push();
      bool shown = f.kind != "app" &&
                   showRemoteJpg(baseUrl() + "/api/thumb?w=240&h=135&f=" + urlEncode(dir + "/" + f.name));
      if (!shown) {
        cv.fillScreen(C_BG);
        centerText(f.kind == "app" ? "Firmware file" : "No preview", 50, C_DIM);
      }
      cv.setFont(&fonts::Font0);
      cv.fillRect(0, 0, W, 11, 0);
      cv.setTextColor(C_TEXT);
      cv.setCursor(3, 2);
      cv.printf("%d/%d %s  %s", sel + 1, (int)files.size(), f.name.c_str(), fmtSize(f.size).c_str());
      cv.fillRect(0, H - 10, W, 10, 0);
      cv.setTextColor(C_DIM);
      cv.setCursor(3, H - 9);
      cv.print(", / prev next   d save to SD   x delete   Esc");
      push();
      Keys k;
      do { k = readKeys(); delay(8); } while (!k.any);
      if (isBack(k)) break;
      if (k.left || k.up) sel = (sel + files.size() - 1) % files.size();
      else if (k.right || k.down) sel = (sel + 1) % files.size();
      else if (k.ch == 'd' || k.enter) downloadToSd(dir + "/" + f.name, f.name, f.size);
      else if (k.ch == 'x') {
        if (confirm("Delete " + f.name.substring(0, 18) + "?")) {
          JsonDocument dd;
          httpJson("DELETE", "/api/file?f=" + urlEncode(dir + "/" + f.name), "", dd);
          toast("Deleted", C_OK);
          break;
        }
      }
    }
  }
}

// ------------------------------------------------------------- Pi system
static String throttleText(const String& hex) {
  long v = strtol(hex.c_str(), nullptr, 16);
  if (hex.isEmpty()) return "?";
  if (v == 0) return "OK";
  String s;
  if (v & 0x1) s += "UNDERVOLT ";
  if (v & 0x2) s += "FREQCAP ";
  if (v & 0x4) s += "THROTTLED ";
  if (v & 0x8) s += "HOT ";
  if (s.isEmpty()) s = "past issue";
  return s;
}

static void screenSystem() {
  if (!ensureOnline()) return;
  int sel = 0;
  std::vector<Item> items;
  JsonDocument st, net;
  auto load = [&]() { apiGet("/api/status", st, 6000); refreshStatus(); };
  auto fill = [&]() {
    items.clear();
    items.push_back({"Host", String((const char*)(st["host"] | "")) + " " + pi.host});
    items.push_back({"CPU temp", String(pi.temp, 1) + " C", pi.temp > 75 ? C_HAL : pi.temp > 65 ? C_WARN : C_TEXT});
    items.push_back({"Power / throttle", throttleText(pi.throttled), pi.throttled == "0x0" ? C_TEXT : C_WARN});
    items.push_back({"Load / memory", String(pi.load, 2) + " / " + String(pi.mem) + "%"});
    items.push_back({"Disk", String(pi.diskFree, 1) + "GB free (" + String(pi.diskPct) + "%)", pi.diskPct > 90 ? C_HAL : C_TEXT});
    int up = st["sys"]["uptime_s"] | 0;
    items.push_back({"Uptime", String(up / 86400) + "d " + String((up / 3600) % 24) + "h " + String((up / 60) % 60) + "m"});
    for (JsonPair kv : st["services"].as<JsonObject>()) {
      bool on = kv.value().as<bool>();
      items.push_back({String("svc ") + kv.key().c_str(), on ? "running" : "stopped", on ? C_OK : C_DIM});
    }
    items.push_back({"Pi WiFi...", ""});
    items.push_back({"Reboot Pi", "", C_WARN});
    items.push_back({"Shut down Pi", "", C_HAL});
  };
  load(); fill();
  while (true) {
    int r = listMenu("Pi system", items, sel, "Enter act   Esc back", nullptr,
                     [&]() { static uint32_t t = 0; if (millis() - t < 5000) return false; t = millis(); load(); return true; }, fill);
    if (r < 0) return;
    String lbl = items[r].label;
    JsonDocument d;
    if (lbl.startsWith("svc ")) {
      String name = lbl.substring(4);
      int c = choose(name, {"Restart", "Start", "Stop"});
      const char* acts[] = {"restart", "start", "stop"};
      if (c >= 0) {
        busy(String(acts[c]) + " " + name + "...");
        String body = String("{\"name\":\"") + name + "\",\"action\":\"" + acts[c] + "\"}";
        bool ok = apiPost("/api/service", body, d, 30000);
        toast(ok ? String("Done") : errOf(d), ok ? C_OK : C_HAL, 1400);
      }
    } else if (lbl == "Pi WiFi...") {
      apiGet("/api/net", net, 8000);
      if (!(net["installed"] | false)) { toast("Run setup-picam-ap.sh on the Pi", C_WARN, 2200); }
      else {
        String m = net["mode"] | "?";
        int c = choose("Pi WiFi: " + (m == "ap" ? String("own PiCam network") : "joined " + String((const char*)(net["ssid"] | ""))),
                       {"Use saved WiFi / hotspot", "Host own PiCam network"});
        if (c >= 0) {
          String body = c == 0 ? "{\"mode\":\"client\"}" : "{\"mode\":\"ap\"}";
          apiPost("/api/net", body, d, 8000);
          toast("Pi switching WiFi...", C_WARN, 1500);
          WiFi.disconnect();
          pi.host = "";
          delay(15000);
          if (ensureOnline()) toast("Reconnected", C_OK);
          return;
        }
      }
    } else if (lbl == "Reboot Pi" || lbl == "Shut down Pi") {
      bool reboot = lbl == "Reboot Pi";
      if (confirm(reboot ? "Reboot the Pi?" : "Shut the Pi down?")) {
        apiPost("/api/system", reboot ? "{\"action\":\"reboot\"}" : "{\"action\":\"poweroff\"}", d, 8000);
        pi.online = false;
        toast(reboot ? "Rebooting (~60s)" : "Shutting down", C_WARN, 1500);
        return;
      }
    }
  }
}

// ------------------------------------------------------------- links / QR
static void screenLinks() {
  if (!ensureOnline()) return;
  JsonDocument d;
  if (!apiGet("/api/links", d)) { toast("Pi error", C_HAL); return; }
  std::vector<Item> items;
  std::vector<String> urls;
  for (JsonObject o : d["links"].as<JsonArray>()) {
    items.push_back({o["name"] | "", ""});
    urls.push_back(o["url"] | "");
  }
  int sel = 0;
  while (true) {
    int r = listMenu("Watch on phone / PC", items, sel, "Enter = QR code   Esc back");
    if (r < 0) return;
    cv.fillScreen(TFT_WHITE);
    cv.qrcode(urls[r].c_str(), 4, 4, 127, 4);
    cv.setFont(&fonts::Font0);
    cv.setTextColor(TFT_BLACK);
    cv.setCursor(136, 10);
    cv.print(items[r].label);
    // wrap the url in the right column
    String u = urls[r];
    int y = 30;
    while (u.length() && y < 120) {
      String part = u.substring(0, 16);
      cv.setCursor(136, y);
      cv.print(part);
      u = u.substring(part.length());
      y += 11;
    }
    push();
    Keys k;
    do { k = readKeys(); delay(10); } while (!k.any);
  }
}

// ------------------------------------------------------------- settings
static void screenWifiAdd() {
  busy("Scanning...");
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  std::vector<String> ssids;
  std::vector<Item> items;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;
    for (auto& x : ssids) if (x == s) dup = true;
    if (dup) continue;
    ssids.push_back(s);
    items.push_back({s, String(WiFi.RSSI(i)) + "dBm"});
  }
  WiFi.scanDelete();
  items.push_back({"Type a name...", ""});
  int sel = 0;
  int r = listMenu("Add WiFi", items, sel, "Enter choose   Esc back");
  if (r < 0) return;
  String ssid = r < (int)ssids.size() ? ssids[r] : "";
  if (ssid.isEmpty() && !textInput("Network name", ssid)) return;
  String pass;
  if (!textInput("Password for " + ssid, pass, true)) return;
  addNet(ssid, pass);
  saveSettings();
  WiFi.disconnect();
  pi.host = "";
  toast("Saved " + ssid, C_OK);
}

static void screenSettings() {
  int sel = 0;
  while (true) {
    std::vector<Item> items;
    items.push_back({"WiFi", WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "not connected"});
    items.push_back({"Add WiFi network...", ""});
    items.push_back({"Forget a network...", String(cfg.nets.size()) + " saved"});
    items.push_back({"Pi address", cfg.piHost.length() ? cfg.piHost : "auto (" + (pi.host.length() ? pi.host : String("?")) + ")"});
    items.push_back({"Pi token", cfg.token.length() ? "set" : "none"});
    items.push_back({"Find the Pi again", ""});
    items.push_back({"Brightness", String(cfg.bright * 100 / 255) + "%"});
    items.push_back({"Rotate screen (o)", String(cfg.rot * 90) + " deg"});
    items.push_back({"Load /picam.cfg from SD", sdOk ? "" : "no SD"});
    items.push_back({"About", APP_VERSION});
    int r = listMenu("Settings", items, sel, "Enter select   Esc back");
    if (r < 0) return;
    switch (r) {
      case 0: WiFi.disconnect(); pi.host = ""; ensureOnline(); break;
      case 1: screenWifiAdd(); break;
      case 2: {
        std::vector<String> names;
        for (auto& n : cfg.nets) names.push_back(n.ssid);
        int c = choose("Forget which?", names);
        if (c >= 0 && confirm("Forget " + names[c] + "?")) { cfg.nets.erase(cfg.nets.begin() + c); saveSettings(); }
        break;
      }
      case 3: {
        String h = cfg.piHost;
        if (textInput("Pi IP (blank = auto)", h)) { h.trim(); cfg.piHost = h; pi.host = ""; saveSettings(); }
        break;
      }
      case 4: {
        String t = cfg.token;
        if (textInput("Token (setup script prints it)", t)) { t.trim(); cfg.token = t; saveSettings(); }
        break;
      }
      case 5: cfg.lastHost = ""; pi.host = ""; if (ensureOnline()) toast("Found " + pi.host, C_OK); break;
      case 6: {
        int c = choose("Brightness", {"25%", "50%", "75%", "100%"}, 2);
        if (c >= 0) { cfg.bright = (c + 1) * 63; M5Cardputer.Display.setBrightness(cfg.bright); saveSettings(); }
        break;
      }
      case 7: cycleRotation(); break;
      case 8: toast(importSdConfig() ? "Imported" : "No /picam.cfg on SD", C_WARN); pi.host = ""; break;
      case 9: toast("PiCam Remote " APP_VERSION, C_BLUE, 1500); break;
    }
  }
}

// ------------------------------------------------------------- home
static void quickAction(const char* path, const String& doing, const String& okMsg) {
  if (!ensureOnline()) return;
  busy(doing);
  JsonDocument d;
  if (apiPost(path, "", d, 30000)) { refreshStatus(); toast(okMsg, C_OK); }
  else toast(String((const char*)(d["error"] | "failed")).substring(0, 34), C_HAL, 2000);
}

static void splash() {
  cv.fillScreen(C_BG);
  for (int r = 34; r > 0; r -= 2) {
    uint8_t v = 255 - r * 6;
    cv.fillCircle(W / 2, 52, r, cv.color565(v, v / 8, v / 10));
  }
  cv.fillCircle(W / 2, 52, 6, C_WARN);
  centerText("PiCam Remote", 96, C_TEXT, &fonts::FreeSansBold9pt7b);
  centerText("for your Raspberry Pi camera", 118, C_DIM, &fonts::Font0);
  push();
}

void setup() {
  auto mcfg = M5.config();
  M5Cardputer.begin(mcfg, true);
  cv.setColorDepth(16);
  cv.createSprite(W, H);
  loadSettings();
  applyRotation();
  M5Cardputer.Display.setBrightness(cfg.bright);
  splash();

  SPI.begin(40, 39, 14, 12);
  sdOk = SD.begin(12, SPI, 25000000);
  if (importSdConfig()) toast("Loaded /picam.cfg", C_OK);
  jpgBuf = (uint8_t*)malloc(JPG_MAX);

  delay(600);
  ensureOnline(true);
}

void loop() {
  static int sel = 0;
  std::vector<Item> items;
  auto fill = [&]() {
    items.clear();
    items.push_back({"Live view", pi.online ? modeName(pi.mode) : "offline", pi.online ? C_TEXT : C_DIM});
    items.push_back({"Take photo", ""});
    items.push_back({pi.rec ? "Stop recording" : "Record clip", pi.rec ? fmtDur(pi.recElapsed) : "", pi.rec ? C_HAL : C_TEXT});
    items.push_back({"Timelapse", pi.tl ? String(pi.tlFrames) + " frames" : (pi.tlRendering ? "rendering" : ""), pi.tl ? C_WARN : C_TEXT});
    items.push_back({"Gallery", ""});
    items.push_back({"Image & fisheye", ""});
    items.push_back({"Camera mode", ""});
    items.push_back({"Watch on phone (QR)", ""});
    items.push_back({"Pi system", pi.online ? String(pi.temp, 0) + "C" : "", pi.temp > 75 ? C_HAL : C_TEXT});
    items.push_back({"Settings", WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "no WiFi"});
  };
  fill();
  String title = pi.online ? "PiCam  " + pi.host : "PiCam  (offline)";
  int r = listMenu(title, items, sel, "Enter  l live  p photo  r rec  t tl",
                   [&](const Keys& k, int) {
                     if (k.ch == 'l') { screenLive(); return true; }
                     if (k.ch == 'p') { quickAction("/api/photo", "Capturing...", "Photo saved on Pi"); return true; }
                     if (k.ch == 'r') { quickAction("/api/record/toggle", "Working...", pi.rec ? "Clip saved" : "Recording"); return true; }
                     if (k.ch == 't') { screenTimelapse(); return true; }
                     if (k.ch == 'g') { screenGallery(); return true; }
                     if (k.ch == 'o') { cycleRotation(); return true; }
                     return false;
                   },
                   []() {
                     static uint32_t t = 0;
                     if (millis() - t < 4000) return false;
                     t = millis();
                     if (WiFi.status() == WL_CONNECTED && pi.host.length()) refreshStatus();
                     return true;
                   },
                   fill);
  switch (r) {
    case 0: screenLive(); break;
    case 1: quickAction("/api/photo", "Capturing...", "Photo saved on Pi"); break;
    case 2: quickAction("/api/record/toggle", "Working...", pi.rec ? "Clip saved" : "Recording"); break;
    case 3: screenTimelapse(); break;
    case 4: screenGallery(); break;
    case 5: screenImage(); break;
    case 6: screenMode(); break;
    case 7: screenLinks(); break;
    case 8: screenSystem(); break;
    case 9: screenSettings(); break;
    default: if (!pi.online) ensureOnline(true); break;
  }
}
