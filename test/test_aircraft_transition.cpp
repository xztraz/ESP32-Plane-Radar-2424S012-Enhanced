#include "ui/aircraft_transition.h"
#include "ui/trail_fade.h"
#include <cassert>
#include <cstdio>
using namespace ui::aircraft_transition;
int main() {
  assert(intensity(true, 0, 0) == 0);
  assert(intensity(true, 1000, 0) >= 2030 && intensity(true, 1000, 0) <= 2066);
  assert(intensity(true, 2000, 0) == 4096);
  assert(intensity(true, 2500, 0) == 4096);
  // Fade-out begins at the first successful missing update: NO extra hold.
  assert(intensity(false, 0, 4096) == 4096);
  assert(intensity(false, 2000, 4096) < 4096);
  assert(intensity(false, 6000, 4096) == 2048);
  assert(intensity(false, 12000, 4096) == 0);
  assert(intensity(false, 20000, 4096) == 0);
  // Interrupted fading recovers from the exact current brightness.
  const uint16_t dimmed = intensity(false, 10000, 4096);
  assert(dimmed > 0 && dimmed < 2048);
  assert(intensity(true, 0, dimmed) == dimmed);
  assert(intensity(true, 2000, dimmed) == 4096);
  // A newcomer fades monotonically; a disappearing target decreases.
  uint16_t prev = 0;
  for (uint32_t ms = 0; ms <= 2000; ms += 10) {
    const uint16_t curr = intensity(true, ms, 0);
    assert(curr >= prev); prev = curr;
  }
  prev = 4096;
  for (uint32_t ms = 0; ms <= 12000; ms += 10) {
    const uint16_t curr = intensity(false, ms, 4096);
    assert(curr <= prev); prev = curr;
  }
  // Output follows the real configured trail fade curve when disappearing.
  for (uint8_t c = 0; c < 4; ++c) {
    ui::trail_fade::selectCurve(c);
    const uint16_t* curve = ui::trail_fade::activeCurveQ12();
    assert(intensity(false, 0, 4096, curve) == 4096);
    assert(intensity(false, 12000, 4096, curve) == 0);
    uint16_t last = 4096;
    for (uint32_t t = 0; t <= 12000; t += 5) {
      const uint16_t value = intensity(false, t, 4096, curve);
      assert(value <= last); last = value;
    }
  }
  assert(multiply(4096,4096) == 4096);
  assert(multiply(2048,2048) == 1024);
  puts("V17.37 aircraft appear/disappear fade Q12: OK");
}
