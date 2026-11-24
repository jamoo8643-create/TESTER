// Pocket Grit - a pocket groovebox for the M5Stack Cardputer / Cardputer ADV
// 4 sample tracks + 1 synth track, 16 steps, mic sampling, SD loading, live scope.
#include <M5Cardputer.h>
#include <vector>
#include <Preferences.h>
#include "engine.h"
#include "storage.h"

using namespace engine;

// ------------------------------------------------------------------ look
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static constexpr uint16_t C_BG     = rgb(18, 16, 22);
static constexpr uint16_t C_PANEL  = rgb(34, 30, 42);
static constexpr uint16_t C_GRID   = rgb(52, 46, 62);
static constexpr uint16_t C_GRID4  = rgb(74, 66, 88);
static constexpr uint16_t C_TEXT   = rgb(225, 220, 205);
static constexpr uint16_t C_DIM    = rgb(130, 122, 140);
static constexpr uint16_t C_ACCENT = rgb(255, 150, 40);   // amber
static constexpr uint16_t C_SCOPE  = rgb(90, 255, 190);   // mint
static constexpr uint16_t C_REC    = rgb(255, 60, 70);
static constexpr uint16_t C_TRACK[NUM_TRACKS] = {
    rgb(255, 110, 60), rgb(255, 200, 70), rgb(120, 220, 255), rgb(200, 130, 255), rgb(90, 255, 190)};

static M5Canvas canvas(&M5Cardputer.Display);

// ------------------------------------------------------------------ state
enum Page { PG_SEQ, PG_SYNTH, PG_SAMPLE, PG_COUNT };
static Page  page        = PG_SEQ;
static int   track       = 0;      // selected track on SEQ page
static int   slotSel     = 0;      // selected slot on SAMPLE page
static int   paramSel    = P_CUTOFF;
static int   octave      = 3;
static int   lastNote    = 48;
static int   heldNote    = -1;
static char  heldKey     = 0;
static int   editCursor  = 0;      // synth step-entry cursor
static bool  recArm      = false;
static String toast;
static uint32_t toastUntil = 0;

static bool  browsing    = false;
static std::vector<String> files;
static int   fileSel     = 0;

static const char* PARAM_NAMES[P_COUNT] = {"WAVE", "DETUNE", "CUTOFF", "RESO", "ENVMOD",
                                           "ATTACK", "DECAY", "SUSTAIN", "RELEASE", "VOLUME"};
static const char* WAVE_NAMES[4] = {"SAW", "SQUARE", "TRI", "SINE"};
static const char* NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static Preferences prefs;
static int rotation = 0;

static void applyRotation(int r)
{
    rotation = r & 3;
    M5Cardputer.Display.setRotation(rotation);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
}

static void say(const String& s, uint32_t ms = 1500)
{
    toast = s;
    toastUntil = millis() + ms;
}

static String noteName(int n)
{
    if (n < 0) return "---";
    return String(NOTE_NAMES[n % 12]) + String(n / 12 - 1);
}

// ------------------------------------------------------------------ keyboard
struct KeyEvents {
    std::vector<uint8_t> down, up;
    bool shift = false, fn = false;
};
static bool prevKeys[256];

static KeyEvents pollKeys()
{
    KeyEvents ev;
    bool now[256] = {false};
    for (auto& p : M5Cardputer.Keyboard.keyList()) {
        uint8_t c = (uint8_t)M5Cardputer.Keyboard.getKeyValue(p).value_first;
        now[c] = true;
    }
    ev.shift = now[KEY_LEFT_SHIFT];
    ev.fn    = now[KEY_FN];
    for (int c = 1; c < 256; c++) {
        if (now[c] && !prevKeys[c]) ev.down.push_back(c);
        if (!now[c] && prevKeys[c]) ev.up.push_back(c);
        prevKeys[c] = now[c];
    }
    return ev;
}

// ------------------------------------------------------------------ helpers
static int quantizedStep()
{
    int s = curStep < 0 ? 0 : curStep;
    if (stepPhase() > 0.5f) s = (s + 1) % STEPS;
    return s;
}

