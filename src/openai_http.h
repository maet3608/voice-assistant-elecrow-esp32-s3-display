#pragma once

#include <Arduino.h>
#include <Stream.h>

// Shared HTTPS transport for the OpenAI API. STT, LLM and TTS all post to
// api.openai.com with the same authorization header, so the connection
// handling lives in one place. Each helper returns the HTTP status code (or a
// negative HTTPClient error code).

constexpr int HTTP_OK_STATUS = 200;

// Provides the bearer token used by every request. The credential itself is
// owned by the caller (include/credentials.h).
void openaiHttpBegin(const char *apiKey);

// POSTs a fully assembled multipart/form-data body and reads the text response
// into `responseBody`. Used by the transcription upload.
int httpsPostMultipart(const char *path, const char *contentType,
                       const uint8_t *body, size_t len, String &responseBody);

// POSTs a JSON payload and reads the (text) response into `responseBody`.
int httpsPostJson(const char *path, const String &payload, String &responseBody);

// POSTs a JSON payload and streams the binary response into `sink`. Used for the
// text-to-speech download; handles both Content-Length and chunked responses.
int httpsPostToStream(const char *path, const String &payload, Stream &sink);
