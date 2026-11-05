#pragma once
#include <cstdint>
namespace ui::trails {
// Zero disables trails; 1 through 32 selects point budget per aircraft.
uint16_t points();
// Global 0..100% darkening of the newest trail compared with the icon.
// 0 means the same RGB332 color; older samples still fade by age.
uint8_t startDarkeningPercent();
// Global trail fading profile: Smooth S, Linear, Phosphor, Late Fade.
uint8_t fadeCurve();
bool saveFromPortal(const char* text);
bool saveStartDarkeningPercent(const char* text);
bool saveFadeCurve(const char* text);
}
