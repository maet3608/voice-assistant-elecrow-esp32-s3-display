// ---------------------------------------------------------------------------
// Elecrow DLE06235B 3.5" ESP32-S3 voice assistant
// Touch the screen to record a spoken phrase. The audio is transcribed with
// OpenAI and the result is displayed on screen.
//   - Display/touch initialization is unchanged from the original project.
//   - Audio is captured from the ES8311 codec microphone over I2S.
//   - Simple energy-based voice activity detection (VAD) ends the recording.
// ---------------------------------------------------------------------------

#include "ST77922.h"
#include "ST77922_Touch.h"
#include <Arduino.h>
#include <TFT_eSPI.h>

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <esp32-hal-psram.h>
#include <freertos/FreeRTOS.h>
#include <math.h>

#include "credentials.h"
#include "es8311.h"

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
// Audio capture configuration (ES8311 codec + I2S)
// ---------------------------------------------------------------------------
#define PIN_I2S_BCK 18
#define PIN_I2S_WS 21
#define PIN_I2S_DIN 16  // codec -> ESP32 (microphone data)
#define PIN_I2S_DOUT 15 // ESP32 -> codec (unused for capture)
#define PIN_I2S_MCK 17

#define SAMPLE_RATE 16000
#define MAX_RECORD_SECS 12
#define NO_SPEECH_TIMEOUT_MS 6000 // give up if nothing is spoken
#define SILENCE_TIMEOUT_MS 1300   // stop after this much trailing silence
#define SILENCE_PAD_MS 350        // keep a little audio after last speech
#define VAD_THRESHOLD 600.0f      // RMS of 16-bit samples counted as speech

#define TRANSCRIBE_MODEL "gpt-4o-transcribe"

// I2S is driven with the legacy API (driver/i2s.h); no channel handles needed.

// 44-byte WAV header + mono 16-bit PCM (PSRAM with internal-RAM fallback).
static uint8_t *wavBuf = nullptr;
static size_t pcmBufBytes = 0; // usable PCM bytes, determined at init
static const size_t MAX_PCM_BYTES = SAMPLE_RATE * 2 * MAX_RECORD_SECS;

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

// ---------------------------------------------------------------------------
// Audio init: I2S bus + ES8311 codec
// ---------------------------------------------------------------------------
void initAudio() {
  i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
      .mclk_multiple = I2S_MCLK_MULTIPLE_384,
  };

  i2s_pin_config_t pins = {
      .mck_io_num = PIN_I2S_MCK,
      .bck_io_num = PIN_I2S_BCK,
      .ws_io_num = PIN_I2S_WS,
      .data_out_num = PIN_I2S_DOUT,
      .data_in_num = PIN_I2S_DIN,
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("i2s_driver_install failed: %s\n", esp_err_to_name(err));
    return;
  }
  err = i2s_set_pin(I2S_NUM_1, &pins);
  if (err != ESP_OK) {
    Serial.printf("i2s_set_pin failed: %s\n", esp_err_to_name(err));
    return;
  }

  // Reuses I2C port 0 (GPIO38/39), already installed by the touch driver.
  if (es8311_codec_init() != ESP_OK) {
    Serial.println("ES8311 codec init failed");
    return;
  }

  Serial.printf("PSRAM: found=%d size=%d free=%d\n",
                psramFound(), ESP.getPsramSize(), ESP.getFreePsram());
  Serial.printf("Heap: free=%d maxAlloc=%d\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Prefer PSRAM, fall back to internal RAM, shrink if necessary.
  size_t want = 44 + MAX_PCM_BYTES;
  wavBuf = psramFound() ? (uint8_t *)ps_malloc(want) : nullptr;
  if (!wavBuf)
    wavBuf = (uint8_t *)malloc(want);
  while (!wavBuf && want > (44 + 8000)) { // down to ~0.25 s of audio
    want /= 2;
    wavBuf = psramFound() ? (uint8_t *)ps_malloc(want) : nullptr;
    if (!wavBuf)
      wavBuf = (uint8_t *)malloc(want);
  }
  if (!wavBuf) {
    Serial.println("Failed to allocate audio buffer");
    return;
  }
  pcmBufBytes = want - 44;
  Serial.printf("Audio buffer: %u bytes (~%u ms)\n", (unsigned)pcmBufBytes,
                (unsigned)(pcmBufBytes / 2 * 1000 / SAMPLE_RATE));
}

