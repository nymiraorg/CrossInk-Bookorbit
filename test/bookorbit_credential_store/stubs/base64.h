#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

// Minimal stub: the test only asserts that the password is NOT stored plaintext,
// so a fixed, non-plaintext marker is enough.
struct base64 {
  static String encode(const uint8_t* data, size_t len) {
    (void)data;
    (void)len;
    return String("obfuscated");
  }
};
