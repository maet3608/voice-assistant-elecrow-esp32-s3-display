// ---------------------------------------------------------------------------
// Elecrow DLE06235B 3.5" ESP32-S3 voice assistant
// Touch the screen to record a spoken phrase. The audio is transcribed with
// OpenAI and the result is displayed on screen.
//   - Display/touch initialization is unchanged from the original project.
//   - Audio capture uses the audio-tools I2SStream on top of the ES8311 codec.
//   - Simple energy-based voice activity detection (VAD) ends the recording.
//   - The WAV header and the multipart body are assembled in a single buffer
//     and posted with HTTPClient.
// ---------------------------------------------------------------------------

#include "ST77922.h"
#include "ST77922_Touch.h"
#include <Arduino.h>
#include <TFT_eSPI.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "AudioTools.h" // vendored in lib/audio-tools (I2SStream)
#include "credentials.h"
#include "es8311.h"

using namespace audio_tools;

// ---------------------------------------------------------------------------
// Display & touch (initialization kept as-is)
// ---------------------------------------------------------------------------
TFT_eSPI tft;
TFT_eSprite screen(&tft);
ST77922 display;
ST77922_TOUCH touch;

void initDisplay() {
  display.Init();
  display.Set_Rotation(0);
}

void updateDisplay() {
  display.Fill_Colors(
      0,
      0,
      display.Get_Width(),
      display.Get_Height(),
      static_cast<uint16_t *>(screen.getPointer()));
}

void initScreen() {
  screen.createSprite(display.Get_Width(), display.Get_Height());
  screen.setSwapBytes(true);
  screen.fillSprite(TFT_BLACK);
}

void initTouch() {
  touch.init();
  touch.Set_Rotation(0);
}

// ---------------------------------------------------------------------------
// Audio capture configuration (ES8311 codec + I2S via audio-tools)
// ---------------------------------------------------------------------------
// Note: these names must not collide with audio-tools' default PIN_I2S_* macros.
constexpr int I2S_BCK_PIN = 18;
constexpr int I2S_WS_PIN = 21;
constexpr int I2S_DIN_PIN = 16;  // codec -> ESP32 (microphone data)
constexpr int I2S_DOUT_PIN = 15; // ESP32 -> codec (unused for capture)
constexpr int I2S_MCK_PIN = 17;
constexpr int I2S_PORT_NO = 1;   // I2S_NUM_1

constexpr int SAMPLE_RATE = 16000;
constexpr int MAX_RECORD_SECS = 12;
constexpr uint32_t NO_SPEECH_TIMEOUT_MS = 6000; // give up if nothing is spoken
constexpr uint32_t SILENCE_TIMEOUT_MS = 1300;   // stop after this much trailing silence
constexpr uint32_t SILENCE_PAD_MS = 350;        // keep a little audio after last speech
constexpr float VAD_THRESHOLD = 600.0f;         // RMS of 16-bit samples counted as speech

#define TRANSCRIBE_MODEL "gpt-4o-transcribe"
#define WAV_BOUNDARY "----ESP32Boundary7MA4YWxk"

constexpr const char *OPENAI_HOST = "api.openai.com";
constexpr uint16_t OPENAI_PORT = 443;
constexpr const char *TRANSCRIBE_PATH = "/v1/audio/transcriptions";

I2SStream i2sStream;

// ---------------------------------------------------------------------------
// The multipart request body is built in a single buffer so it can be posted
// with one HTTPClient::POST() call. Layout:
//   [MULTIPART_HEAD][44 byte WAV header][PCM][MULTIPART_TAIL]
// ---------------------------------------------------------------------------
static const char MULTIPART_HEAD[] =
    "--" WAV_BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
    TRANSCRIBE_MODEL "\r\n"
    "--" WAV_BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
static const char MULTIPART_TAIL[] = "\r\n--" WAV_BOUNDARY "--\r\n";

constexpr size_t WAV_HEADER_LEN = 44;
constexpr size_t HEAD_LEN = sizeof(MULTIPART_HEAD) - 1;
constexpr size_t TAIL_LEN = sizeof(MULTIPART_TAIL) - 1;
constexpr size_t MAX_PCM_BYTES = SAMPLE_RATE * 2 * MAX_RECORD_SECS;
constexpr size_t MIN_PCM_BYTES = SAMPLE_RATE * 2 * 2; // fallback: ~2 s of audio

static uint8_t *reqBuf = nullptr;  // head + WAV header + PCM + tail
static uint8_t *pcmArea = nullptr; // reqBuf + HEAD_LEN + WAV_HEADER_LEN
static size_t pcmBufBytes = 0;     // usable PCM bytes, determined at init

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi connection FAILED");
  }
}

