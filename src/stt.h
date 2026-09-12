#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Speech-to-text: record the spoken phrase, upload it and return the text.
// ---------------------------------------------------------------------------

// Allocates the PSRAM buffer that holds the multipart transcription request
// (head + WAV header + PCM + tail). Call once at boot.
bool initStt();

// Captures a spoken phrase into the request buffer using the shared audio path.
// Returns the number of captured samples, or 0 when no speech was detected.
size_t sttRecord();

// Uploads the last captured phrase to the transcription endpoint and returns
// the transcript ("" when the request or the response was unusable).
String sttTranscribe();
