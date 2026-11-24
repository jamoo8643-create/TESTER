# Pocket Grit

A pocket groovebox for the M5Stack Cardputer and Cardputer ADV: four sample tracks, a mono synth, a 16-step sequencer, mic sampling, WAV loading from SD and a live oscilloscope.

## Install (M5Launcher)

1. Grab `PocketGrit.bin` from the `firmware` branch (or the latest Actions run).
2. Copy it to your SD card, e.g. `/apps/PocketGrit.bin`.
3. In M5Launcher choose SD, pick the file and install it.

## Pages

`Tab` goes to the next page, `` ` `` to the previous one, `Space` plays and stops everywhere. If the screen comes up sideways or upside down, press `\` until it's right; the app remembers your choice.

### SEQ: the beat
| Key | Does |
| --- | --- |
| `1`-`8`, `q`-`i` | toggle steps 1-16 on the selected track (hold `Shift` for an accent) |
| `a s d f g` | select track 1-4 or the synth track (S) |
| `z x c v b` | play pads 1-4 and the synth; with REC on while playing they get written into the beat |
| `o` | REC on/off |
| `p` | next pattern (switches at the end of the bar while playing) |
| `n` | copy this pattern into the next one |
| `m` | mute the selected track |
| `-` / `=` | tempo down / up |
| `[` / `]` | master volume |
| `Del` | clear the selected track |
| `k` / `l` | save / load the whole project on SD (`/groovebox`) |

On the synth track a step uses the last note you played.

### SYN: the synth
| Key | Does |
| --- | --- |
| `a s d f g h j k` | white keys C to C, `w e t y u` are the black keys |
| `z` / `x` | octave down / up |
| `[` / `]` | pick a parameter, `-` / `=` change it (`Shift` for fine steps) |
| `c` | cycle the waveform (saw, square, triangle, sine) |
| `o` | REC: while playing, notes land on the beat; while stopped, each note fills the next step (`Del` adds a rest) |

Parameters: wave, detune, cutoff, resonance, filter envelope amount, attack, decay, sustain, release, volume.

### SMP: the sampler
| Key | Does |
| --- | --- |
| `1`-`4` | select a slot (and hear it) |
| `r` or `Enter` | record from the mic into the slot; press any key to stop (about 1 second max) |
| `l` | load a `.wav` from `/groovebox`, `/samples` or the SD root (`;` `.` to move, `Enter` to load) |
| `s` | save the slot as a WAV in `/groovebox` |
| `-` / `=` | pitch in semitones |
| `[` / `]` | slot volume |
| `v` | reverse |
| `d` | back to the built-in sound |

Recordings are trimmed, cleaned of DC offset and normalized automatically. WAVs can be 8 or 16 bit, mono or stereo, any rate; longer files are cut at about 0.7 to 1 second.

## Building

Pushes to `main` build with PlatformIO in GitHub Actions and publish the result to the `firmware` branch. Locally: `pio run -e cardputer`.
