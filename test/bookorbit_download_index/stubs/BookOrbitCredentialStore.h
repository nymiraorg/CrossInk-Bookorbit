#pragma once

#include <string>

// Minimal stub: BookOrbitDownloadIndex only reads getServerUrl() (for the index's
// server-URL CRC scope). Kept tiny so the test need not compile the real store.
class BookOrbitCredentialStore {
 public:
  static BookOrbitCredentialStore& getInstance() {
    static BookOrbitCredentialStore instance;
    return instance;
  }
  void setServerUrl(const std::string& url) { serverUrl = url; }
  const std::string& getServerUrl() const { return serverUrl; }

 private:
  std::string serverUrl = "https://example.com";
};

#define BOOKORBIT_STORE BookOrbitCredentialStore::getInstance()
