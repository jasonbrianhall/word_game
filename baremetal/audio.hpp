#pragma once
#include <stdint.h>

enum AudioDriver { AUDIO_NONE, AUDIO_HDA, AUDIO_AC97 };

// Detects Intel HD Audio, then AC97. The boot command line can force one:
// audio=hda, audio=ac97 or audio=off.
AudioDriver audio_init(const char* cmdline);
uint32_t audio_play_pos();
uint32_t audio_frames_wanted();                     // frames to write now to stay ahead
void audio_write(const int16_t* samples, int n);    // signed 16-bit mono at audio_rate()
constexpr int audio_rate() { return 48000; }
const char* audio_name();
