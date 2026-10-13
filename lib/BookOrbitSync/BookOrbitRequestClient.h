#pragma once

#include <ArduinoJson.h>

#include <functional>
#include <string>

/**
 * HTTP client for BookOrbit's native request/metadata API (/api/v1/*).
 *
 * Unlike BookOrbitSyncClient (KOReader credentials, x-auth-user/x-auth-key),
 * this client speaks BookOrbit's normal JWT session: it logs in natively,
 * holds a Bearer access token, and refreshes it from a persisted rotating
 * refresh token. The token pair lives in BookOrbitRequestCredentialStore.
 *
 * Base URL: BookOrbitRequestCredentialStore::getBaseUrl(), e.g.
 *   https://books.example.com/api/v1
 *
 * Endpoints used:
 *   POST /auth/login        — native login (clientKind "native")
 *   POST /auth/refresh      — rotate the refresh token, get a fresh access token
 *   GET  /metadata-fetch/stream — metadata search (SSE), see searchMetadata
 *   POST /book-requests     — submit a request
 *   GET  /book-requests     — list the signed-in user's requests ("My requests")
 */
class BookOrbitRequestClient {
 public:
  enum Error : int8_t {
    OK = 0,
    NO_CREDENTIALS,  // no persisted refresh token and no stored access token
    NETWORK_ERROR,   // transport / timeout
    AUTH_FAILED,     // login/refresh rejected (401)
    SERVER_ERROR,    // 5xx
    JSON_ERROR,      // response could not be parsed
    NOT_FOUND,       // 404
  };

  /**
   * Native login. Persists the refresh token and caches the access token.
   * The refresh token must only be persisted over a verified connection; the
   * caller is responsible for ensuring the server URL is TLS-verified before
   * calling this with a credential that matters.
   */
  static Error loginNative(const std::string& username, const std::string& password);

  /** Refresh the access token from the persisted refresh token (rotating it). */
  static Error refreshSession();

  /**
   * Authenticated request against the base URL. An empty body is a GET; a
   * non-empty body is a POST with a JSON payload. On a 401, refreshes the
   * session once and retries. The response body is parsed into outDoc.
   */
  static Error request(const std::string& path, const std::string& body, JsonDocument& outDoc);

  /**
   * Incremental metadata search over the SSE stream (GET /metadata-fetch/stream).
   * onCandidate is called once per candidate JSON object as it arrives; return
   * false to stop the stream early. Returns OK on a clean end-of-stream.
   */
  using CandidateCallback = std::function<bool(const JsonDocument& candidate)>;
  static Error searchMetadata(const std::string& title, const std::string& author, const CandidateCallback& onCandidate);

  /** Submit a book request (POST /book-requests). Returns OK and the request id in outRequestId. */
  static Error submitRequest(const JsonDocument& payload, int32_t& outRequestId);

  /** List the signed-in user's requests (GET /book-requests). */
  static Error fetchMyRequests(JsonDocument& outDoc);

  // The CA root PEM for verified TLS, or nullptr when none is compiled in
  // (https requests then refuse rather than fall back to setInsecure()).
  static const char* getCaCert();

  // Human-readable detail about the last native-login failure, for surfacing on
  // the reader's screen: the JSON parse error, the HTTP status, the free heap at
  // failure, and the captured response body length (0 == empty body). Cleared on
  // the next login attempt.
  static const std::string& lastLoginDiagnostic();
};
