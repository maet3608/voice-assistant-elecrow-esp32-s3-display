#include "audio_io.h"

#include <math.h>

#include "AudioTools.h"
#include "app_config.h"
#include "es8311.h"

using namespace audio_tools;

namespace {

I2SStream i2sStream;
bool ready = false;

// Number of stereo frames moved per I2S access.
constexpr size_t CHUNK_FRAMES = 256;
constexpr size_t CHUNK_BYTES = CHUNK_FRAMES * 4; // 2 channels x 2 bytes
constexpr int DRAIN_BLOCKS = 4;                  // silence pushed after playback

int16_t chunk[CHUNK_FRAMES * 2];

// Reads one chunk of stereo frames, downmixes it to mono and appends it to
// `pcm`. Returns the RMS of the chunk (0 when nothing could be read).
float readMonoChunk(int16_t *pcm, size_t &samples, size_t maxSamples) {
  const size_t bytesRead = i2sStream.readBytes((uint8_t *)chunk, CHUNK_BYTES);
  const int nFrames = (int)(bytesRead / 4);
  int64_t sumSq = 0;

  for (int i = 0; i < nFrames; i++) {
    // Sum both channels so we don't depend on the mono mic's L/R position.
    const int32_t sum = (int32_t)chunk[i * 2] + chunk[i * 2 + 1];
    const int16_t mono = (int16_t)constrain(sum, -32768, 32767);
    if (samples < maxSamples)
      pcm[samples++] = mono;
    sumSq += (int64_t)mono * mono;
  }

  return (nFrames > 0) ? sqrtf((float)sumSq / (float)nFrames) : 0.0f;
}

} // namespace

bool initAudio() {
  pinMode(AMP_ENABLE_PIN, OUTPUT);
  digitalWrite(AMP_ENABLE_PIN, AMP_ENABLE_ACTIVE_LEVEL);

  auto cfg = i2sStream.defaultConfig(RXTX_MODE);
  cfg.port_no = I2S_PORT_NO;
  cfg.is_master = true;
  cfg.pin_mck = I2S_MCK_PIN;
  cfg.pin_bck = I2S_BCK_PIN;
  cfg.pin_ws = I2S_WS_PIN;
  cfg.pin_data = I2S_DOUT_PIN;
  cfg.pin_data_rx = I2S_DIN_PIN;
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = 16;
  cfg.channels = 2;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.buffer_count = 8;
  cfg.buffer_size = 256;
  cfg.auto_clear = true;

  if (!i2sStream.begin(cfg)) {
    Serial.println("I2S driver init failed");
    return false;
  }

  if (es8311_codec_init() != ESP_OK) {
    Serial.println("ES8311 codec init failed");
    return false;
  }

  if (es8311_set_output_volume(OUTPUT_VOLUME_PERCENT, OUTPUT_VOLUME_RANGE_DB) != ESP_OK) {
    Serial.println("ES8311 volume set failed");
    return false;
  }

  ready = true;
  return true;
}

size_t recordSpeech(int16_t *pcm, size_t maxSamples) {
  if (!ready || pcm == nullptr || maxSamples == 0)
    return 0;

  size_t samples = 0;
  bool speech = false;
  size_t lastVoiceSample = 0;
  float peakRms = 0;
  const uint32_t startMs = millis();

  // Flush stale DMA data before capturing.
  i2sStream.readBytes((uint8_t *)chunk, sizeof(chunk));

  while (true) {
    const float rms = readMonoChunk(pcm, samples, maxSamples);
    if (rms > peakRms)
      peakRms = rms;
    if (rms > VAD_THRESHOLD) {
      speech = true;
      lastVoiceSample = samples;
    }

    const uint32_t elapsedMs = millis() - startMs;
    const uint32_t silenceMs = (uint32_t)((samples - lastVoiceSample) * 1000 / SAMPLE_RATE);
    const bool timedOut = speech ? (silenceMs >= SILENCE_TIMEOUT_MS)
                                 : (elapsedMs > NO_SPEECH_TIMEOUT_MS);
    if (timedOut || elapsedMs > (uint32_t)(MAX_RECORD_SECS * 1000) || samples >= maxSamples)
      break;
  }

  Serial.printf("[DIAG] speech=%d peakRMS=%d samples=%u\n", (int)speech, (int)peakRms,
                (unsigned)samples);
  if (!speech)
    return 0;

  // Trim trailing silence, keeping a little padding after the last loud sample.
  size_t endSample = lastVoiceSample + (size_t)(SAMPLE_RATE * SILENCE_PAD_MS / 1000);
  if (endSample > samples)
    endSample = samples;
  return endSample;
}

size_t playPcmMono(const int16_t *pcm, size_t samples, int srcSampleRate) {
  if (!ready || pcm == nullptr || samples == 0 || srcSampleRate <= 0)
    return 0;

  const bool resample = (srcSampleRate != SAMPLE_RATE);
  const size_t outSamples =
      resample ? (size_t)((uint64_t)samples * SAMPLE_RATE / (uint64_t)srcSampleRate) : samples;
  if (outSamples == 0)
    return 0;

  int16_t stereo[CHUNK_FRAMES * 2];
  size_t framesInBlock = 0;

  auto flushBlock = [&]() {
    if (framesInBlock == 0)
      return;
    i2sStream.write((uint8_t *)stereo, framesInBlock * 4);
    framesInBlock = 0;
  };

  for (size_t i = 0; i < outSamples; i++) {
    int16_t sample;
    if (!resample) {
      sample = pcm[i];
    } else {
      // Map the output index onto a fractional source position and interpolate
      // linearly (24 kHz -> 16 kHz is a ratio of 3/2).
      const uint64_t pos = (uint64_t)i * (uint64_t)srcSampleRate;
      const size_t index = (size_t)(pos / (uint64_t)SAMPLE_RATE);
      const uint32_t frac = (uint32_t)(pos % (uint64_t)SAMPLE_RATE);
      const int16_t a = pcm[index];
      const int16_t b = (index + 1 < samples) ? pcm[index + 1] : a;
      sample = (int16_t)((int32_t)a + ((int32_t)(b - a) * (int32_t)frac) / SAMPLE_RATE);
    }

    stereo[framesInBlock * 2] = sample;     // left
    stereo[framesInBlock * 2 + 1] = sample; // right
    if (++framesInBlock == CHUNK_FRAMES)
      flushBlock();
  }
  flushBlock();

  // Push the tail of the audio out of the DMA queue and leave silence behind.
  memset(stereo, 0, sizeof(stereo));
  for (int i = 0; i < DRAIN_BLOCKS; i++)
    i2sStream.write((uint8_t *)stereo, CHUNK_BYTES);

  return samples;
}
