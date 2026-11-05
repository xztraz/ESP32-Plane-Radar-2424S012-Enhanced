#pragma once
#include <Arduino.h>
#include <cstdint>
#include "services/aircraft_palette.h"
namespace services::adsb { struct Aircraft; }
namespace services::aircraft_styles {
void init();
String encoded();
bool save(const char* code);
services::aircraft_palette::Selection defaultSelection(const services::adsb::Aircraft& plane);
uint32_t defaultRgb(const services::adsb::Aircraft& plane);
bool militaryPriority();
bool helicopterPriority();
}