static int stepForKey(uint8_t c)
{
    static const char* row = "12345678qwertyui";
    const char* p = strchr(row, c);
    return (p && c) ? (int)(p - row) : -1;
}

// piano: white keys on the a-row, black keys on the q-row
static int noteForKey(uint8_t c)
{
    switch (c) {
    case 'a': return 0;  case 'w': return 1;  case 's': return 2;  case 'e': return 3;
    case 'd': return 4;  case 'f': return 5;  case 't': return 6;  case 'g': return 7;
    case 'y': return 8;  case 'h': return 9;  case 'u': return 10; case 'j': return 11;
    case 'k': return 12;
    }
    return -1;
}

static void writeSynthStep(int step, int note)
{
    song.notes[curPattern][step] = note;
}

// ------------------------------------------------------------------ sampling
static void drawRecScreen(const int16_t* data, uint32_t pos, uint32_t max)
{
    canvas.fillSprite(C_BG);
    canvas.setTextColor(C_REC);
    canvas.setTextDatum(top_left);
    canvas.drawString("REC", 6, 4);
    canvas.fillCircle(34, 7, 3, (millis() / 250) & 1 ? C_REC : C_BG);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(String("slot ") + (slotSel + 1) + "  any key = stop", 46, 4);
    // level meter
    canvas.drawRect(6, 120, 228, 8, C_GRID4);
    canvas.fillRect(7, 121, (int)(226.0f * pos / max), 6, C_REC);
    // waveform of the last chunk
    int mid = 66;
    if (pos > 512) {
        const int16_t* d = data + pos - 512;
        int py = mid;
        for (int x = 0; x < 240; x++) {
            int y = mid + d[x * 2] / 600;
            if (y < 16) y = 16; if (y > 116) y = 116;
            if (x) canvas.drawLine(x - 1, py, x, y, C_SCOPE);
            py = y;
        }
    }
    canvas.pushSprite(0, 0);
}

static void recordSlot()
{
    Slot& s = slots[slotSel];
    engine::pause();
    M5Cardputer.Mic.begin();
    s.len = 0;
    const uint32_t CH = 256;
    uint32_t pos = 0;
    // let go of the key that started recording
    do { M5Cardputer.update(); pollKeys(); delay(5); } while (M5Cardputer.Keyboard.isPressed());
    while (pos + CH <= SLOT_MAX) {
        if (M5Cardputer.Mic.record(s.data + pos, CH, 16000)) pos += CH;
        M5Cardputer.update();
        auto k = pollKeys();
        if (!k.down.empty()) break;
        drawRecScreen(s.data, pos > CH * 2 ? pos - CH * 2 : 0, SLOT_MAX);
    }
    while (M5Cardputer.Mic.isRecording()) delay(1);
    M5Cardputer.Mic.end();

    // tidy: remove DC, trim leading silence, normalize, fade out
    uint32_t n = pos;
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += s.data[i];
    int32_t dc = n ? (int32_t)(sum / n) : 0;
    int32_t peak = 1;
    uint32_t first = n;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v = s.data[i] - dc;
        s.data[i] = (int16_t)constrain(v, -32768, 32767);
        int32_t a = abs(v);
        if (a > peak) peak = a;
    }
    int32_t thr = peak / 8;
    for (uint32_t i = 0; i < n; i++) if (abs(s.data[i]) > thr) { first = i; break; }
    if (first < n) {
        uint32_t start = first > 80 ? first - 80 : 0;
        memmove(s.data, s.data + start, (n - start) * sizeof(int16_t));
        n -= start;
    }
    float g = 30000.0f / peak;
    if (g > 12) g = 12;
    for (uint32_t i = 0; i < n; i++) s.data[i] = (int16_t)constrain((int32_t)(s.data[i] * g), -32768, 32767);
    for (uint32_t i = 0; i < 128 && i < n; i++) {
        uint32_t k = n - 1 - i;
        s.data[k] = (int16_t)((int32_t)s.data[k] * (int32_t)i / 128);
    }
    s.rate = 16000;
    s.pitch = 0;
    s.reverse = false;
    snprintf(s.name, sizeof(s.name), "MIC%d", slotSel + 1);
    s.len = n;
    engine::resume();
    say(String("recorded ") + String(n / 16000.0f, 2) + "s");
}