// ---------------------------------------------------------------------------
// One-shot audio diagnostic (prints I2S RX status + per-channel RMS)
// ---------------------------------------------------------------------------
void audioDiag() {
  int16_t chunk[512];
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_NUM_1, chunk, sizeof(chunk), &bytesRead, pdMS_TO_TICKS(200));
  int nFrames = (int)(bytesRead / 4);
  int64_t sumA = 0, sumB = 0;
  for (int i = 0; i < nFrames; i++) {
    int16_t a = chunk[i * 2];
    int16_t b = chunk[i * 2 + 1];
    sumA += (int64_t)a * a;
    sumB += (int64_t)b * b;
  }
  Serial.printf("[DIAG] i2s err=%d bytes=%d frames=%d\n", (int)err, (int)bytesRead, nFrames);
  if (nFrames > 0) {
    Serial.printf("[DIAG] chA RMS=%d  chB RMS=%d\n",
                  (int)sqrtf((float)sumA / nFrames), (int)sqrtf((float)sumB / nFrames));
    Serial.print("[DIAG] first samples:");
    for (int i = 0; i < 16; i++)
      Serial.printf(" %d", chunk[i]);
    Serial.println();
  }
}

// ---------------------------------------------------------------------------
// WAV header (44-byte PCM header)
// ---------------------------------------------------------------------------
void writeWavHeader(uint8_t *buf, uint32_t dataBytes) {
  const uint32_t sampleRate = SAMPLE_RATE;
  const uint16_t channels = 1;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
  const uint16_t blockAlign = channels * (bitsPerSample / 8);

  memcpy(buf + 0, "RIFF", 4);
  uint32_t riffSize = 36 + dataBytes;
  memcpy(buf + 4, &riffSize, 4);
  memcpy(buf + 8, "WAVE", 4);
  memcpy(buf + 12, "fmt ", 4);
  uint32_t fmtSize = 16;
  memcpy(buf + 16, &fmtSize, 4);
  uint16_t audioFormat = 1; // PCM
  memcpy(buf + 20, &audioFormat, 2);
  memcpy(buf + 22, &channels, 2);
  memcpy(buf + 24, &sampleRate, 4);
  memcpy(buf + 28, &byteRate, 4);
  memcpy(buf + 32, &blockAlign, 2);
  memcpy(buf + 34, &bitsPerSample, 2);
  memcpy(buf + 36, "data", 4);
  memcpy(buf + 40, &dataBytes, 4);
}

// ---------------------------------------------------------------------------
// Recording with simple energy-based voice activity detection (VAD).
// Captures mono 16-bit PCM from the I2S left channel into wavBuf (after the
// header) and returns the total WAV size (header + PCM), or 0 if no speech.
// ---------------------------------------------------------------------------
size_t recordSpeech() {
  if (!wavBuf || pcmBufBytes == 0)
    return 0;
  uint8_t *pcm = wavBuf + 44;
  int16_t chunk[512]; // 256 stereo frames
  size_t pcmBytes = 0;
  bool speech = false;
  size_t lastVoiceSample = 0;
  uint32_t startMs = millis();
  float peakRms = 0;

  // Flush stale DMA data before capturing.
  size_t flushed = 0;
  i2s_read(I2S_NUM_1, chunk, sizeof(chunk), &flushed, pdMS_TO_TICKS(20));

  while (true) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_1, chunk, sizeof(chunk), &bytesRead, pdMS_TO_TICKS(100));
    if (err == ESP_ERR_TIMEOUT) {
      bytesRead = 0; // treat a read timeout as a silent frame
    } else if (err != ESP_OK) {
      break;
    }

    int nFrames = (int)(bytesRead / 4); // 2 channels x 2 bytes
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
        pcm[pcmBytes++] = (uint8_t)(mono & 0xFF);
        pcm[pcmBytes++] = (uint8_t)((mono >> 8) & 0xFF);
      }
      sumSq += (int64_t)mono * mono;
    }

    float rms = (nFrames > 0) ? sqrtf((float)sumSq / (float)nFrames) : 0.0f;
    if (rms > peakRms)
      peakRms = rms;
    if (rms > VAD_THRESHOLD) {
      speech = true;
      lastVoiceSample = pcmBytes / 2;
    }

    uint32_t elapsed = millis() - startMs;
    size_t silenceSamples = (pcmBytes / 2) - lastVoiceSample;

    if (speech && (silenceSamples * 1000 / SAMPLE_RATE) >= SILENCE_TIMEOUT_MS)
      break;
    if (!speech && elapsed > NO_SPEECH_TIMEOUT_MS)
      break;
    if (elapsed > (uint32_t)(MAX_RECORD_SECS * 1000))
      break;
    if (pcmBytes >= pcmBufBytes)
      break;
  }

  Serial.printf("[DIAG] speech=%d peakRMS=%d samples=%u\n", (int)speech, (int)peakRms,
                (unsigned)(pcmBytes / 2));
  if (!speech)
    return 0;

  // Trim trailing silence, keeping a little padding after the last loud sample.
  size_t padSamples = (size_t)(SAMPLE_RATE * SILENCE_PAD_MS / 1000);
  size_t endSample = lastVoiceSample + padSamples;
  size_t totalSamples = pcmBytes / 2;
  if (endSample > totalSamples)
    endSample = totalSamples;

  uint32_t dataBytes = (uint32_t)(endSample * 2);
  writeWavHeader(wavBuf, dataBytes);
  return 44 + dataBytes;
}

