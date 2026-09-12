#include "openai_http.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "app_config.h"

namespace {

const char *g_apiKey = nullptr;

// Creates the HTTPS client used for one request. TLS validation is disabled for
// development
bool beginRequest(HTTPClient &http, WiFiClientSecure &client, const char *path,
                  const char *contentType) {
  client.setInsecure();
  client.setTimeout(20000);

  http.setTimeout(30000);
  http.setReuse(false); // single request: send "Connection: close"

  if (!http.begin(client, OPENAI_HOST, OPENAI_PORT, path, true))
    return false;

  http.addHeader("Authorization", String("Bearer ") + g_apiKey);
  http.addHeader("Content-Type", contentType);
  return true;
}

// POSTs `body` and reads the textual response. Shared by the multipart and JSON
// entry points below; only failures are logged.
int postAndRead(const char *path, const char *contentType, const uint8_t *body, size_t len,
                String &responseBody) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, path, contentType))
    return HTTPC_ERROR_CONNECTION_REFUSED;

  const int status = http.POST(const_cast<uint8_t *>(body), len);
  if (status == HTTP_OK_STATUS)
    responseBody = http.getString();
  http.end();

  if (status != HTTP_OK_STATUS)
    Serial.printf("POST %s failed (%d)\n", path, status);
  return status;
}

} // namespace

void openaiHttpBegin(const char *apiKey) {
  g_apiKey = apiKey;
}

int httpsPostMultipart(const char *path, const char *contentType,
                       const uint8_t *body, size_t len, String &responseBody) {
  return postAndRead(path, contentType, body, len, responseBody);
}

int httpsPostJson(const char *path, const String &payload, String &responseBody) {
  return postAndRead(path, "application/json", (const uint8_t *)payload.c_str(), payload.length(),
                     responseBody);
}

int httpsPostToStream(const char *path, const String &payload, Stream &sink) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, path, "application/json"))
    return HTTPC_ERROR_CONNECTION_REFUSED;

  const int status = http.POST((uint8_t *)payload.c_str(), payload.length());
  int written = -1;
  if (status == HTTP_OK_STATUS)
    written = http.writeToStream(&sink);
  http.end();

  if (status != HTTP_OK_STATUS)
    Serial.printf("POST %s failed (%d)\n", path, status);
  else if (written < 0)
    Serial.printf("POST %s download failed: %s\n", path,
                  HTTPClient::errorToString(written).c_str());
  return status;
}