// ------------------------------------------------------------------ input
static void globalKey(uint8_t c, const KeyEvents& k)
{
    switch (c) {
    case ' ':
        setPlaying(!playing);
        break;
    case '\\':  // flip/rotate the screen if it comes up sideways; remembered
        applyRotation(rotation + 1);
        prefs.putInt("rot", rotation);
        say(String("screen rotation ") + rotation);
        break;
    case KEY_TAB:
        page = (Page)((page + 1) % PG_COUNT);
        browsing = false;
        break;
    case '`':
        page = (Page)((page + PG_COUNT - 1) % PG_COUNT);
        browsing = false;
        break;
    }
}

static void seqKey(uint8_t c, const KeyEvents& k)
{
    int st = stepForKey(c);
    if (st >= 0) {
        if (track == SYNTH_TRACK) {
            int8_t& n = song.notes[curPattern][st];
            n = n >= 0 ? -1 : lastNote;
        } else {
            uint8_t& v = song.steps[curPattern][track][st];
            if (k.shift) v = v == 2 ? 0 : 2;
            else v = v ? 0 : 1;
        }
        return;
    }
    static const char* trackKeys = "asdfg";
    const char* tp = strchr(trackKeys, c);
    if (tp && c) { track = tp - trackKeys; return; }
    static const char* padKeys = "zxcvb";
    const char* pp = strchr(padKeys, c);
    if (pp && c) {
        int t = pp - padKeys;
        post(EV_TRIG, t, (uint8_t)lastNote);
        track = t;
        if (recArm && playing) {
            int q = quantizedStep();
            if (t == SYNTH_TRACK) writeSynthStep(q, lastNote);
            else song.steps[curPattern][t][q] = 1;
        }
        return;
    }
    switch (c) {
    case '-': song.bpm = max(40, song.bpm - (k.shift ? 1 : 2)); break;
    case '=': song.bpm = min(240, song.bpm + (k.shift ? 1 : 2)); break;
    case '[': master = max(0, master - 15); say(String("volume ") + master * 100 / 255); break;
    case ']': master = min(255, master + 15); say(String("volume ") + master * 100 / 255); break;
    case 'p': {
        int next = ((queuedPattern >= 0 ? queuedPattern : curPattern) + 1) % NUM_PATTERNS;
        if (playing) queuedPattern = next; else curPattern = next;
        break;
    }
    case 'n': {  // copy pattern into the next one
        int dst = (curPattern + 1) % NUM_PATTERNS;
        memcpy(song.steps[dst], song.steps[curPattern], sizeof(song.steps[0]));
        memcpy(song.notes[dst], song.notes[curPattern], sizeof(song.notes[0]));
        say(String("copied to P") + (dst + 1));
        break;
    }
    case 'm': song.mute[track] ^= 1; break;
    case 'o': recArm = !recArm; break;
    case KEY_BACKSPACE:
        if (track == SYNTH_TRACK) memset(song.notes[curPattern], -1, STEPS);
        else memset(song.steps[curPattern][track], 0, STEPS);
        say("track cleared");
        break;
    case 'k': { String m; storage::saveProject(m); say(m); break; }
    case 'l': { String m; storage::loadProject(m); say(m); break; }
    }
}

