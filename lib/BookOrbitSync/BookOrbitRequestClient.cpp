#include "BookOrbitRequestClient.h"

#include <Logging.h>
#include <SecureHttpClient.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <utility>

#include "BookOrbitRequestCredentialStore.h"

namespace {

constexpr uint32_t HTTP_TIMEOUT_MS = 30000;
constexpr char DEVICE_LABEL[] = "CrossInk X4 Pro";

// BookOrbit native login/refresh payloads.
constexpr char LOGIN_PATH[] = "/auth/login";
constexpr char REFRESH_PATH[] = "/auth/refresh";
constexpr char METADATA_PATH[] = "/metadata-fetch/stream";
constexpr char BOOK_REQUESTS_PATH[] = "/book-requests";

// Builds a full URL from the store's base (server + /api/v1) and a path.
std::string buildUrl(const std::string& path) {
  return BOOKORBIT_REQUEST_STORE.getBaseUrl() + path;
}

// Minimal RFC 3986 percent-encoding for query parameters.
std::string urlEncode(const std::string& value) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (const unsigned char c : value) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Configure a SecureHttpClient for the request API. For https we require a CA
// root (verified TLS + hostname check, see SecureClient); for http the plain
// WiFiClient transport is used. We never call setInsecure() here.
bool configureClient(freeink::SecureHttpClient& http) {
  const std::string base = BOOKORBIT_REQUEST_STORE.getBaseUrl();
  if (base.rfind("https://", 0) == 0) {
    const char* ca = BookOrbitRequestClient::getCaCert();
    if (ca == nullptr) {
      LOG_ERR("BOR", "https request but no CA root is compiled in; refusing to proceed insecurely");
      return false;
    }
    http.setCACert(ca);
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent("CrossInk");
  return true;
}

// Parses a credentials response ({accessToken, refreshToken, ...}) and stores
// it. Returns false when the JSON is missing the required fields.
bool storeCredentials(const JsonDocument& doc, const std::string& username) {
  // .as<const char*>() — NOT `| nullptr`: ArduinoJson v7 resolves `| nullptr`
  // to the std::nullptr_t overload, which returns null whether or not the key
  // is a string, so `| nullptr` can never read a present value.
  const char* access = doc["accessToken"].as<const char*>();
  const char* refresh = doc["refreshToken"].as<const char*>();
  if (access == nullptr || refresh == nullptr) {
    return false;
  }
  BOOKORBIT_REQUEST_STORE.setSession(username, refresh);
  BOOKORBIT_REQUEST_STORE.setAccessToken(access);
  return true;
}

BookOrbitRequestClient::Error mapHttpCode(int code) {
  if (code < 0) return BookOrbitRequestClient::NETWORK_ERROR;
  if (code == 401) return BookOrbitRequestClient::AUTH_FAILED;
  if (code == 404) return BookOrbitRequestClient::NOT_FOUND;
  if (code >= 500) return BookOrbitRequestClient::SERVER_ERROR;
  return BookOrbitRequestClient::OK;
}

}  // namespace

namespace {
// Populated on every native-login failure so the activity can show the reason on
// the e-ink screen (there is no serial access in the field).
std::string s_lastLoginDiagnostic;
}  // namespace

const std::string& BookOrbitRequestClient::lastLoginDiagnostic() {
  return s_lastLoginDiagnostic;
}

const char* BookOrbitRequestClient::getCaCert() {
  // No CA root is compiled in yet: the v3 proof runs against the LAN BookOrbit
  // over plain http. When BookOrbit moves to https (off-LAN), embed the
  // books.sparkwright.io chain here. Returning nullptr makes configureClient()
  // refuse https rather than fall back to setInsecure().
  return nullptr;
}

BookOrbitRequestClient::Error BookOrbitRequestClient::loginNative(const std::string& username,
                                                                 const std::string& password) {
  s_lastLoginDiagnostic.clear();
  freeink::SecureHttpClient http;
  if (!configureClient(http)) return NETWORK_ERROR;
  if (!http.begin(buildUrl(LOGIN_PATH))) return NETWORK_ERROR;
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["username"] = username;
  body["password"] = password;
  body["clientKind"] = "native";
  body["deviceLabel"] = DEVICE_LABEL;
  std::string payload;
  serializeJson(body, payload);

  const int code = http.POST(payload);
  const Error err = mapHttpCode(code);
  if (err != OK) {
    LOG_ERR("BOR", "native login failed: http=%d heap=%u", code, (unsigned)ESP.getFreeHeap());
    s_lastLoginDiagnostic.clear();
    return err == AUTH_FAILED ? AUTH_FAILED : err;
  }

  // Pass the captured body as a C-string, exactly like the working sync client
  // (which parses outBody.c_str()); the std::string reader path is equivalent on
  // the host but this keeps the two clients on one proven path on the device.
  JsonDocument resp;
  const std::string& bodyStr = http.getString();
  const DeserializationError derr = deserializeJson(resp, bodyStr.c_str());
  if (derr != DeserializationError::Ok) {
    char diag[96];
    snprintf(diag, sizeof(diag), "%s http=%d heap=%u body=%u", derr.c_str(), code, (unsigned)ESP.getFreeHeap(),
             (unsigned)bodyStr.size());
    s_lastLoginDiagnostic = diag;
    LOG_ERR("BOR", "native login parse failed: %s", diag);
    return JSON_ERROR;
  }
  if (!storeCredentials(resp, username)) {
    char diag[96];
    snprintf(diag, sizeof(diag), "missing token http=%d heap=%u body=%u", code, (unsigned)ESP.getFreeHeap(),
             (unsigned)bodyStr.size());
    s_lastLoginDiagnostic = diag;
    LOG_ERR("BOR", "native login: %s", diag);
    return JSON_ERROR;
  }
  LOG_DBG("BOR", "native login ok for %s", username.c_str());
  return OK;
}

BookOrbitRequestClient::Error BookOrbitRequestClient::refreshSession() {
  if (!BOOKORBIT_REQUEST_STORE.hasRefreshToken()) return NO_CREDENTIALS;

  freeink::SecureHttpClient http;
  if (!configureClient(http)) return NETWORK_ERROR;
  if (!http.begin(buildUrl(REFRESH_PATH))) return NETWORK_ERROR;
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["refreshToken"] = BOOKORBIT_REQUEST_STORE.getRefreshToken();
  std::string payload;
  serializeJson(body, payload);

  const int code = http.POST(payload);
  const Error err = mapHttpCode(code);
  if (err != OK) {
    LOG_ERR("BOR", "refresh failed: %d", code);
    return err;
  }

  JsonDocument resp;
  if (deserializeJson(resp, http.getString().c_str()) != DeserializationError::Ok) return JSON_ERROR;
  const char* access = resp["accessToken"].as<const char*>();
  const char* refresh = resp["refreshToken"].as<const char*>();
  if (access == nullptr || refresh == nullptr) return JSON_ERROR;

  // Atomically persist the rotated refresh token, then cache the access token.
  BOOKORBIT_REQUEST_STORE.rotateRefreshToken(refresh);
  BOOKORBIT_REQUEST_STORE.setAccessToken(access);
  LOG_DBG("BOR", "session refreshed");
  return OK;
}

BookOrbitRequestClient::Error BookOrbitRequestClient::request(const std::string& path,
                                                             const std::string& body,
                                                             JsonDocument& outDoc) {
  if (!BOOKORBIT_REQUEST_STORE.hasAccessToken()) {
    const Error err = refreshSession();
    if (err != OK) return err;
  }

  // One retry after a refresh, so a stale access token recovers transparently.
  for (int attempt = 0; attempt < 2; ++attempt) {
    freeink::SecureHttpClient http;
    if (!configureClient(http)) return NETWORK_ERROR;
    if (!http.begin(buildUrl(path))) return NETWORK_ERROR;
    http.addHeader("Accept", "application/json");
    const std::string auth = "Bearer " + BOOKORBIT_REQUEST_STORE.getAccessToken();
    http.addHeader("Authorization", auth);
    if (!body.empty()) http.addHeader("Content-Type", "application/json");

    const int code = body.empty() ? http.GET() : http.POST(body);
    if (code == 401) {
      // Session expired mid-flight: refresh once and retry.
      const Error err = refreshSession();
      if (err != OK) return err;
      continue;
    }

    const Error err = mapHttpCode(code);
    if (err != OK) return err;
    if (deserializeJson(outDoc, http.getString().c_str()) != DeserializationError::Ok) return JSON_ERROR;
    return OK;
  }
  return AUTH_FAILED;
}

BookOrbitRequestClient::Error BookOrbitRequestClient::searchMetadata(const std::string& title,
                                                                    const std::string& author,
                                                                    const CandidateCallback& onCandidate) {
  if (!BOOKORBIT_REQUEST_STORE.hasAccessToken()) {
    const Error err = refreshSession();
    if (err != OK) return err;
  }

  freeink::SecureHttpClient http;
  if (!configureClient(http)) return NETWORK_ERROR;

  std::string query = "?mediaKind=ebook&title=" + urlEncode(title);
  if (!author.empty()) query += "&author=" + urlEncode(author);
  if (!http.begin(buildUrl(METADATA_PATH) + query)) return NETWORK_ERROR;
  http.addHeader("Accept", "text/event-stream");
  const std::string auth = "Bearer " + BOOKORBIT_REQUEST_STORE.getAccessToken();
  http.addHeader("Authorization", auth);

  // Incremental SSE: accumulate bytes until a blank line, then parse the event.
  // `data:` lines carry a candidate JSON object; `provider-status` events and
  // the end of the stream carry no candidate and are ignored.
  std::string buffer;
  bool streamOk = true;
  const int code = http.GET(
      [&](const uint8_t* data, size_t len) {
        buffer.append(reinterpret_cast<const char*>(data), len);
        size_t pos;
        while ((pos = buffer.find("\n\n")) != std::string::npos) {
          const std::string event = buffer.substr(0, pos);
          buffer.erase(0, pos + 2);

          const size_t dataPos = event.find("data:");
          if (dataPos == std::string::npos) continue;
          const std::string payload = event.substr(dataPos + 5);
          // A candidate JSON object vs. a provider-status/"done" envelope: only
          // try to parse and hand back what looks like a JSON object.
          const size_t brace = payload.find('{');
          if (brace == std::string::npos) continue;

          JsonDocument candidate;
          const DeserializationError err = deserializeJson(candidate, payload.substr(brace));
          if (err != DeserializationError::Ok) continue;
          if (!onCandidate(candidate)) {
            streamOk = false;
            return false;  // stop streaming
          }
        }
        return true;
      },
      nullptr);

  if (code < 0) return NETWORK_ERROR;
  if (code == 401) {
    // Session expired: refresh and let the caller retry the search.
    return refreshSession();
  }
  const Error err = mapHttpCode(code);
  if (err != OK) return err;
  return streamOk ? OK : OK;
}

BookOrbitRequestClient::Error BookOrbitRequestClient::submitRequest(const JsonDocument& payload,
                                                                   int32_t& outRequestId) {
  std::string body;
  serializeJson(payload, body);

  JsonDocument resp;
  const Error err = request(BOOK_REQUESTS_PATH, body, resp);
  if (err != OK) return err;

  // The create endpoint returns { request: { id, ... } }.
  outRequestId = resp["request"]["id"] | 0;
  return outRequestId > 0 ? OK : JSON_ERROR;
}

BookOrbitRequestClient::Error BookOrbitRequestClient::fetchMyRequests(JsonDocument& outDoc) {
  return request(BOOK_REQUESTS_PATH, "", outDoc);
}
