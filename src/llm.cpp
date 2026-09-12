#include "llm.h"

#include <ArduinoJson.h>

#include "app_config.h"
#include "openai_http.h"

String llmAnswer(const String &question) {
  // Build the payload with the JSON serializer so quotes, control characters
  // and non-ASCII text in the transcription are escaped correctly.
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
  if (err) {
    Serial.printf("LLM JSON parse error: %s\n", err.c_str());
    return "";
  }

  const char *answer = doc["choices"][0]["message"]["content"];
  if (answer == nullptr) {
    Serial.println("No 'choices[0].message.content' in the LLM response");
    return "";
  }
  return String(answer);
}