static void synthKey(uint8_t c, const KeyEvents& k)
{
    int nk = noteForKey(c);
    if (nk >= 0) {
        int note = (octave + 1) * 12 + nk;
        lastNote = note;
        heldNote = note;
        heldKey = c;
        post(EV_NOTE_ON, note);
        if (recArm) {
            if (playing) writeSynthStep(quantizedStep(), note);
            else { writeSynthStep(editCursor, note); editCursor = (editCursor + 1) % STEPS; }
        }
        return;
    }
    int step = k.shift ? 1 : 5;
    switch (c) {
    case 'z': octave = max(0, octave - 1); break;
    case 'x': octave = min(7, octave + 1); break;
    case 'c': song.synth[P_WAVE] = (song.synth[P_WAVE] + 1) & 3; break;
    case '[': paramSel = (paramSel + P_COUNT - 1) % P_COUNT; break;
    case ']': paramSel = (paramSel + 1) % P_COUNT; break;
    case '-':
        if (paramSel == P_WAVE) song.synth[P_WAVE] = (song.synth[P_WAVE] + 3) & 3;
        else song.synth[paramSel] = max(0, song.synth[paramSel] - step);
        break;
    case '=':
        if (paramSel == P_WAVE) song.synth[P_WAVE] = (song.synth[P_WAVE] + 1) & 3;
        else song.synth[paramSel] = min(100, song.synth[paramSel] + step);
        break;
    case 'o': recArm = !recArm; editCursor = 0; break;
    case KEY_BACKSPACE:  // rest in step entry
        if (recArm && !playing) { writeSynthStep(editCursor, -1); editCursor = (editCursor + 1) % STEPS; }
        break;
    }
}

static void sampleKey(uint8_t c, const KeyEvents& k)
{
    if (browsing) {
        switch (c) {
        case ';': if (fileSel > 0) fileSel--; break;
        case '.': if (fileSel + 1 < (int)files.size()) fileSel++; break;
        case KEY_ENTER:
            if (!files.empty()) {
                String err;
                if (storage::loadWav(files[fileSel].c_str(), slotSel, err)) {
                    say(String("loaded ") + slots[slotSel].name);
                    post(EV_TRIG, slotSel);
                } else say(err);
            }
            browsing = false;
            break;
        case KEY_BACKSPACE: browsing = false; break;
        }
        return;
    }
    if (c >= '1' && c <= '4') { slotSel = c - '1'; post(EV_TRIG, slotSel); return; }
    Slot& s = slots[slotSel];
    switch (c) {
    case ';': slotSel = (slotSel + NUM_SLOTS - 1) % NUM_SLOTS; break;
    case '.': slotSel = (slotSel + 1) % NUM_SLOTS; break;
    case 'r':
    case KEY_ENTER: recordSlot(); break;
    case 'l':
        if (!storage::ok()) { say("no SD card"); break; }
        storage::listWavs(files);
        fileSel = 0;
        if (files.empty()) say("no .wav in / /samples /groovebox", 2500);
        else browsing = true;
        break;
    case 's': {
        char p[40];
        snprintf(p, sizeof(p), "/groovebox/%s_%lu.wav", s.name, (unsigned long)(millis() % 100000));
        say(storage::saveWav(p, slotSel) ? String("saved ") + p : String("save failed"), 2500);
        break;
    }
    case '-': s.pitch = max(-24, s.pitch - 1); post(EV_TRIG, slotSel); break;
    case '=': s.pitch = min(24, s.pitch + 1); post(EV_TRIG, slotSel); break;
    case '[': s.vol = max(0, s.vol - 5); break;
    case ']': s.vol = min(100, s.vol + 5); break;
    case 'v': s.reverse = !s.reverse; post(EV_TRIG, slotSel); break;
    case 'p':
    case 'z': post(EV_TRIG, slotSel); break;
    case 'd': makeDefaultSound(slotSel); say("default sound"); break;
    }
}

static void handleInput()
{
    KeyEvents k = pollKeys();
    for (uint8_t c : k.down) {
        globalKey(c, k);
        if (c == ' ' || c == KEY_TAB || c == '`' || c == '\\') continue;
        switch (page) {
        case PG_SEQ:    seqKey(c, k); break;
        case PG_SYNTH:  synthKey(c, k); break;
        case PG_SAMPLE: sampleKey(c, k); break;
        default: break;
        }
    }
    for (uint8_t c : k.up) {
        if (c == heldKey && heldNote >= 0) {
            post(EV_NOTE_OFF, heldNote);
            heldNote = -1;
            heldKey = 0;
        }
    }
}

