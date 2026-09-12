// Elecrow DLE06235B 3.5" ESP32-S3 voice assistant.
//
// One voice turn per screen touch: STT -> LLM -> TTS, with the display
// following along. Settings live in app_config.h, shared audio I/O in
// audio_io.* and the HTTPS transport in openai_http.*.

#include <Arduino.h>
#include <WiFi.h>

#include "app_config.h"
#include "audio_io.h"
#include "credentials.h"
#include "display_ui.h"
#include "llm.h"
#include "openai_http.h"
#include "stt.h"
#include "tts.h"

namespace {

void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++)
    delay(500);

  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("WiFi connection FAILED");
}

void fail(const String &message, uint32_t holdMs) {
  Serial.println(message);
  uiMessage(message);
  delay(holdMs);
}

// One voice turn: STT -> LLM -> TTS, with the display following along.
void runInteraction() {
  // 1. STT - capture the phrase, then upload it for transcription.
  uiConversation("Listening...", "", "");
  if (sttRecord() == 0) {
    fail("No speech detected", 1200);
    return;
  }

  uiConversation("Transcribing...", "", "");
  String question = sttTranscribe();
  if (question.isEmpty()) {
    fail("Transcription failed", 1500);
    return;
  }
  question.trim();
  Serial.printf("Transcript: %s\n", question.c_str());

  // 2. LLM - answer the transcription.
  uiConversation("Thinking...", question, "");
  String answer = llmAnswer(question);
  if (answer.isEmpty()) {
    fail("No answer", 1500);
    return;
  }
  answer.trim();
  Serial.printf("Answer: %s\n", answer.c_str());

  // 3. TTS - speak the answer, with the display showing both turns.
  uiConversation("Speaking...", question, answer);
  ttsSpeak(answer);

  // 4. Keep the completed turn visible until the next activation.
  uiConversation("Touch to speak", question, answer);
}

} // namespace

void setup() {
  Serial.begin(115200);

  initDisplayUi();
  initWiFi();

  openaiHttpBegin(OPENAI_API_KEY);

  if (!initAudio() || !initStt() || !initTts()) {
    uiMessage("Audio not ready");
  } else if (WiFi.status() == WL_CONNECTED) {
    uiMessage("Touch to speak");
  } else {
    uiMessage("WiFi offline");
  }
}

void loop() {
  static bool prevTouched = false;
  const bool nowTouched = readTouch();
  const bool pressed = nowTouched && !prevTouched;
  prevTouched = nowTouched;

  if (pressed)
    runInteraction();
  delay(5);
}