// Prefer PSRAM, fall back to internal RAM and to a smaller buffer.
static uint8_t *allocRequestBuffer(size_t &bytes) {
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

// ---------------------------------------------------------------------------
// Audio init: I2S bus (audio-tools) + ES8311 codec + request buffer
// ---------------------------------------------------------------------------
bool initAudio() {
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
  cfg.auto_clear = false;

  if (!i2sStream.begin(cfg)) {
    Serial.println("I2S driver init failed");
    return false;
  }

  // Reuses I2C port 0 (GPIO38/39), already installed by the touch driver.
  if (es8311_codec_init() != ESP_OK) {
    Serial.println("ES8311 codec init failed");
    return false;
  }

  size_t want = HEAD_LEN + WAV_HEADER_LEN + MAX_PCM_BYTES + TAIL_LEN;
  reqBuf = allocRequestBuffer(want);
  if (reqBuf == nullptr) {
    Serial.println("Failed to allocate audio buffer");
    return false;
  }
  pcmArea = reqBuf + HEAD_LEN + WAV_HEADER_LEN;
  pcmBufBytes = want - HEAD_LEN - WAV_HEADER_LEN - TAIL_LEN;
  Serial.printf("Audio buffer: %u bytes (~%u ms)\n", (unsigned)pcmBufBytes,
                (unsigned)(pcmBufBytes / 2 * 1000 / SAMPLE_RATE));
  return true;
}

// ---------------------------------------------------------------------------
// WAV header (44-byte PCM header)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Reads one chunk of stereo frames, downmixes it to mono and appends it to the
// PCM area. Returns the RMS of the chunk (0 when nothing could be read).
// ---------------------------------------------------------------------------
float readMonoChunk(size_t &pcmBytes, int16_t *chunk, size_t frames) {
  size_t bytesRead = i2sStream.readBytes((uint8_t *)chunk, frames * 4);
  const int nFrames = (int)(bytesRead / 4); // 2 channels x 2 bytes
  int64_t sumSq = 0;
  for (int i = 0; i < nFrames; i++) {
    // Sum both channels so we don't depend on the mono mic's L/R position.
    int32_t s = (int32_t)chunk[i * 2] + chunk[i * 2 + 1];
    if (s > 32767)
      s = 32767;
    if (s < -32768)
      s = -32768;
    int16_t mono = (int16_t)s;
    if (pcmBytes + 2 <= pcmBufBytes) {
      pcmArea[pcmBytes++] = (uint8_t)(mono & 0xFF);
      pcmArea[pcmBytes++] = (uint8_t)((mono >> 8) & 0xFF);
    }
    sumSq += (int64_t)mono * mono;
  }
  return (nFrames > 0) ? sqrtf((float)sumSq / (float)nFrames) : 0.0f;
}

// ---------------------------------------------------------------------------
// Recording with simple energy-based voice activity detection (VAD).
// Captures mono 16-bit PCM into the request buffer and completes the multipart
// body. Returns the total request body size, or 0 if no speech was detected.
// ---------------------------------------------------------------------------
size_t recordSpeech() {
  if (reqBuf == nullptr || pcmBufBytes == 0)
    return 0;

  int16_t chunk[512]; // 256 stereo frames
  size_t pcmBytes = 0;
  bool speech = false;
  size_t lastVoiceSample = 0;
  float peakRms = 0;
  const uint32_t startMs = millis();

  // Flush stale DMA data before capturing.
  i2sStream.readBytes((uint8_t *)chunk, sizeof(chunk));

  while (true) {
    const float rms = readMonoChunk(pcmBytes, chunk, 256);
    if (rms > peakRms)
      peakRms = rms;
    if (rms > VAD_THRESHOLD) {
      speech = true;
      lastVoiceSample = pcmBytes / 2;
    }

    const uint32_t elapsedMs = millis() - startMs;
    const uint32_t silenceMs = (uint32_t)((pcmBytes / 2 - lastVoiceSample) * 1000 / SAMPLE_RATE);
    const bool timedOut = speech ? (silenceMs >= SILENCE_TIMEOUT_MS) : (elapsedMs > NO_SPEECH_TIMEOUT_MS);
    if (timedOut || elapsedMs > (uint32_t)(MAX_RECORD_SECS * 1000) || pcmBytes >= pcmBufBytes)
      break;
  }

  Serial.printf("[DIAG] speech=%d peakRMS=%d samples=%u\n", (int)speech, (int)peakRms,
                (unsigned)(pcmBytes / 2));
  if (!speech)
    return 0;

  // Trim trailing silence, keeping a little padding after the last loud sample.
  const size_t totalSamples = pcmBytes / 2;
  size_t endSample = lastVoiceSample + (size_t)(SAMPLE_RATE * SILENCE_PAD_MS / 1000);
  if (endSample > totalSamples)
    endSample = totalSamples;

  const uint32_t dataBytes = (uint32_t)(endSample * 2);
  writeWavHeader(reqBuf + HEAD_LEN, dataBytes);
  memcpy(reqBuf, MULTIPART_HEAD, HEAD_LEN);
  memcpy(reqBuf + HEAD_LEN + WAV_HEADER_LEN + dataBytes, MULTIPART_TAIL, TAIL_LEN);
  return HEAD_LEN + WAV_HEADER_LEN + dataBytes + TAIL_LEN;
}

// ---------------------------------------------------------------------------
// OpenAI transcription: multipart POST to /v1/audio/transcriptions
// ---------------------------------------------------------------------------
int postMultipart(const uint8_t *body, size_t len, String &responseBody) {
  WiFiClientSecure client;
  client.setInsecure(); // Development only; pin a CA certificate for production.
  client.setTimeout(20000);

  HTTPClient http;
  http.setTimeout(30000);
  http.setReuse(false); // single request: send "Connection: close" (as before)
  if (!http.begin(client, OPENAI_HOST, OPENAI_PORT, TRANSCRIBE_PATH, true)) {
    Serial.println("TLS connection to OpenAI failed");
    return -1;
  }

  http.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + WAV_BOUNDARY);

  const int status = http.POST(const_cast<uint8_t *>(body), len);
  if (status == HTTP_CODE_OK)
    responseBody = http.getString();
  http.end();

  Serial.printf("--- OpenAI response (status %d, %u bytes) ---\n", status,
                (unsigned)responseBody.length());
  Serial.println(responseBody);
  return status;
}

