#pragma once
#include <stdint.h>

namespace ui {
// Timings are the most recently completed frame, in microseconds.
// The nested measurements let us find bottlenecks without a second buffer.
struct RadarFrameTimings {
  uint32_t grid_us = 0;       // Full static grid including runways
  uint32_t runways_us = 0;    // Subset of grid_us
  uint32_t flash_copy_us = 0; // Raw flash-to-framebuffer memcpy only
  uint32_t aircraft_us = 0;   // Projection, trails, labels and icons
  uint32_t projection_us = 0;
  uint32_t trails_us = 0;
  uint32_t dots_us = 0;
  uint32_t labels_us = 0;
  uint32_t icons_us = 0;
  uint32_t transfer_us = 0;
  uint16_t cpu_mhz_frame_start = 0;  // Frequency at render start.
  uint16_t cpu_mhz_frame_end = 0;    // Frequency after screen transfer.
};

/** Count of slow SPI transfers and CPU frequency measured in the last one. */
struct RadarSpiSlowDiag {
  uint32_t count = 0;
  uint16_t cpu_mhz_start = 0;
  uint16_t cpu_mhz_end = 0;
  uint32_t last_transfer_us = 0;
};
RadarSpiSlowDiag radarDisplaySpiSlowDiag();

RadarFrameTimings radarDisplayFrameTimings();
/** 1 when the embedded flash grid has the expected dimensions. */
bool radarDisplayFlashGridActive();


/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw the complete radar scene (scheduled at 2–6.25 Hz by range). */
void radarDisplayRefreshAircraft();

/** Require the full scene to redraw after changing settings or range. */
void radarDisplayInvalidateSlowLayer();
void radarDisplayThemeChanged();
/** Last whole-frame and scene composition durations (microseconds). */
uint32_t radarDisplayLastRenderUs();
uint32_t radarDisplayLastSlowRenderUs();
/** Number of full-screen framebuffers (1 RGB332, or 0 for direct fallback). */
uint8_t radarDisplayFramebufferCount();

}  // namespace ui
