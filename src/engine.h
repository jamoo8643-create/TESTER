// Pocket Grit - audio engine (sequencer, sample voices, synth voice)
#pragma once
#include <Arduino.h>

static constexpr uint32_t SR          = 22050;   // engine sample rate
static constexpr int      BLOCK       = 256;     // samples per audio block
static constexpr int      NUM_SLOTS   = 4;       // sample slots / drum tracks
static constexpr int      NUM_TRACKS  = 5;       // 4 drum + 1 synth
static constexpr int      SYNTH_TRACK = 4;
static constexpr int      STEPS       = 16;
static constexpr int      NUM_PATTERNS = 4;
static constexpr uint32_t SLOT_MAX    = 16384;   // samples per slot
static constexpr int      SCOPE_LEN   = 1024;

struct Slot {
    int16_t*          data   = nullptr;
    volatile uint32_t len    = 0;
    uint32_t          rate   = SR;
    int8_t            pitch  = 0;     // semitones
    uint8_t           vol    = 80;    // 0..100
    bool              reverse = false;
    char              name[16] = "";
};

enum SynthParam {
    P_WAVE, P_DETUNE, P_CUTOFF, P_RESO, P_ENVMOD,
    P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE, P_VOLUME, P_COUNT
};

struct Song {
    uint16_t bpm = 112;
    uint8_t  steps[NUM_PATTERNS][NUM_SLOTS][STEPS];   // 0 off, 1 on, 2 accent
    int8_t   notes[NUM_PATTERNS][STEPS];              // -1 rest, else MIDI note
    uint8_t  synth[P_COUNT];                          // 0..100 (wave 0..3)
    uint8_t  mute[NUM_TRACKS];
};

enum EvType : uint8_t { EV_NOTE_ON, EV_NOTE_OFF, EV_TRIG };
struct Event { EvType type; int8_t a; uint8_t b; };

namespace engine {
extern Slot  slots[NUM_SLOTS];
extern Song  song;
extern volatile bool    playing;
extern volatile int     curStep;
extern volatile int     curPattern;
extern volatile int     queuedPattern;
extern volatile uint8_t master;      // 0..255 speaker volume
extern volatile uint8_t trackFlash[NUM_TRACKS];
extern volatile int     synthNote;   // currently sounding note (-1 none)
extern int16_t          scope[SCOPE_LEN];
extern volatile uint32_t scopePos;

void begin();
void setPlaying(bool on);
void post(EvType t, int8_t a, uint8_t b = 0);
void pause();    // stop rendering + release speaker (blocks until idle)
void resume();
bool paused();
void makeDefaultSound(int slot);
float stepPhase();  // 0..1 position inside current step
}
