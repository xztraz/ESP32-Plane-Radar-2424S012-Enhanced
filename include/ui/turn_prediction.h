#pragma once

#include <cstdint>
#include "services/adsb_client.h"

namespace ui::turn_prediction {

// A display-only prediction. Never write these coordinates into ADS-B history.
struct Pose {
  float lat = 0.0f;
  float lon = 0.0f;
  float nose_deg = 0.0f;
  float track_deg = 0.0f;
  float track_rate_deg_s = 0.0f;
  float age_s = 0.0f;
  bool turning = false;
};

// Called once per aircraft on each frame, on the display task only.
// Identity must be stable across successive ADS-B snapshots.
Pose predict(const char* identity, const services::adsb::Aircraft& aircraft,
             uint32_t fetch_ms, uint32_t now_ms, float km_per_deg_lon);

// Predict a point on the temporary curved segment between the received fix
// and the current symbol. This does not update the turn estimator.
void positionAt(const services::adsb::Aircraft& aircraft, float track_rate_deg_s,
                float seconds_since_fix, float km_per_deg_lon,
                float* out_lat, float* out_lon);

void reset();

}  // namespace ui::turn_prediction
