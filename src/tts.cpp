#include "tts.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "app_config.h"
#include "audio_io.h"
#include "openai_http.h"

namespace {

// Collects a binary HTTP response body into a fixed buffer. It derives from
// Stream because HTTPClient::writeToStream() expects a Stream.
class BufferStream : public Stream {
public:
  BufferStream(uint8_t *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t *data, size_t len) override {
    const size_t room = capacity_ - size_;
    const size_t copy = (len < room) ? len : room;
    if (copy > 0) {
      memcpy(buffer_ + size_, data, copy);
      size_ += copy;
    }
    if (copy < len)
      overflowed_ = true;
    // Always report the full length: HTTPClient treats a short write as a
    // failed transfer and aborts the download.
    return len;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  size_t size() const { return size_; }
  bool overflowed() const { return overflowed_; }

private:
  uint8_t *buffer_;
  size_t capacity_;
  size_t size_ = 0;
  bool overflowed_ = false;
};

uint8_t *pcmBuffer = nullptr;
size_t pcmCapacityBytes = 0;

} // namespace

bool initTts() {
  // OpenAI's "pcm" output is 24 kHz, 16-bit mono, so this covers MAX_TTS_SECS.
  const size_t want = (size_t)TTS_PCM_SAMPLE_RATE * 2 * MAX_TTS_SECS;
  pcmBuffer = (uint8_t *)heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (pcmBuffer == nullptr)
    pcmBuffer = (uint8_t *)malloc(want);
  if (pcmBuffer == nullptr) {
    Serial.println("Failed to allocate TTS buffer");
    return false;
  }

  pcmCapacityBytes = want;
  Serial.printf("TTS buffer: %u bytes (~%d s)\n", (unsigned)want, MAX_TTS_SECS);
  return true;
}

bool ttsSpeak(const String &text) {
  if (pcmBuffer == nullptr || text.isEmpty())
    return false;

  JsonDocument request;
  request["model"] = TTS_MODEL;
  request["voice"] = TTS_VOICE;
  request["input"] = text;
  request["response_format"] = "pcm"; // raw 24 kHz, 16-bit signed LE, mono
  request["speed"] = 1.0;

  String payload;
  serializeJson(request, payload);

  BufferStream sink(pcmBuffer, pcmCapacityBytes);
  if (httpsPostToStream(TTS_PATH, payload, sink) != HTTP_OK_STATUS)
    return false;

  if (sink.overflowed())
    Serial.println("TTS audio truncated (buffer full)");
  if (sink.size() < 2) {
    Serial.println("TTS returned no audio");
    return false;
  }

  // Ignore a trailing odd byte so only whole 16-bit samples are played.
  const size_t samples = sink.size() / 2;
  const size_t played = playPcmMono((const int16_t *)pcmBuffer, samples, TTS_PCM_SAMPLE_RATE);
  Serial.printf("Spoke %u samples (~%u ms)\n", (unsigned)played,
                (unsigned)((uint64_t)played * 1000 / TTS_PCM_SAMPLE_RATE));
  return played > 0;
}
