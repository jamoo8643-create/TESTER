// Pocket Grit - SD card: WAV import/export, project save/load
#pragma once
#include <Arduino.h>
#include <vector>

namespace storage {
bool begin();
bool ok();
// Fills `out` with up to `max` .wav paths found in /, /samples, /groovebox
void listWavs(std::vector<String>& out, size_t max = 80);
bool loadWav(const char* path, int slot, String& err);
bool saveWav(const char* path, int slot);
bool saveProject(String& msg);
bool loadProject(String& msg);
}
