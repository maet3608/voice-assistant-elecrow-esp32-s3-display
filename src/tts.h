#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Text-to-speech: synthesize the answer and play it on the loudspeaker.
// ---------------------------------------------------------------------------

// Allocates the PSRAM buffer that holds the synthesized audio. Call once at boot.
bool initTts();

// Downloads the spoken form of `text` and plays it through the speaker.
// Returns false when nothing could be synthesized or played.
bool ttsSpeak(const String &text);
