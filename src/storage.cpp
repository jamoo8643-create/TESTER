// Pocket Grit - SD card helpers
#include "storage.h"
#include "engine.h"
#include <SD.h>
#include <SPI.h>

namespace storage {

static bool    sdOk = false;
static SPIClass sdSpi(HSPI);
static const char* DIR = "/groovebox";

bool begin()
{
    sdSpi.begin(40, 39, 14, 12);  // SCK, MISO, MOSI, CS on Cardputer / ADV
    sdOk = SD.begin(12, sdSpi, 25000000);
    if (sdOk && !SD.exists(DIR)) SD.mkdir(DIR);
    return sdOk;
}

bool ok() { return sdOk; }

static bool isWav(const String& n)
{
    String l = n;
    l.toLowerCase();
    return l.endsWith(".wav") && !l.startsWith("._");
}

static void scanDir(const char* dir, std::vector<String>& out, size_t max)
{
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return;
    while (out.size() < max) {
        File f = d.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            String name = f.name();
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            if (isWav(name)) {
                String p = String(dir);
                if (!p.endsWith("/")) p += "/";
                out.push_back(p + name);
            }
        }
        f.close();
    }
    d.close();
}

void listWavs(std::vector<String>& out, size_t max)
{
    out.clear();
    if (!sdOk) return;
    scanDir("/groovebox", out, max);
    scanDir("/samples", out, max);
    scanDir("/Samples", out, max);
    scanDir("/", out, max);
}

