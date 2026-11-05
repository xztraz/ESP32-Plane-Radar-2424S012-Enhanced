#pragma once
#include <Arduino.h>
#include <cstdint>
namespace ui::display_options {
enum Field : uint8_t { kCallsign=1, kRegistration=2, kType=4, kAltitude=8 };
void init();
String encoded();  // field-mask,trail-dots,icon-size-% (e.g. 15,1,100)
bool save(const char* value);
bool show(Field field);
bool trailDots();
uint8_t labelBrightnessPct(); // 20..100% in 10% steps (100 default)
uint16_t iconScalePct();
float iconScale();
}
