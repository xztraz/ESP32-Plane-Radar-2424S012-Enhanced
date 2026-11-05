#include "ui/trail_fade.h"
#include "services/aircraft_palette.h"
#include <cassert>
#include <cstdio>

int main() {
  using namespace ui::trail_fade;
  selectCurve(1);  // Linear, to observe smooth movement over real time.
  constexpr uint32_t interval = 15000;
  const uint32_t reciprocal = reciprocalIntervalQ16(interval);
  assert(fixAgeMs(1024u * 101u + 200u, 100u) == 1224u);
  assert(fixAgeMs(1024u * 65536u + 200u, 65535u) == 1224u);
  assert(agePositionQ8(0, interval, reciprocal, 32) == 0);
  assert(agePositionQ8(interval, interval, reciprocal, 32) >= 255);
  assert(agePositionQ8(interval, interval, reciprocal, 32) <= 257);
  assert(agePositionQ8(interval * 32u, interval, reciprocal, 32) == 31 * 256);
  // The same stored fix must receive identical color when a NEW fix arrives.
  // Unlike a sample-number-based LUT, inserting a fix doesn't shift its age.
  const uint32_t now = 80000;
  const uint16_t old_fix_tick = 20;
  const uint32_t pos_before = agePositionQ8(fixAgeMs(now, old_fix_tick),interval,reciprocal,32);
  const uint32_t pos_after  = agePositionQ8(fixAgeMs(now, old_fix_tick),interval,reciprocal,32);
  assert(pos_before == pos_after);
  const uint16_t shade_before = shadeAtPositionQ8(0,pos_before,32,25);
  assert(shade_before == shadeAtPositionQ8(0,pos_after,32,25));
  // With no new samples, fade position and color progress continuously.
  uint16_t last_shade = shadeAtPositionQ8(0,0,32,25);
  for (uint32_t ms=1000; ms<=400000; ms+=250) {
    const uint32_t position=agePositionQ8(ms,interval,reciprocal,32);
    const uint16_t shade=shadeAtPositionQ8(0,position,32,25);
    assert(shade >= last_shade);
    last_shade=shade;
  }
  puts("V17.25 timestamp-anchored segment colors / fixed-point continuity: OK");
}
