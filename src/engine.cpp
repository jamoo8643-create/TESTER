// Pocket Grit - audio engine
#include "engine.h"
#include <M5Cardputer.h>
#include <math.h>

namespace engine {

Slot  slots[NUM_SLOTS];
Song  song;
volatile bool    playing       = false;
volatile int     curStep       = 0;
volatile int     curPattern    = 0;
volatile int     queuedPattern = -1;
volatile uint8_t master        = 170;
volatile uint8_t trackFlash[NUM_TRACKS];
volatile int     synthNote     = -1;
int16_t          scope[SCOPE_LEN];
volatile uint32_t scopePos     = 0;

enum { EV_START = 100, EV_STOP = 101 };

static QueueHandle_t   evq;
static volatile bool   reqPause = false;
static volatile bool   idle     = false;
static int16_t         bufs[3][BLOCK];
static volatile float  stepPos  = 0;  // samples elapsed in current step
static volatile float  stepLen  = 1;

// ---------------------------------------------------------------- drum voices
struct DrumVoice {
    bool     active = false;
    uint32_t pos    = 0;   // 16.16 fixed
    uint32_t inc    = 0;
    float    gain   = 0;
};
static DrumVoice dv[NUM_SLOTS];

static void trigDrum(int t, bool accent)
{
    Slot& s = slots[t];
    if (s.len < 2) return;
    float ratio = (float)s.rate / SR * powf(2.0f, s.pitch / 12.0f);
    dv[t].inc    = (uint32_t)(ratio * 65536.0f);
    dv[t].pos    = 0;
    dv[t].gain   = (s.vol / 100.0f) * (accent ? 1.35f : 0.9f);
    dv[t].active = true;
    trackFlash[t] = 6;
}

static inline float renderDrum(int t)
{
    DrumVoice& v = dv[t];
    if (!v.active) return 0;
    Slot& s = slots[t];
    uint32_t len = s.len;
    uint32_t i   = v.pos >> 16;
    if (i + 1 >= len) { v.active = false; return 0; }
    float frac = (v.pos & 0xFFFF) * (1.0f / 65536.0f);
    float a, b;
    if (s.reverse) {
        a = s.data[len - 1 - i];
        b = s.data[len - 2 - i];
    } else {
        a = s.data[i];
        b = s.data[i + 1];
    }
    v.pos += v.inc;
    return (a + (b - a) * frac) * (1.0f / 32768.0f) * v.gain;
}

// ---------------------------------------------------------------- synth voice
struct SynthVoice {
    int   note   = -1;
    float ph1 = 0, ph2 = 0, inc1 = 0, inc2 = 0;
    int   stage  = 0;      // 0 idle 1 attack 2 decay 3 sustain 4 release
    float env    = 0;
    float low = 0, band = 0;
    float accent = 1;
};
static SynthVoice sv;
static int   seqGate = 0;      // samples until sequencer note-off
static bool  liveHeld = false;

static void synthOn(int note, bool fromSeq)
{
    float f = 440.0f * powf(2.0f, (note - 69) / 12.0f);
    float det = song.synth[P_DETUNE] * 0.3f;            // up to 30 cents
    sv.inc1 = f / SR;
    sv.inc2 = f * powf(2.0f, det / 1200.0f) / SR;
    sv.note = note;
    sv.stage = 1;
    synthNote = note;
    trackFlash[SYNTH_TRACK] = 6;
    if (!fromSeq) liveHeld = true;
}

static void synthOff(int note)
{
    if (note >= 0 && note != sv.note) return;
    if (sv.stage && sv.stage != 4) sv.stage = 4;
}

static inline float polyblep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline float osc(int wave, float ph, float dt)
{
    switch (wave) {
    case 0: return (2.0f * ph - 1.0f) - polyblep(ph, dt);                         // saw
    case 1: {                                                                      // square
        float v = ph < 0.5f ? 1.0f : -1.0f;
        float p2 = ph + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
        return v + polyblep(ph, dt) - polyblep(p2, dt);
    }
    case 2: return ph < 0.5f ? (4.0f * ph - 1.0f) : (3.0f - 4.0f * ph);          // triangle
    default: return sinf(ph * 6.2831853f);                                         // sine
    }
}

static inline float timeCoef(uint8_t p)
{
    float t = 0.005f + (p / 100.0f) * (p / 100.0f) * 2.5f;   // seconds
    return expf(-1.0f / (t * SR));
}

static inline float renderSynth()
{
    if (!sv.stage) return 0;
    const uint8_t* P = song.synth;
    // envelope
    switch (sv.stage) {
    case 1: {
        float at = 0.002f + (P[P_ATTACK] / 100.0f) * (P[P_ATTACK] / 100.0f) * 2.0f;
        sv.env += 1.0f / (at * SR);
        if (sv.env >= 1.0f) { sv.env = 1.0f; sv.stage = 2; }
        break;
    }
    case 2: {
        float sus = P[P_SUSTAIN] / 100.0f;
        sv.env = sus + (sv.env - sus) * timeCoef(P[P_DECAY]);
        if (sv.env - sus < 0.001f) sv.stage = 3;
        break;
    }
    case 3: sv.env = P[P_SUSTAIN] / 100.0f; break;
    case 4:
        sv.env *= timeCoef(P[P_RELEASE]);
        if (sv.env < 0.0005f) { sv.env = 0; sv.stage = 0; synthNote = -1; return 0; }
        break;
    }
    int wave = P[P_WAVE] & 3;
    float o = osc(wave, sv.ph1, sv.inc1);
    if (P[P_DETUNE]) o = 0.6f * (o + osc(wave, sv.ph2, sv.inc2));
    sv.ph1 += sv.inc1; if (sv.ph1 >= 1.0f) sv.ph1 -= 1.0f;
    sv.ph2 += sv.inc2; if (sv.ph2 >= 1.0f) sv.ph2 -= 1.0f;

    // filter: Chamberlin SVF, 2x oversampled
    float base = 60.0f * powf(2.0f, P[P_CUTOFF] / 100.0f * 7.2f);
    float fc   = base * powf(2.0f, P[P_ENVMOD] / 100.0f * 5.0f * sv.env);
    if (fc > 8000.0f) fc = 8000.0f;
    float f = 2.0f * sinf(3.14159265f * fc / (2.0f * SR));
    float q = 2.0f - P[P_RESO] / 100.0f * 1.88f;
    for (int k = 0; k < 2; k++) {
        sv.low += f * sv.band;
        float high = o - sv.low - q * sv.band;
        sv.band += f * high;
        if (sv.band > 3.0f) sv.band = 3.0f; else if (sv.band < -3.0f) sv.band = -3.0f;
    }
    return sv.low * sv.env * (P[P_VOLUME] / 100.0f) * 0.7f;
}

// ---------------------------------------------------------------- sequencer
static void advanceStep()
{
    int st = curStep + 1;
    if (st >= STEPS) st = 0;
    if (st == 0 && queuedPattern >= 0) { curPattern = queuedPattern; queuedPattern = -1; }
    curStep = st;
    int p = curPattern;
    for (int t = 0; t < NUM_SLOTS; t++) {
        uint8_t v = song.steps[p][t][st];
        if (v && !song.mute[t]) trigDrum(t, v == 2);
    }
    int n = song.notes[p][st];
    if (n >= 0 && !song.mute[SYNTH_TRACK]) {
        synthOn(n, true);
        liveHeld = false;
        seqGate = (int)(stepLen * 0.6f);
    }
}

static void handleEvents()
{
    Event e;
    while (xQueueReceive(evq, &e, 0) == pdTRUE) {
        switch ((int)e.type) {
        case EV_NOTE_ON:  synthOn(e.a, false); seqGate = 0; break;
        case EV_NOTE_OFF: liveHeld = false; synthOff(e.a); break;
        case EV_TRIG:     if (e.a == SYNTH_TRACK) { synthOn(e.b, true); seqGate = (int)(SR * 0.15f); }
                          else trigDrum(e.a, false);
                          break;
        case EV_START:
            curStep = -1;
            stepPos = 1e9f;
            playing = true;
            break;
        case EV_STOP:
            playing = false;
            curStep = 0;
            if (!liveHeld) synthOff(-1);
            break;
        }
    }
}

static void render(int16_t* out)
{
    handleEvents();
    float spb = (float)SR * 60.0f / (song.bpm * 4.0f);
    stepLen = spb;
    uint32_t sp = scopePos;
    for (int i = 0; i < BLOCK; i++) {
        if (playing) {
            float pos = stepPos + 1.0f;
            if (pos >= spb) { pos = 0; advanceStep(); }
            stepPos = pos;
        }
        if (seqGate > 0 && --seqGate == 0 && !liveHeld) synthOff(-1);

        float x = 0;
        for (int t = 0; t < NUM_SLOTS; t++) x += renderDrum(t) * 0.75f;
        x += renderSynth();
        // gentle saturation, the "grit"
        x = x / (1.0f + fabsf(x) * 0.6f) * 1.25f;
        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
        int16_t s = (int16_t)(x * 32000.0f);
        out[i] = s;
        scope[sp++ & (SCOPE_LEN - 1)] = s;
    }
    scopePos = sp;
}

static void audioTask(void*)
{
    int bi = 0;
    for (;;) {
        if (reqPause) { idle = true; vTaskDelay(5); continue; }
        if (M5Cardputer.Speaker.isPlaying(0) >= 2) { vTaskDelay(1); continue; }
        M5Cardputer.Speaker.setVolume(master);
        render(bufs[bi]);
        M5Cardputer.Speaker.playRaw(bufs[bi], BLOCK, SR, false, 1, 0);
        bi = (bi + 1) % 3;
    }
}

// ---------------------------------------------------------------- default kit
static float frand() { return (float)esp_random() / 2147483648.0f - 1.0f; }

void makeDefaultSound(int i)
{
    Slot& s = slots[i];
    s.len = 0;
    s.rate = SR;
    s.pitch = 0;
    s.reverse = false;
    uint32_t n = 0;
    float ph = 0, hp = 0, prev = 0;
    switch (i) {
    case 0:  // kick: pitch-swept sine with a click
        strcpy(s.name, "KICK"); s.vol = 90;
        n = SR * 45 / 100;
        for (uint32_t k = 0; k < n; k++) {
            float t = (float)k / SR;
            float f = 45.0f + 110.0f * expf(-t * 28.0f);
            ph += f / SR;
            float v = sinf(ph * 6.2831853f) * expf(-t * 7.0f);
            if (k < 40) v += frand() * 0.3f * (1.0f - k / 40.0f);
            v = tanhf(v * 1.8f);
            s.data[k] = (int16_t)(v * 30000);
        }
        break;
    case 1:  // snare: tone + noise
        strcpy(s.name, "SNARE"); s.vol = 75;
        n = SR * 28 / 100;
        for (uint32_t k = 0; k < n; k++) {
            float t = (float)k / SR;
            ph += 185.0f / SR;
            float tone = sinf(ph * 6.2831853f) * expf(-t * 30.0f) * 0.6f;
            float nz = frand();
            hp = nz - prev; prev = nz;
            float noise = (nz * 0.4f + hp * 0.6f) * expf(-t * 16.0f);
            s.data[k] = (int16_t)(tanhf((tone + noise) * 1.4f) * 28000);
        }
        break;
    case 2:  // hat: high-passed noise, short
        strcpy(s.name, "HAT"); s.vol = 55;
        n = SR * 7 / 100;
        for (uint32_t k = 0; k < n; k++) {
            float t = (float)k / SR;
            float nz = frand();
            hp = nz - prev; prev = nz;
            s.data[k] = (int16_t)(hp * 0.5f * expf(-t * 60.0f) * 30000);
        }
        break;
    default:  // clap: three bursts then a tail
        strcpy(s.name, "CLAP"); s.vol = 70;
        n = SR * 30 / 100;
        for (uint32_t k = 0; k < n; k++) {
            float t = (float)k / SR;
            float burst = 0;
            for (int b = 0; b < 3; b++) {
                float tb = t - b * 0.011f;
                if (tb >= 0 && tb < 0.011f) burst = expf(-tb * 180.0f);
            }
            float tail = t > 0.033f ? expf(-(t - 0.033f) * 14.0f) : 0;
            float nz = frand();
            hp = nz - prev; prev = nz;
            float v = (hp * 0.7f + nz * 0.3f) * (burst + tail * 0.8f);
            s.data[k] = (int16_t)(tanhf(v * 1.5f) * 28000);
        }
        break;
    }
    s.len = n;
}

// ---------------------------------------------------------------- public api
void begin()
{
    evq = xQueueCreate(32, sizeof(Event));
    memset(song.steps, 0, sizeof(song.steps));
    memset(song.notes, -1, sizeof(song.notes));
    memset(song.mute, 0, sizeof(song.mute));
    const uint8_t defs[P_COUNT] = {0, 30, 35, 55, 55, 0, 30, 40, 25, 70};
    memcpy(song.synth, defs, sizeof(defs));
    // a starter beat in pattern 1
    for (int s = 0; s < STEPS; s += 4) song.steps[0][0][s] = 1;
    song.steps[0][1][4] = 1; song.steps[0][1][12] = 1;
    for (int s = 2; s < STEPS; s += 4) song.steps[0][2][s] = 1;
    const int8_t bass[STEPS] = {36, -1, -1, 36, -1, -1, 39, -1, 36, -1, 43, -1, -1, 41, -1, 39};
    memcpy(song.notes[0], bass, sizeof(bass));

    for (int i = 0; i < NUM_SLOTS; i++) {
        slots[i].data = (int16_t*)heap_caps_malloc(SLOT_MAX * sizeof(int16_t), MALLOC_CAP_8BIT);
        makeDefaultSound(i);
    }
    M5Cardputer.Speaker.setChannelVolume(0, 255);
    xTaskCreatePinnedToCore(audioTask, "pg_audio", 4096, nullptr, 3, nullptr, 0);
}

void post(EvType t, int8_t a, uint8_t b)
{
    Event e{t, a, b};
    xQueueSend(evq, &e, 0);
}

void setPlaying(bool on) { post((EvType)(on ? EV_START : EV_STOP), 0); }

void pause()
{
    if (reqPause) return;
    reqPause = true;
    while (!idle) vTaskDelay(2);
    while (M5Cardputer.Speaker.isPlaying()) vTaskDelay(2);
    M5Cardputer.Speaker.end();
}

void resume()
{
    if (!reqPause) return;
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setChannelVolume(0, 255);
    for (auto& v : dv) v.active = false;
    idle = false;
    reqPause = false;
}

bool paused() { return reqPause; }

float stepPhase()
{
    float l = stepLen;
    float p = stepPos / (l > 1 ? l : 1);
    return p > 1 ? 1 : p;
}

}  // namespace engine
