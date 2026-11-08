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
// Dot markers are every Nth stored trail fix, never per-frame or interpolated.
uint8_t dotSpacing();             // 1..8, default 3
uint8_t dotDiameterHalfPx();      // 4..14: 2.0..7.0 px, default 9 (4.5 px)
bool saveFromPortal(const char* text);
bool saveStartDarkeningPercent(const char* text);
bool saveFadeCurve(const char* text);
bool saveDotSpacing(const char* text);
bool saveDotDiameterHalfPx(const char* text);
}
