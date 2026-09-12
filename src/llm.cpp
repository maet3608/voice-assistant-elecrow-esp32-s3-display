#include "llm.h"

#include <ArduinoJson.h>

#include "app_config.h"
#include "openai_http.h"

String llmAnswer(const String &question) {
  // Serialize rather than hand-build the JSON, so quotes and non-ASCII text in
  // the transcription are escaped correctly.
  JsonDocument request;
  request["model"] = LLM_MODEL;
  request["max_tokens"] = LLM_MAX_TOKENS;
  request["temperature"] = LLM_TEMPERATURE;

  JsonArray messages = request["messages"].to<JsonArray>();
  JsonObject system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] = LLM_SYSTEM_PROMPT;
  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = question;

  String payload;
  serializeJson(request, payload);

  String response;
  if (httpsPostJson(LLM_PATH, payload, response) != HTTP_OK_STATUS)
    return "";

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, response);
  const char *answer =
      err ? nullptr : doc["choices"][0]["message"]["content"].as<const char *>();
  if (answer == nullptr) {
    Serial.printf("LLM response unusable (%s)\n",
                  err ? err.c_str() : "no choices[0].message.content");
    return "";
  }
  return String(answer);
}
