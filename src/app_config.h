#pragma once

#include <Arduino.h>

// Central configuration for the Elecrow DLE06235B 3.5" ESP32-S3 voice assistant.

// ---------------------------------------------------------------------------
// I2S / ES8311 codec (capture and playback share this bus)
// Note: these names must not collide with audio-tools' default PIN_I2S_* macros.
constexpr int I2S_BCK_PIN = 18;
constexpr int I2S_WS_PIN = 21;
constexpr int I2S_DIN_PIN = 16;  // codec -> ESP32 (microphone / ADC data)
constexpr int I2S_DOUT_PIN = 15; // ESP32 -> codec (speaker / DAC data)
constexpr int I2S_MCK_PIN = 17;
constexpr int I2S_PORT_NO = 1; // I2S_NUM_1

// The vendored ES8311 driver (lib/ES8311) brings the codec up at this rate with
// an MCLK multiple of 384x, so anything played back must be at this rate.
constexpr int SAMPLE_RATE = 16000;

constexpr int AMP_ENABLE_PIN = 1;
constexpr int AMP_ENABLE_ACTIVE_LEVEL = LOW;

// ---------------------------------------------------------------------------
// Speech-to-text: record the spoken phrase and transcribe it
// ---------------------------------------------------------------------------
constexpr int MAX_RECORD_SECS = 12;
constexpr uint32_t NO_SPEECH_TIMEOUT_MS = 6000; // give up if nothing is spoken
constexpr uint32_t SILENCE_TIMEOUT_MS = 1000;   // stop after this much trailing silence
constexpr uint32_t SILENCE_PAD_MS = 350;        // keep a little audio after last speech
constexpr float VAD_THRESHOLD = 600.0f;         // RMS of 16-bit samples counted as speech

#define TRANSCRIBE_MODEL "gpt-4o-transcribe"

// ---------------------------------------------------------------------------
// Large language model: answer the transcription
// ---------------------------------------------------------------------------
#define LLM_MODEL "gpt-4o-mini"
#define LLM_SYSTEM_PROMPT                                           \
  "You are a helpful voice assistant on a small embedded display. " \
  "Answer concisely in at most 60 words using plain prose."
constexpr int LLM_MAX_TOKENS = 160;
constexpr float LLM_TEMPERATURE = 0.7f;

// ---------------------------------------------------------------------------
// Text-to-speech: speak the answer on the loudspeaker
// ---------------------------------------------------------------------------
#define TTS_MODEL "gpt-4o-mini-tts"
#define TTS_VOICE "alloy"
constexpr int TTS_PCM_SAMPLE_RATE = 24000;
constexpr int MAX_TTS_SECS = 30;

// ---------------------------------------------------------------------------
// Speaker output
// ---------------------------------------------------------------------------
// Speaker level as a percentage of the codec's initialized output level, using a
// logarithmic (dB-linear) taper so it behaves like a normal volume knob: the
// attenuation grows linearly in dB as the percentage drops.
//   100 % -> the level es8311_codec_init() set up (its 85 % default, +12.5 dB)
//   0 %   -> mute
//   OUTPUT_VOLUME_RANGE_DB -> total attenuation spanned between 100 % and 0 %
constexpr int OUTPUT_VOLUME_PERCENT = 70;
constexpr float OUTPUT_VOLUME_RANGE_DB = 40.0f;

// ---------------------------------------------------------------------------
// OpenAI HTTP endpoints
// ---------------------------------------------------------------------------
constexpr const char *OPENAI_HOST = "api.openai.com";
constexpr uint16_t OPENAI_PORT = 443;
constexpr const char *STT_PATH = "/v1/audio/transcriptions";
constexpr const char *LLM_PATH = "/v1/chat/completions";
constexpr const char *TTS_PATH = "/v1/audio/speech";
