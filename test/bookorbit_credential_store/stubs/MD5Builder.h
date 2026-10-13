#pragma once

#include <Arduino.h>

// Minimal stub: the credential-store round-trip test never calls getMd5Password(),
// so MD5Builder only needs to compile.
class MD5Builder {
 public:
  void begin() {}
  void add(const char*) {}
  void add(uint8_t) {}
  void calculate() {}
  String toString() { return String("md5"); }
};
