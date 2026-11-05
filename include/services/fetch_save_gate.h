#pragma once
#include <atomic>
#include <stdint.h>

namespace services {
// Allocation-free coordination between the HTTPS fetch task and
// web settings writes. Non-blocking: neither task waits while holding a
// web-server or flash lock. No timers, heap allocations, or extra RTOS task.
class FetchSaveGate {
 public:
  bool tryBeginFetch(uint32_t now_ms) {
    if (paused(now_ms)) return false;
    uint8_t idle = 0;
    if (!state_.compare_exchange_strong(idle, static_cast<uint8_t>(1)))
      return false;
    // A save may have requested priority while the fetch won the gate.
    if (paused(now_ms)) { state_.store(0); return false; }
    return true;
  }
  void endFetch() { state_.store(0); }

  // Tiny browser preflight: reserve time to receive/parse the upcoming POST
  // before constructing its larger request body on the ESP32. The actual
  // POST must still acquire the gate (a GET cannot hold it across requests).
  bool reserveSaveWindow(uint32_t now_ms) {
    next_fetch_after_ms_.store(now_ms + 1500U);
    return state_.load() == 0;
  }

  bool tryBeginSave(uint32_t now_ms) {
    uint8_t idle = 0;
    if (!state_.compare_exchange_strong(idle, static_cast<uint8_t>(2))) {
      // Browser receives HTTP 503 and retries. Pause the next fetch to
      // give it a chance to complete, without blocking the web loop.
      next_fetch_after_ms_.store(now_ms + 2000U);
      return false;
    }
    return true;
  }
  void endSave(uint32_t now_ms) {
    // Let HTTP POST buffers and NVS writes settle before the next TLS GET.
    next_fetch_after_ms_.store(now_ms + 1000U);
    state_.store(0);
  }

 private:
  bool paused(uint32_t now_ms) const {
    const uint32_t until = next_fetch_after_ms_.load();
    return until != 0 && static_cast<int32_t>(until - now_ms) > 0;
  }
  std::atomic<uint8_t> state_{0}; // 0=free, 1=TLS fetch, 2=settings save
  std::atomic<uint32_t> next_fetch_after_ms_{0};
};
}  // namespace services
