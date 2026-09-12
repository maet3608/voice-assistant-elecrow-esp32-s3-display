#include "openai_http.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "app_config.h"

namespace {

const char *g_apiKey = nullptr;

// Creates the HTTPS client used for one request. TLS validation is disabled for
// development; pin a CA certificate before shipping.
bool beginRequest(HTTPClient &http, WiFiClientSecure &client, const char *path,
                  const char *contentType) {
  client.setInsecure();
  client.setTimeout(20000);

  http.setTimeout(30000);
  http.setReuse(false); // single request: send "Connection: close"

  if (!http.begin(client, OPENAI_HOST, OPENAI_PORT, path, true)) {
    Serial.printf("TLS connection to %s%s failed\n", OPENAI_HOST, path);
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + g_apiKey);
  http.addHeader("Content-Type", contentType);
  return true;
}

} // namespace

void openaiHttpBegin(const char *apiKey) {
  g_apiKey = apiKey;
}

int httpsPostMultipart(const char *path, const char *contentType,
                       const uint8_t *body, size_t len, String &responseBody) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, path, contentType))
    return HTTPC_ERROR_CONNECTION_REFUSED;

  const int status = http.POST(const_cast<uint8_t *>(body), len);
  if (status == HTTP_OK_STATUS)
    responseBody = http.getString();
  http.end();

  Serial.printf("--- POST %s -> status %d (%u bytes) ---\n", path, status,
                (unsigned)responseBody.length());
  if (status == HTTP_OK_STATUS)
    Serial.println(responseBody);
  return status;
}

int httpsPostJson(const char *path, const String &payload, String &responseBody) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, path, "application/json"))
    return HTTPC_ERROR_CONNECTION_REFUSED;

  const int status = http.POST((uint8_t *)payload.c_str(), payload.length());
  if (status == HTTP_OK_STATUS)
    responseBody = http.getString();
  http.end();

  Serial.printf("--- POST %s -> status %d (%u bytes) ---\n", path, status,
                (unsigned)responseBody.length());
  if (status == HTTP_OK_STATUS)
    Serial.println(responseBody);
  return status;
}

int httpsPostToStream(const char *path, const String &payload, Stream &sink) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, path, "application/json"))
    return HTTPC_ERROR_CONNECTION_REFUSED;

  const int status = http.POST((uint8_t *)payload.c_str(), payload.length());
  int written = -1;
  if (status == HTTP_OK_STATUS) {
    written = http.writeToStream(&sink);
    if (written < 0)
      Serial.printf("Binary download failed: %s\n",
                    HTTPClient::errorToString(written).c_str());
  } else {
    // Error responses are JSON, so they are safe to log.
    Serial.printf("--- POST %s -> status %d ---\n", path, status);
    Serial.println(http.getString());
  }
  http.end();

  Serial.printf("--- POST %s -> status %d, %d bytes downloaded ---\n", path, status, written);
  return status;
}
