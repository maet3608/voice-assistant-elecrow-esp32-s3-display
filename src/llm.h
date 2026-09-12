#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Large language model: answer the transcription.
// ---------------------------------------------------------------------------

// Sends the user's question to the chat model and returns the assistant reply
// ("" when the request or the response was unusable).
String llmAnswer(const String &question);
