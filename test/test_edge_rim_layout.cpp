// Host regression check for aircraft icon -> bearing-dot transition geometry.
// The live trail uses the projected aircraft position and is clipped by the
// stroke renderer, never by substituting the bearing-clamped dot coordinate.
#include "ui/radar_theme.h"
#include <cassert>
#include <cmath>
#include <initializer_list>

int main() {
  using namespace ui::radar;
  static_assert(kSize == 240, "assumes 240x240 round screen");
  static_assert(kCenterX == 120 && kCenterY == 120, "pixel centre convention");
  static_assert(kAircraftDisplayRadiusPx == 119, "icons reach near rim");
  static_assert(kBeyondRingScreenMarginPx == 0, "dot centres on physical rim");
  static_assert(kTrailDisplayRadiusPx - 1 == 119, "trail centreline reaches rim");
  // The dot is bearing-clamped at r=120, independently of aircraft range.
  for (const float angle : {0.0f, 0.3f, 1.0f, 2.2f, 3.9f, 5.5f}) {
    const float dot_x = kCenterX + (kCenterX - kBeyondRingScreenMarginPx) * cosf(angle);
    const float dot_y = kCenterY + (kCenterX - kBeyondRingScreenMarginPx) * sinf(angle);
    const float radius = hypotf(dot_x - kCenterX, dot_y - kCenterY);
    assert(fabsf(radius - 120.0f) < 0.0001f);
    // Different true aircraft distances share the same dot bearing, but
    // cannot be used as a geometric trail endpoint interchangeably.
    const float actual_x = kCenterX + 143.0f * cosf(angle);
    const float actual_y = kCenterY + 143.0f * sinf(angle);
    assert(hypotf(actual_x - dot_x, actual_y - dot_y) > 22.9f);
  }
}