// ------------------------------------------------------------------ drawing
static void drawHeader()
{
    canvas.fillRect(0, 0, 240, 12, C_PANEL);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(C_ACCENT);
    canvas.drawString("POCKET GRIT", 3, 2);
    static const char* tabs[PG_COUNT] = {"SEQ", "SYN", "SMP"};
    int x = 74;
    for (int i = 0; i < PG_COUNT; i++) {
        bool on = page == i;
        if (on) canvas.fillRoundRect(x - 2, 1, 22, 10, 2, C_ACCENT);
        canvas.setTextColor(on ? C_BG : C_DIM);
        canvas.drawString(tabs[i], x, 2);
        x += 24;
    }
    canvas.setTextDatum(top_right);
    canvas.setTextColor(C_TEXT);
    String r = String(song.bpm) + " P" + (curPattern + 1);
    if (queuedPattern >= 0) r += ">" + String(queuedPattern + 1);
    canvas.drawString(r, 222, 2);
    // transport icon
    if (playing) canvas.fillTriangle(227, 2, 227, 10, 234, 6, C_SCOPE);
    else canvas.fillRect(227, 3, 7, 7, C_DIM);
    if (recArm) canvas.fillCircle(237, 6, 2, C_REC);
}

static void drawScope(int y0, int h)
{
    canvas.fillRect(0, y0, 240, h, C_BG);
    int mid = y0 + h / 2;
    for (int x = 0; x < 240; x += 8) canvas.drawPixel(x, mid, C_GRID4);
    for (int x = 0; x < 240; x += 60) canvas.drawFastVLine(x, y0, h, C_GRID);
    // copy & trigger on a rising zero crossing so the wave stands still
    static int16_t snap[SCOPE_LEN];
    uint32_t end = scopePos;
    for (int i = 0; i < SCOPE_LEN; i++) snap[i] = scope[(end + i) & (SCOPE_LEN - 1)];
    int start = SCOPE_LEN - 240 - 1;
    for (int i = SCOPE_LEN - 240 - 1; i > SCOPE_LEN - 240 - 400 && i > 1; i--) {
        if (snap[i - 1] < 0 && snap[i] >= 0) { start = i; break; }
    }
    int py = mid;
    float scale = (h / 2 - 2) / 32768.0f * 1.6f;
    for (int x = 0; x < 240; x++) {
        int y = mid - (int)(snap[start + x] * scale);
        if (y < y0 + 1) y = y0 + 1;
        if (y > y0 + h - 2) y = y0 + h - 2;
        if (x) {
            canvas.drawLine(x - 1, py, x, y, C_SCOPE);
            canvas.drawLine(x - 1, py + 1, x, y + 1, rgb(30, 110, 85));  // glow
        }
        py = y;
    }
}

static void drawSeq(int y0)
{
    static const char* lbl[NUM_TRACKS] = {"1", "2", "3", "4", "S"};
    const int rowH = 12, cellW = 14, x0 = 16;
    for (int t = 0; t < NUM_TRACKS; t++) {
        int y = y0 + t * rowH;
        bool sel = t == track;
        uint16_t tc = C_TRACK[t];
        if (sel) canvas.fillRect(0, y, 14, rowH - 1, tc);
        else if (trackFlash[t]) canvas.fillRect(0, y, 14, rowH - 1, C_GRID4);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(sel ? C_BG : (song.mute[t] ? C_DIM : tc));
        canvas.drawString(song.mute[t] ? "m" : lbl[t], 4, y + 2);
        for (int s = 0; s < STEPS; s++) {
            int x = x0 + s * cellW;
            bool on;
            bool acc = false;
            if (t == SYNTH_TRACK) on = song.notes[curPattern][s] >= 0;
            else { uint8_t v = song.steps[curPattern][t][s]; on = v; acc = v == 2; }
            uint16_t off = (s % 4 == 0) ? C_GRID4 : C_GRID;
            uint16_t col = on ? (song.mute[t] ? C_DIM : tc) : off;
            canvas.fillRoundRect(x, y, cellW - 2, rowH - 2, 2, col);
            if (acc) canvas.fillRect(x + 4, y + 3, cellW - 10, rowH - 8, C_BG);
            if (playing && s == curStep) canvas.drawRoundRect(x - 1, y - 1, cellW, rowH, 2, C_TEXT);
        }
    }
}

