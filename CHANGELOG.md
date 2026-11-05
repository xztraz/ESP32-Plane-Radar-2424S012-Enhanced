# Changes from MatixYo's ESP32-Plane-Radar

This document describes the **customized fork's changes relative to the original project**, not every internal development iteration. The fork source version is **v2.0.0**.

This is an independently developed fork and is **not represented as a rebase of the latest upstream tag**. Compare against the actual fork parent commit when publishing to obtain an exact Git diff.

## Hardware and build

- Reconfigured display pinout and settings for the ESP32-2424S012 non-touch (ESP32-C3 / GC9A01).
- Configured display SPI at 80 MHz and a 160 MHz CPU, with Wi-Fi power saving disabled and TX power capped at 8.5 dBm.
- Updated PlatformIO platform/dependencies, added Windows build/merge/upload scripts, a single merged flash image at offset `0x0000`, and expanded diagnostic reporting.

## Display and aircraft rendering

- Replaced the two-layer/double-buffer approach with one RGB332 framebuffer and complete range-dependent refreshes.
- Added six antialiased, flash-backed radar patterns; palette recoloring provides six theme colors without duplicate bitmaps for every color.
- Added antialiased symbols with configurable size, helicopter rings and smoother screen-edge dots.
- Added fixed-point path-interpolated aircraft trails with phosphor-like color gradients, selectable fade curves and additive compositing.
- Updated geometry so aircraft icons approach the physical edge while the trail follows the actual projected position rather than the bearing-clamped edge dot.
- Added 2-second aircraft-symbol fade-in and 12-second additive fade-out, triggered by missing aircraft from a **successful** ADS-B update. Aircraft labels appear/disappear immediately, and trails fade independently.

## Web configuration and overlays

- Reorganized settings into General, Aircraft, Wi-Fi and Status pages.
- Added configurable trail fade, length and darkening, symbol scaling, label lines and brightness, colors and watchlist rules.
- Added custom watchlist names as an additional line, retaining callsigns and other enabled aircraft information.
- Improved adaptive label placement and priority behavior, retained runway overlays and added longer range presets.

## Networking and reliability

- Replaced the previous mbedTLS HTTPS download path with BearSSL and a streaming-compatible HTTP response reader to reduce peak TLS memory use.
- Added memory, fetch-success and render-performance diagnostics, recovery checks for the LAN web server, and coordinated saves during aircraft downloads.

## Attribution

Original copyright and licensing: [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar). The repository retains its MIT [LICENSE](LICENSE).
