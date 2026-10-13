#pragma once

#include <PersistableStore.h>

#include <string>

/**
 * Native BookOrbit request credentials (the v3 "Request a book" path).
 *
 * The request/metadata API (/api/v1/*) authenticates with BookOrbit's normal
 * JWT session, NOT the KOReader credentials the sync path uses. A native login
 * (clientKind "native") returns a short-lived access token plus a rotating
 * refresh token. This store persists the refresh token (obfuscated, tied to
 * this device) and keeps the access token in RAM only, so a reboot drops the
 * short-lived token and re-derives it from the persisted refresh token.
 *
 * The refresh token is a long-lived credential equivalent to a password for the
 * least-privilege requester account, so callers must only persist it after a
 * verified-TLS login (see BookOrbitRequestClient::loginNative).
 */
class BookOrbitRequestCredentialStore : public PersistableStore<BookOrbitRequestCredentialStore> {
 private:
  std::string username;      // the least-privilege requester account
  std::string refreshToken;  // obfuscated on SD; rotated on every refresh
  std::string serverUrl;     // base server, normalized by getBaseUrl()
  std::string accessToken;   // RAM-only; never serialized to disk

  BookOrbitRequestCredentialStore() = default;
  ~BookOrbitRequestCredentialStore() = default;

  friend class PersistableStore<BookOrbitRequestCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/bookorbit-request.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Persisted session identity (after a successful native login or refresh).
  void setSession(const std::string& user, const std::string& refresh);
  // Persist a rotated refresh token atomically (BookOrbit rotates on refresh).
  void rotateRefreshToken(const std::string& refresh);

  // RAM-only access token.
  void setAccessToken(const std::string& token);
  void clearAccessToken();
  const std::string& getAccessToken() const;
  bool hasAccessToken() const;

  const std::string& getUsername() const {
    ensureLoaded();
    return username;
  }
  const std::string& getRefreshToken() const {
    ensureLoaded();
    return refreshToken;
  }
  bool hasRefreshToken() const {
    ensureLoaded();
    return !refreshToken.empty();
  }

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const {
    ensureLoaded();
    return serverUrl;
  }
  /** Server URL normalized + the /api/v1 request/metadata API prefix. */
  std::string getBaseUrl() const;

  void clearSession();
};

#define BOOKORBIT_REQUEST_STORE BookOrbitRequestCredentialStore::getInstance()
