// ---------------------------------------------------------------------------
// Elecrow DLE06235B 3.5" ESP32-S3 voice assistant
//
// Touch the screen to run one complete voice turn:
//   1. STT     - record the spoken phrase and transcribe it.        (stt.*)
//   2. LLM     - ask the chat model to answer the transcription.    (llm.*)
//   3. TTS     - synthesize the answer and play it on the speaker.  (tts.*)
//   4. Display - show the transcription and the answer on screen.   (display_ui.*)
//
// Shared helpers: app_config.h (settings), audio_io.* (I2S + ES8311 codec),
// openai_http.* (HTTPS transport used by all three OpenAI calls).
// ---------------------------------------------------------------------------

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
  Serial.print("Connecting to WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\nWiFi connection FAILED");
}

bool busy = false;

void fail(const String &message, uint32_t holdMs) {
  Serial.println(message);
  uiMessage(message);
  delay(holdMs);
}

// ---------------------------------------------------------------------------
// One voice turn: STT -> LLM -> TTS, with the display following along.
// ---------------------------------------------------------------------------
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

  // 4. Display - keep the completed turn visible until the next activation.
  uiConversation("Touch to speak", question, answer);
}

} // namespace

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
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

  if (pressed && !busy) {
    busy = true;
    runInteraction();
    busy = false;
  }
  delay(5);
}
