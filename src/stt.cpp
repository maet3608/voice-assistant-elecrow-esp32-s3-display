#include "stt.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "app_config.h"
#include "audio_io.h"
#include "openai_http.h"

// Must be a macro so it can be concatenated into the multipart head below.
#define WAV_BOUNDARY "----ESP32Boundary7MA4YWxk"

namespace {

// The upload is one contiguous buffer, so the request can be posted with a
// single HTTPClient call:
//   [MULTIPART_HEAD][44 byte WAV header][PCM][MULTIPART_TAIL]
const char MULTIPART_HEAD[] =
    "--" WAV_BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n" TRANSCRIBE_MODEL "\r\n"
    "--" WAV_BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
const char MULTIPART_TAIL[] = "\r\n--" WAV_BOUNDARY "--\r\n";

constexpr size_t HEAD_LEN = sizeof(MULTIPART_HEAD) - 1;
constexpr size_t TAIL_LEN = sizeof(MULTIPART_TAIL) - 1;
constexpr size_t WAV_HEADER_LEN = 44;
constexpr size_t MAX_PCM_BYTES = (size_t)SAMPLE_RATE * 2 * MAX_RECORD_SECS;
constexpr size_t MIN_PCM_BYTES = (size_t)SAMPLE_RATE * 2 * 2; // fallback: ~2 s of audio

#pragma pack(push, 1)
struct WavHeader {
  char riff[4];
  uint32_t riffSize;
  char wave[4];
  char fmt[4];
  uint32_t fmtSize;
  uint16_t audioFormat;
  uint16_t channels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char data[4];
  uint32_t dataSize;
};
#pragma pack(pop)
static_assert(sizeof(WavHeader) == WAV_HEADER_LEN, "WAV header must be 44 bytes");

uint8_t *reqBuf = nullptr;      // head + WAV header + PCM + tail
int16_t *pcmArea = nullptr;     // reqBuf + HEAD_LEN + WAV_HEADER_LEN
size_t pcmCapacitySamples = 0;  // usable samples, determined at init
size_t capturedSamples = 0;     // samples captured by the last sttRecord()

// Prefers PSRAM, falls back to internal RAM and finally to a smaller buffer.
uint8_t *allocRequestBuffer(size_t &bytes) {
  const size_t minBytes = HEAD_LEN + WAV_HEADER_LEN + MIN_PCM_BYTES + TAIL_LEN;
  while (bytes >= minBytes) {
    uint8_t *buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr)
      buf = (uint8_t *)malloc(bytes);
    if (buf != nullptr)
      return buf;
    bytes /= 2;
  }
  return nullptr;
}

void writeWavHeader(uint8_t *buf, uint32_t dataBytes) {
  WavHeader h = {};
  memcpy(h.riff, "RIFF", 4);
  h.riffSize = 36 + dataBytes;
  memcpy(h.wave, "WAVE", 4);
  memcpy(h.fmt, "fmt ", 4);
  h.fmtSize = 16;
  h.audioFormat = 1; // PCM
  h.channels = 1;
  h.sampleRate = SAMPLE_RATE;
  h.bitsPerSample = 16;
  h.blockAlign = h.channels * (h.bitsPerSample / 8);
  h.byteRate = h.sampleRate * h.blockAlign;
  memcpy(h.data, "data", 4);
  h.dataSize = dataBytes;
  memcpy(buf, &h, sizeof(h));
}

} // namespace

bool initStt() {
  size_t want = HEAD_LEN + WAV_HEADER_LEN + MAX_PCM_BYTES + TAIL_LEN;
  reqBuf = allocRequestBuffer(want);
  if (reqBuf == nullptr) {
    Serial.println("Failed to allocate audio buffer");
    return false;
  }

  pcmArea = (int16_t *)(reqBuf + HEAD_LEN + WAV_HEADER_LEN);
  const size_t pcmBytes = want - HEAD_LEN - WAV_HEADER_LEN - TAIL_LEN;
  pcmCapacitySamples = pcmBytes / 2;

  Serial.printf("Audio buffer: %u bytes (~%u ms)\n", (unsigned)pcmBytes,
                (unsigned)((uint64_t)pcmCapacitySamples * 1000 / SAMPLE_RATE));
  return true;
}

size_t sttRecord() {
  if (reqBuf == nullptr)
    return 0;
  capturedSamples = recordSpeech(pcmArea, pcmCapacitySamples);
  return capturedSamples;
}

String sttTranscribe() {
  if (reqBuf == nullptr || capturedSamples == 0)
    return "";

  const uint32_t dataBytes = (uint32_t)(capturedSamples * 2);
  writeWavHeader(reqBuf + HEAD_LEN, dataBytes);
  memcpy(reqBuf, MULTIPART_HEAD, HEAD_LEN);
  memcpy(reqBuf + HEAD_LEN + WAV_HEADER_LEN + dataBytes, MULTIPART_TAIL, TAIL_LEN);

  const size_t bodyLen = HEAD_LEN + WAV_HEADER_LEN + dataBytes + TAIL_LEN;
  const String contentType = String("multipart/form-data; boundary=") + WAV_BOUNDARY;

  String response;
  if (httpsPostMultipart(STT_PATH, contentType.c_str(), reqBuf, bodyLen, response) !=
      HTTP_OK_STATUS)
    return "";

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, response);
  const char *text = err ? nullptr : doc["text"].as<const char *>();
  if (text == nullptr) {
    Serial.printf("STT response unusable (%s)\n", err ? err.c_str() : "no 'text' field");
    return "";
  }
  return String(text);
}
