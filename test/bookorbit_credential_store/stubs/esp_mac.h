#pragma once

#include <cstdint>

// Minimal stub: a fixed, deterministic eFuse MAC so obfuscation is reproducible.
inline int esp_efuse_mac_get_default(uint8_t* mac) {
  for (int i = 0; i < 6; i++) mac[i] = static_cast<uint8_t>(i + 1);
  return 0;
}