String transcribeAudio(const uint8_t *body, size_t len) {
  String response;
  if (postMultipart(body, len, response) != HTTP_CODE_OK)
    return "";

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return "";
  }

  const char *text = doc["text"];
  if (text == nullptr) {
    Serial.println("No 'text' field in JSON");
    return "";
  }
  return String(text);
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
void showMessage(const String &msg) {
  screen.fillSprite(TFT_BLACK);
  screen.setTextColor(TFT_WHITE);
  screen.setTextSize(2);
  screen.setTextDatum(MC_DATUM);
  screen.drawString(msg, screen.width() / 2, screen.height() / 2);
  updateDisplay();
}

void drawWrapped(const String &text, int x, int y, int maxWidth, int lineHeight) {
  int curY = y;
  int len = text.length();
  int i = 0;
  while (i < len) {
    while (i < len && (text[i] == ' ' || text[i] == '\n'))
      i++; // skip leading ws
    if (i >= len)
      break;

    String line;
    while (i < len) {
      int j = i;
      while (j < len && text[j] != ' ' && text[j] != '\n')
        j++;
      String word = text.substring(i, j);
      String candidate = line.isEmpty() ? word : (line + " " + word);
      if (!line.isEmpty() && screen.textWidth(candidate) > maxWidth)
        break;
      line = candidate;
      i = j;
      if (i < len && text[i] == '\n') {
        i++;
        break;
      }
      while (i < len && text[i] == ' ')
        i++;
    }
    screen.drawString(line, x, curY);
    curY += lineHeight;
  }
}

void showTranscript(const String &text) {
  screen.fillSprite(TFT_BLACK);
  screen.setTextColor(TFT_WHITE);
  screen.setTextSize(2);
  screen.setTextDatum(TL_DATUM);
  drawWrapped(text, 10, 10, screen.width() - 20, 20);
  updateDisplay();
}

// ---------------------------------------------------------------------------
// Interaction flow
// ---------------------------------------------------------------------------
bool busy = false;

void runInteraction() {
  if (reqBuf == nullptr || pcmBufBytes == 0) {
    showMessage("Audio not ready");
    delay(1500);
    return;
  }

  showMessage("Listening...");
  const size_t bodyLen = recordSpeech();
  if (bodyLen == 0) {
    Serial.println("No speech detected");
    showMessage("No speech detected");
    delay(1200);
    return;
  }

  showMessage("Transcribing...");
  String text = transcribeAudio(reqBuf, bodyLen);
  if (text.isEmpty()) {
    Serial.println("Transcription failed");
    showMessage("Transcription failed");
    delay(1500);
    return;
  }

  text.trim();
  Serial.printf("Transcript: %s\n", text.c_str());
  showTranscript(text);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  initDisplay();
  initScreen();
  initTouch();

  initWiFi();

  if (!initAudio()) {
    showMessage("Audio not ready");
  } else if (WiFi.status() == WL_CONNECTED) {
    showMessage("Touch to speak");
  } else {
    showMessage("WiFi offline");
  }
}

void loop() {
  static bool prevTouched = false;
  bool nowTouched = touch.Get_Touch();
  bool pressed = nowTouched && !prevTouched;
  prevTouched = nowTouched;

  if (pressed && !busy) {
    busy = true;
    runInteraction();
    busy = false;
  }
  delay(5);
}