static void drawBar(int x, int y, int w, int v, uint16_t c)
{
    canvas.fillRect(x, y, w, 4, C_GRID);
    canvas.fillRect(x, y, w * v / 100, 4, c);
}

static void drawSynth(int y0)
{
    canvas.setTextDatum(top_left);
    for (int i = 0; i < P_COUNT; i++) {
        int col = i / 5, row = i % 5;
        int x = col * 120 + 2, y = y0 + row * 12;
        bool sel = i == paramSel;
        if (sel) canvas.fillRoundRect(x - 1, y - 1, 116, 11, 2, C_PANEL);
        canvas.setTextColor(sel ? C_ACCENT : C_DIM);
        canvas.drawString(PARAM_NAMES[i], x + 1, y + 1);
        if (i == P_WAVE) {
            canvas.setTextColor(C_TEXT);
            canvas.drawString(WAVE_NAMES[song.synth[P_WAVE] & 3], x + 52, y + 1);
        } else {
            drawBar(x + 52, y + 3, 44, song.synth[i], sel ? C_ACCENT : C_TRACK[SYNTH_TRACK]);
            canvas.setTextColor(C_TEXT);
            canvas.setTextDatum(top_right);
            canvas.drawString(String(song.synth[i]), x + 114, y + 1);
            canvas.setTextDatum(top_left);
        }
    }
    // the synth track, as a strip of notes
    int y = y0 + 61;
    for (int s = 0; s < STEPS; s++) {
        int x = s * 15;
        bool on = song.notes[curPattern][s] >= 0;
        uint16_t c = on ? C_TRACK[SYNTH_TRACK] : (s % 4 == 0 ? C_GRID4 : C_GRID);
        canvas.fillRect(x + 1, y, 13, 3, c);
        if (playing && s == curStep) canvas.fillRect(x + 1, y, 13, 3, C_TEXT);
        if (recArm && !playing && s == editCursor) canvas.fillRect(x + 1, y, 13, 3, C_REC);
    }
}

static void drawThumb(int x, int y, int w, int h, const Slot& s, uint16_t c)
{
    canvas.fillRect(x, y, w, h, C_BG);
    uint32_t n = s.len;
    if (n < 2) return;
    int mid = y + h / 2;
    for (int i = 0; i < w; i++) {
        uint32_t a = (uint64_t)i * n / w, b = (uint64_t)(i + 1) * n / w;
        int32_t pk = 0;
        for (uint32_t k = a; k < b; k += 4) { int32_t v = abs(s.data[k]); if (v > pk) pk = v; }
        int hh = pk * (h / 2) / 32768;
        int xx = s.reverse ? x + w - 1 - i : x + i;
        canvas.drawFastVLine(xx, mid - hh, hh * 2 + 1, c);
    }
}

static void drawSample(int y0)
{
    canvas.setTextDatum(top_left);
    if (browsing) {
        canvas.setTextColor(C_ACCENT);
        canvas.drawString(String("load into slot ") + (slotSel + 1), 2, y0);
        int first = max(0, fileSel - 2);
        for (int i = 0; i < 4 && first + i < (int)files.size(); i++) {
            int idx = first + i;
            int y = y0 + 12 + i * 12;
            if (idx == fileSel) canvas.fillRect(0, y - 1, 240, 11, C_PANEL);
            canvas.setTextColor(idx == fileSel ? C_TEXT : C_DIM);
            String n = files[idx];
            if (n.length() > 38) n = "..." + n.substring(n.length() - 35);
            canvas.drawString(n, 4, y);
        }
        return;
    }
    for (int i = 0; i < NUM_SLOTS; i++) {
        const Slot& s = slots[i];
        int y = y0 + i * 15;
        bool sel = i == slotSel;
        if (sel) canvas.fillRoundRect(0, y - 1, 240, 14, 2, C_PANEL);
        canvas.setTextColor(C_TRACK[i]);
        canvas.drawString(String(i + 1), 4, y + 2);
        canvas.setTextColor(sel ? C_TEXT : C_DIM);
        canvas.drawString(s.name, 14, y + 2);
        drawThumb(74, y, 70, 12, s, trackFlash[i] ? C_TEXT : C_TRACK[i]);
        char info[32];
        snprintf(info, sizeof(info), "%+d v%d%s", s.pitch, s.vol, s.reverse ? " R" : "");
        canvas.drawString(info, 150, y + 2);
        canvas.setTextDatum(top_right);
        canvas.drawString(String((float)s.len / s.rate, 2) + "s", 238, y + 2);
        canvas.setTextDatum(top_left);
    }
}

