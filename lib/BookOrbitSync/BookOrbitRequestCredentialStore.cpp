#include "BookOrbitRequestCredentialStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
// The request/metadata API lives under /api/v1 (not /api/v1/koreader, which is
// the KOReader sync surface the existing store targets).
constexpr char API_PREFIX[] = "/api/v1";
}  // namespace

void BookOrbitRequestCredentialStore::toJson(JsonDocument& doc) const {
  doc["username"] = username;
  doc["refresh_obf"] = obfuscation::obfuscateToBase64(refreshToken);
  doc["serverUrl"] = serverUrl;
  // accessToken is deliberately absent: RAM-only.
}

bool BookOrbitRequestCredentialStore::fromJson(JsonVariantConst doc) {
  username = doc["username"] | "";

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string refresh = obfuscation::deobfuscateFromBase64(doc["refresh_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::INVALID && !refresh.empty()) {
    LOG_ERR("BOR", "Ignoring unreadable BookOrbit refresh token");
    refresh.clear();
  }
  refreshToken = refresh;
  serverUrl = doc["serverUrl"] | "";
  return true;
}

void BookOrbitRequestCredentialStore::setSession(const std::string& user, const std::string& refresh) {
  ensureLoaded();
  username = user;
  refreshToken = refresh;
  saveToFile();
  LOG_DBG("BOR", "Stored request session for user: %s", user.c_str());
}

void BookOrbitRequestCredentialStore::rotateRefreshToken(const std::string& refresh) {
  ensureLoaded();
  refreshToken = refresh;
  saveToFile();
  LOG_DBG("BOR", "Rotated request refresh token");
}

void BookOrbitRequestCredentialStore::setAccessToken(const std::string& token) {
  ensureLoaded();
  accessToken = token;
}

void BookOrbitRequestCredentialStore::clearAccessToken() {
  ensureLoaded();
  accessToken.clear();
}

const std::string& BookOrbitRequestCredentialStore::getAccessToken() const {
  ensureLoaded();
  return accessToken;
}

bool BookOrbitRequestCredentialStore::hasAccessToken() const {
  ensureLoaded();
  return !accessToken.empty();
}

void BookOrbitRequestCredentialStore::setServerUrl(const std::string& url) {
  ensureLoaded();
  serverUrl = url;
  saveToFile();
}

std::string BookOrbitRequestCredentialStore::getBaseUrl() const {
  ensureLoaded();
  if (serverUrl.empty()) {
    return "";
  }

  std::string url;
  if (serverUrl.find("://") == std::string::npos) {
    // BookOrbit is self-hosted and normally behind TLS; unlike the sync store
    // we do not silently assume a scheme, but normalize to https when none is
    // given (the LAN proof uses an explicit http:// URL).
    url = "https://" + serverUrl;
  } else {
    url = serverUrl;
  }

  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  // Strip a trailing "/api/v1" the user may have pasted from BookOrbit's own
  // settings page, then append the canonical request prefix.
  constexpr char API_V1_SUFFIX[] = "/api/v1";
  if (url.size() >= sizeof(API_V1_SUFFIX) - 1 &&
      url.compare(url.size() - (sizeof(API_V1_SUFFIX) - 1), sizeof(API_V1_SUFFIX) - 1, API_V1_SUFFIX) == 0) {
    url.resize(url.size() - (sizeof(API_V1_SUFFIX) - 1));
  }

  return url + API_PREFIX;
}

void BookOrbitRequestCredentialStore::clearSession() {
  ensureLoaded();
  username.clear();
  refreshToken.clear();
  accessToken.clear();
  saveToFile();
  LOG_DBG("BOR", "Cleared request session");
}
