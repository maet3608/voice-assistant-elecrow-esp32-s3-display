#pragma once

#include <Arduino.h>

// Shared audio I/O: ES8311 codec + I2S capture and playback. Both directions
// use one I2S bus (I2S_NUM_1) and one codec clock, so they live together.
// Playback audio is resampled to SAMPLE_RATE, the rate the codec runs at.

// Initializes the I2S bus, the ES8311 codec and the speaker amplifier.
bool initAudio();

// Captures one spoken phrase into `pcm` (16-bit mono, SAMPLE_RATE) using
// energy-based voice activity detection. Returns the number of captured
// samples with trailing silence trimmed, or 0 when no speech was detected.
size_t recordSpeech(int16_t *pcm, size_t maxSamples);

// Plays 16-bit mono PCM through the speaker, resampling from `srcSampleRate` to
// SAMPLE_RATE and duplicating each sample into both I2S channels. Returns the
// number of source samples played.
size_t playPcmMono(const int16_t *pcm, size_t samples, int srcSampleRate);
