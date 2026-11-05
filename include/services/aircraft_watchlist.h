#pragma once
#include <cstddef>
#include <cstdint>
#include <Arduino.h>
namespace services::adsb { struct Aircraft; }
namespace services::watchlist {
constexpr size_t kMaxWatched = 50;
void init();
// Color token: RRGGBBHHYY (RGB332 color plus hue/shade); old RRGGBB accepted.
// Entries: R:REG*|RRGGBB|NAME|P ; C:CALL*|RRGGBB|NAME|P
// Legacy saved H:hex rules are ignored on migration.
// P is 0 or 1. The legacy format without |P remains supported.
bool save(const char* encoded);
String encoded();
bool find(const services::adsb::Aircraft& aircraft, uint32_t* rgb,
          char* name, size_t name_len, bool* priority = nullptr, uint8_t* hue = nullptr,
          uint8_t* shade = nullptr);
bool priority(const services::adsb::Aircraft& aircraft);
}