static uint32_t rd32(File& f) { uint8_t b[4]; f.read(b, 4); return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t rd16(File& f) { uint8_t b[2]; f.read(b, 2); return b[0] | (b[1] << 8); }

bool loadWav(const char* path, int slotIdx, String& err)
{
    File f = SD.open(path, FILE_READ);
    if (!f) { err = "can't open"; return false; }
    char id[5] = {0};
    f.read((uint8_t*)id, 4);
    rd32(f);
    char wv[5] = {0};
    f.read((uint8_t*)wv, 4);
    if (strcmp(id, "RIFF") || strcmp(wv, "WAVE")) { err = "not a WAV"; f.close(); return false; }

    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0, dataLen = 0;
    bool haveData = false;
    while (f.available() >= 8) {
        char cid[5] = {0};
        f.read((uint8_t*)cid, 4);
        uint32_t clen = rd32(f);
        if (!strcmp(cid, "fmt ")) {
            fmt = rd16(f); ch = rd16(f); rate = rd32(f);
            rd32(f); rd16(f); bits = rd16(f);
            if (clen > 16) f.seek(f.position() + (clen - 16));
        } else if (!strcmp(cid, "data")) {
            dataLen = clen;
            haveData = true;
            break;
        } else {
            f.seek(f.position() + clen + (clen & 1));
        }
    }
    if (!haveData || (fmt != 1 && fmt != 0xFFFE) || (bits != 8 && bits != 16) || ch < 1 || ch > 2 || rate < 4000) {
        err = "need 8/16-bit PCM";
        f.close();
        return false;
    }

    Slot& s = engine::slots[slotIdx];
    s.len = 0;
    uint32_t dec = (rate + 23999) / 24000;  // integer decimation to <= 24 kHz
    if (dec < 1) dec = 1;
    const uint32_t bps = bits / 8 * ch;
    uint32_t frames = dataLen / bps;
    uint32_t outN = 0;
    static uint8_t buf[1024];
    int32_t acc = 0;
    uint32_t accN = 0;
    while (frames && outN < SLOT_MAX) {
        uint32_t want = sizeof(buf) / bps;
        if (want > frames) want = frames;
        int got = f.read(buf, want * bps);
        if (got <= 0) break;
        uint32_t fr = got / bps;
        frames -= fr;
        for (uint32_t i = 0; i < fr && outN < SLOT_MAX; i++) {
            int32_t v = 0;
            for (int c = 0; c < ch; c++) {
                const uint8_t* p = buf + i * bps + c * (bits / 8);
                v += bits == 16 ? (int16_t)(p[0] | (p[1] << 8)) : ((int)p[0] - 128) << 8;
            }
            acc += v / ch;
            if (++accN == dec) {
                s.data[outN++] = (int16_t)(acc / (int32_t)dec);
                acc = 0;
                accN = 0;
            }
        }
    }
    f.close();
    if (outN < 16) { err = "empty"; return false; }
    // short fade-out to avoid clicks when truncated
    for (uint32_t i = 0; i < 64 && i < outN; i++) {
        uint32_t k = outN - 1 - i;
        s.data[k] = (int16_t)((int32_t)s.data[k] * (int32_t)i / 64);
    }
    s.rate = rate / dec;
    s.pitch = 0;
    s.reverse = false;
    String n = path;
    n = n.substring(n.lastIndexOf('/') + 1);
    n.replace(".wav", ""); n.replace(".WAV", "");
    n.toUpperCase();
    strlcpy(s.name, n.c_str(), sizeof(s.name));
    s.len = outN;
    return true;
}

static void wr32(File& f, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; f.write(b, 4); }
static void wr16(File& f, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; f.write(b, 2); }

bool saveWav(const char* path, int slotIdx)
{
    if (!sdOk) return false;
    Slot& s = engine::slots[slotIdx];
    uint32_t n = s.len;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.write((const uint8_t*)"RIFF", 4); wr32(f, 36 + n * 2);
    f.write((const uint8_t*)"WAVEfmt ", 8); wr32(f, 16);
    wr16(f, 1); wr16(f, 1); wr32(f, s.rate); wr32(f, s.rate * 2); wr16(f, 2); wr16(f, 16);
    f.write((const uint8_t*)"data", 4); wr32(f, n * 2);
    f.write((const uint8_t*)s.data, n * 2);
    f.close();
    return true;
}

struct SlotMeta { int8_t pitch; uint8_t vol; uint8_t reverse; char name[16]; };

bool saveProject(String& msg)
{
    if (!sdOk) { msg = "no SD card"; return false; }
    File f = SD.open("/groovebox/project.pgr", FILE_WRITE);
    if (!f) { msg = "write failed"; return false; }
    f.write((const uint8_t*)"PGR1", 4);
    f.write((const uint8_t*)&engine::song, sizeof(Song));
    for (int i = 0; i < NUM_SLOTS; i++) {
        SlotMeta m{engine::slots[i].pitch, engine::slots[i].vol, engine::slots[i].reverse, {0}};
        memcpy(m.name, engine::slots[i].name, 16);
        f.write((const uint8_t*)&m, sizeof(m));
    }
    f.close();
    for (int i = 0; i < NUM_SLOTS; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/groovebox/slot%d.wav", i + 1);
        saveWav(p, i);
    }
    msg = "saved /groovebox";
    return true;
}

bool loadProject(String& msg)
{
    if (!sdOk) { msg = "no SD card"; return false; }
    File f = SD.open("/groovebox/project.pgr", FILE_READ);
    if (!f) { msg = "no saved project"; return false; }
    char magic[5] = {0};
    f.read((uint8_t*)magic, 4);
    if (strcmp(magic, "PGR1") || f.size() < 4 + sizeof(Song)) { f.close(); msg = "bad project"; return false; }
    Song tmp;
    f.read((uint8_t*)&tmp, sizeof(Song));
    SlotMeta meta[NUM_SLOTS];
    bool haveMeta = f.read((uint8_t*)meta, sizeof(meta)) == sizeof(meta);
    f.close();
    engine::song = tmp;
    for (int i = 0; i < NUM_SLOTS; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/groovebox/slot%d.wav", i + 1);
        String err;
        if (SD.exists(p)) loadWav(p, i, err);
        if (haveMeta) {
            engine::slots[i].pitch = meta[i].pitch;
            engine::slots[i].vol = meta[i].vol;
            engine::slots[i].reverse = meta[i].reverse;
            meta[i].name[15] = 0;
            strlcpy(engine::slots[i].name, meta[i].name, 16);
        }
    }
    msg = "project loaded";
    return true;
}

}  // namespace storage
