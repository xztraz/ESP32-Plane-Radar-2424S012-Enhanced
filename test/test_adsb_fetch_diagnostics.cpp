// g++ -std=c++11 -O2 -Wall -Wextra -Iinclude test/test_adsb_fetch_diagnostics.cpp -o /tmp/adsb-diag-test && /tmp/adsb-diag-test
#include "services/adsb_fetch_diagnostics.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

using services::adsb::FetchDiagnostics;
using Kind = FetchDiagnostics::EventKind;

int main() {
  FetchDiagnostics diag;
  constexpr float radius = 50.0f;
  constexpr double lat = 59.605181, lon = 17.416242;
  // A truly empty but valid response is normal and triggers only bounded info.
  assert(diag.valid(100, 0, radius, lat, lon).kind == Kind::kZeroStarted);
  assert(diag.valid(5100, 0, radius, lat, lon).kind == Kind::kNone);
  auto e = diag.valid(100 + FetchDiagnostics::kZeroReportIntervalMs, 0,
                      radius, lat, lon);
  assert(e.kind == Kind::kZeroPeriodic && e.samples == 3);
  assert(diag.valid(100 + FetchDiagnostics::kZeroReportIntervalMs + 5000,
                    0, radius, lat, lon).kind == Kind::kNone);
  e = diag.valid(100 + FetchDiagnostics::kZeroReportIntervalMs + 6000,
                 7, radius, lat, lon);
  assert(e.kind == Kind::kZeroEnded && e.previous_radius_km == radius);
  assert(diag.valid(990000, 7, radius, lat, lon).kind == Kind::kNone);
  // Range switch while receiving zero starts a new period.
  assert(diag.valid(1000000, 0, 20, lat, lon).kind == Kind::kZeroStarted);
  assert(diag.valid(1005000, 0, radius, lat, lon).kind == Kind::kZeroStarted);
  assert(diag.valid(1010000, 7, radius, lat, lon).kind == Kind::kNone);

  // Missing/malformed 'ac' does not count as valid zero; diagnostics are quiet.
  assert(diag.invalidPayload(2000000).kind == Kind::kInvalidStarted);
  for (uint32_t t = 2005000; t < 2300000; t += 5000)
    assert(diag.invalidPayload(t).kind == Kind::kNone);
  e = diag.invalidPayload(2300000);
  assert(e.kind == Kind::kInvalidPeriodic && e.samples > 1);
  assert(diag.valid(2305000, 0, radius, lat, lon).kind == Kind::kZeroStarted);
  assert(diag.invalidPayload(2310000).kind == Kind::kInvalidStarted);

  // All timing comparisons support millis() rollover.
  FetchDiagnostics rollover;
  assert(rollover.valid(0xfffffff0u, 0, radius, lat, lon).kind == Kind::kZeroStarted);
  assert(rollover.valid(0x00000010u, 0, radius, lat, lon).kind == Kind::kNone);
  assert(rollover.invalidPayload(0xfffffff0u).kind == Kind::kInvalidStarted);
  assert(rollover.invalidPayload(0x00000010u).kind == Kind::kNone);
  std::puts("PASS: valid zero, sparse summaries, range changes, invalid response throttling, rollover");
}
