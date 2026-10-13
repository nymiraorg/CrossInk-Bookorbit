#include <ArduinoJson.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>

#include "BookOrbitCredentialStore.h"

// Covers the Automatic Local Book Delivery additions to the credential store:
// autoDeliveryOnSleep / deliveryScopeId / deliveryScopeName round-trip through
// toJson and fromJson, and — matching the brief's security note — only the password
// is obfuscated; every other field (including the scope id/name) is plaintext.
//
// The store persists through an in-memory Storage stub (see stubs/HalStorage.h),
// so this runs on the host with no SD card.

namespace {

// A bookorbit.json from before the delivery feature: no auto-delivery fields.
constexpr const char* OLD_JSON =
    R"({"username":"alice","password_obf":"x","serverUrl":"https://x","downloadFolder":"/Read","syncBehavior":0,"syncOnSleep":false})";

// A current bookorbit.json carrying the delivery fields.
constexpr const char* FULL_JSON =
    R"({"username":"alice","password_obf":"x","serverUrl":"https://x","downloadFolder":"/Read","syncBehavior":0,"syncOnSleep":true,"autoDeliveryOnSleep":true,"deliveryScopeId":"42","deliveryScopeName":"Send to X4"})";

JsonDocument saveAndRead() {
  Storage.reset();
  auto& store = BookOrbitCredentialStore::getInstance();
  store.setCredentials("alice", "s3cret");
  store.setServerUrl("https://books.example.com");
  store.setDownloadFolder("/Read");
  store.setSyncOnSleep(true);
  store.setAutoDeliveryOnSleep(true);
  store.setDeliveryScopeId("42");
  store.setDeliveryScopeName("Send to X4");
  EXPECT_TRUE(store.saveToFile());

  JsonDocument doc;
  EXPECT_TRUE(PersistableStoreBase::readDocFromFile(BookOrbitCredentialStore::getFilePath(), doc));
  return doc;
}

TEST(BookOrbitCredentialStore, ToJsonWritesDeliveryFields) {
  JsonDocument doc = saveAndRead();

  EXPECT_TRUE(doc["autoDeliveryOnSleep"].as<bool>());
  EXPECT_STREQ(doc["deliveryScopeId"] | "", "42");
  EXPECT_STREQ(doc["deliveryScopeName"] | "", "Send to X4");

  // Pre-existing fields still serialize alongside the new ones.
  EXPECT_STREQ(doc["username"] | "", "alice");
  EXPECT_STREQ(doc["serverUrl"] | "", "https://books.example.com");
  EXPECT_STREQ(doc["downloadFolder"] | "", "/Read");
  EXPECT_TRUE(doc["syncOnSleep"].as<bool>());
}

TEST(BookOrbitCredentialStore, ToJsonOnlyPasswordIsObfuscated) {
  JsonDocument doc = saveAndRead();

  // The plaintext password is never written; only password_obf is.
  EXPECT_TRUE(doc["password"].isNull());
  EXPECT_TRUE(doc["password_obf"].is<const char*>());
  EXPECT_STRNE(doc["password_obf"] | "", "s3cret");

  // Scope id/name are not secrets and are stored plaintext, like serverUrl.
  EXPECT_STREQ(doc["deliveryScopeId"] | "", "42");
  EXPECT_STREQ(doc["deliveryScopeName"] | "", "Send to X4");
}

TEST(BookOrbitCredentialStore, FromJsonDefaultsMissingFields) {
  Storage.reset();
  auto& store = BookOrbitCredentialStore::getInstance();
  Storage.put(BookOrbitCredentialStore::getFilePath(), OLD_JSON);
  ASSERT_TRUE(store.loadFromFile());

  EXPECT_FALSE(store.getAutoDeliveryOnSleep());
  EXPECT_TRUE(store.getDeliveryScopeId().empty());
  EXPECT_TRUE(store.getDeliveryScopeName().empty());
}

TEST(BookOrbitCredentialStore, FromJsonReadsDeliveryFields) {
  Storage.reset();
  auto& store = BookOrbitCredentialStore::getInstance();
  Storage.put(BookOrbitCredentialStore::getFilePath(), FULL_JSON);
  ASSERT_TRUE(store.loadFromFile());

  EXPECT_TRUE(store.getAutoDeliveryOnSleep());
  EXPECT_EQ(store.getDeliveryScopeId(), "42");
  EXPECT_EQ(store.getDeliveryScopeName(), "Send to X4");
}

}  // namespace
