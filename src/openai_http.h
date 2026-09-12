#pragma once

#include <Arduino.h>
#include <Stream.h>

// ---------------------------------------------------------------------------
// Shared HTTPS transport for the OpenAI API.
//
// STT, LLM and TTS all post to api.openai.com with the same authorization
// header, so the connection handling lives in one place. Each helper returns
// the HTTP status code (or a negative HTTPClient error code).
// ---------------------------------------------------------------------------

// HTTP status returned by a successful request.
constexpr int HTTP_OK_STATUS = 200;

// Provides the bearer token used by every request. The credential itself is
// owned by the caller (include/credentials.h) so this module never has to
// include the provisioning header.
void openaiHttpBegin(const char *apiKey);

// POST a fully assembled multipart/form-data body and read the text response
// into `responseBody`. Used by the transcription upload.
int httpsPostMultipart(const char *path, const char *contentType,
                       const uint8_t *body, size_t len, String &responseBody);

// POST a JSON payload and read the (text) response into `responseBody`.
int httpsPostJson(const char *path, const String &payload, String &responseBody);

// POST a JSON payload and stream the binary response into `sink`. Used for the
// text-to-speech download; handles both Content-Length and chunked responses.
int httpsPostToStream(const char *path, const String &payload, Stream &sink);