// ---------------------------------------------------------------------------
// OpenAI transcription: multipart POST to /v1/audio/transcriptions
// ---------------------------------------------------------------------------
String transcribeAudio(const uint8_t *wav, size_t wavSize) {
  const char *host = "api.openai.com";
  const uint16_t port = 443;
  const String boundary = "----ESP32Boundary7MA4YWxk";

  WiFiClientSecure client;
  client.setInsecure(); // Development only; pin a CA certificate for production.
  client.setTimeout(20000);

  if (!client.connect(host, port)) {
    Serial.println("TLS connection to OpenAI failed");
    return "";
  }

  String head;
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  head += String(TRANSCRIBE_MODEL) + "\r\n";
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t contentLength = head.length() + wavSize + tail.length();

  client.print("POST /v1/audio/transcriptions HTTP/1.1\r\n");
  client.print("Host: api.openai.com\r\n");
  client.print("Authorization: Bearer ");
  client.print(OPENAI_API_KEY);
  client.print("\r\n");
  client.print("Content-Type: multipart/form-data; boundary=");
  client.print(boundary);
  client.print("\r\n");
  client.print("Content-Length: ");
  client.print((uint32_t)contentLength);
  client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(head);
  size_t written = 0;
  while (written < wavSize) {
    size_t n = client.write(wav + written, wavSize - written);
    if (n == 0)
      break;
    written += n;
  }
  client.print(tail);

  String response;
  unsigned long deadline = millis() + 30000;
  while (millis() < deadline) {
    while (client.available()) {
      char c = (char)client.read();
      if (response.length() < 8192)
        response += c;
    }
    if (!client.connected() && !client.available())
      break;
    delay(5);
  }
  client.stop();

  Serial.println("--- OpenAI response ---");
  Serial.println(response);
  return response;
}

// ---------------------------------------------------------------------------
// Robust JSON parsing: locate the JSON object (the response may be prefixed),
// then read the "text" field.
// ---------------------------------------------------------------------------
String extractTranscript(const String &response) {
  int jsonStart = response.indexOf('{');
  int jsonEnd = response.lastIndexOf('}');
  if (jsonStart < 0 || jsonEnd <= jsonStart) {
    Serial.println("No JSON object found in response");
    return "";
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response.substring(jsonStart, jsonEnd + 1));
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
  if (!wavBuf || pcmBufBytes == 0) {
    showMessage("Audio not ready");
    delay(1500);
    return;
  }

  showMessage("Listening...");
  size_t wavSize = recordSpeech();
  if (wavSize == 0) {
    Serial.println("No speech detected");
    showMessage("No speech detected");
    delay(1200);
    return;
  }

  showMessage("Transcribing...");
  String response = transcribeAudio(wavBuf, wavSize);
  String text = extractTranscript(response);
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
  initAudio();
  audioDiag();

  if (WiFi.status() == WL_CONNECTED) {
    showMessage("Touch to speak 1");
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