static void drawFooter()
{
    canvas.fillRect(0, 126, 240, 9, C_PANEL);
    canvas.setTextDatum(top_left);
    String hint;
    if (millis() < toastUntil) {
        canvas.setTextColor(C_ACCENT);
        hint = toast;
    } else {
        canvas.setTextColor(C_DIM);
        switch (page) {
        case PG_SEQ:    hint = "1-8 q-i step  asdfg trk  zxcvb pad  p pat  o rec"; break;
        case PG_SYNTH:  hint = String("play a-k  oct z/x ") + octave + "  [] param  -= val  " + noteName(lastNote); break;
        case PG_SAMPLE: hint = browsing ? "; . move  enter load  del back"
                                        : "1-4 slot  r rec  l load  s save  -= pitch  v rev"; break;
        default: break;
        }
    }
    canvas.drawString(hint, 2, 127);
}

static void draw()
{
    canvas.fillSprite(C_BG);
    drawHeader();
    drawScope(13, 50);
    canvas.drawFastHLine(0, 63, 240, C_GRID);
    switch (page) {
    case PG_SEQ:    drawSeq(65); break;
    case PG_SYNTH:  drawSynth(65); break;
    case PG_SAMPLE: drawSample(66); break;
    default: break;
    }
    drawFooter();
    canvas.pushSprite(0, 0);
    for (int t = 0; t < NUM_TRACKS; t++) if (trackFlash[t]) trackFlash[t]--;
}

static void splash()
{
    canvas.fillSprite(C_BG);
    for (int x = 0; x < 240; x++) {
        float t = x / 240.0f;
        int y = 60 + (int)(sinf(t * 18.0f) * 20.0f * sinf(t * 3.14159f));
        canvas.drawPixel(x, y, C_SCOPE);
        canvas.drawPixel(x, y + 1, rgb(30, 110, 85));
    }
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_ACCENT);
    canvas.setTextSize(2);
    canvas.drawString("POCKET GRIT", 120, 26);
    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.drawString("space play  tab page  \\ rotate", 120, 100);
    canvas.drawString(storage::ok() ? "SD ok" : "no SD card", 120, 114);
    canvas.pushSprite(0, 0);
}

// ------------------------------------------------------------------ main
void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    // M5GFX 0.3+ gives the Cardputer panels a built-in rotation offset, so the
    // landscape rotation number differs between library versions. Pick
    // whichever one is actually landscape.
    // Press \\ to step through rotations; the choice is saved in flash.
    prefs.begin("pgrit", false);
    int saved = prefs.getInt("rot", -1);
    if (saved >= 0) {
        applyRotation(saved);
    } else {
        applyRotation(0);
        if (M5Cardputer.Display.width() < M5Cardputer.Display.height()) applyRotation(1);
    }
    M5Cardputer.Display.setBrightness(110);

    canvas.setColorDepth(8);
    canvas.createSprite(240, 135);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);

    M5Cardputer.Speaker.end();
    auto spk = M5Cardputer.Speaker.config();
    spk.sample_rate = 44100;
    spk.task_priority = 4;
    M5Cardputer.Speaker.config(spk);
    M5Cardputer.Speaker.begin();

    auto mic = M5Cardputer.Mic.config();
    mic.noise_filter_level = 64;
    mic.magnification = 24;
    M5Cardputer.Mic.config(mic);

    storage::begin();
    engine::begin();
    if (storage::ok()) {
        String m;
        if (storage::loadProject(m)) say(m);
    }
    splash();
    delay(900);
}

void loop()
{
    M5Cardputer.update();
    handleInput();
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw >= 33) {
        lastDraw = millis();
        draw();
    }
    delay(2);
}
