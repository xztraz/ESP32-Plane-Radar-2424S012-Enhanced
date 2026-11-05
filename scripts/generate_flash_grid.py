#!/usr/bin/env python3
"""Generate the fixed 240x240 RGB332 antialiased radar grid in flash.

V17.12: 8x8 subpixel coverage for four antialiased rings and crosshairs.
The crosshair intensity follows the ring intensity radially, with smoothly
interpolated steps: both inner rings = 3/7 RGB332 green, third ring = 2/7,
outer ring = 1/7. RGB332 levels are selected by rounding calculated
subpixel coverage, without spatial dithering or speckle.

Only executed on the development computer. The ESP32 copies the resulting
57,600-byte constant image from flash into its EXISTING single framebuffer.
No runtime bitmap generation, flash writes, or extra buffers.
"""
from pathlib import Path
import hashlib
import math
import re

ROOT = Path(__file__).resolve().parents[1]
THEME = (ROOT / 'include/ui/radar_theme.h').read_text()


def numeric_constant(name):
    pattern = r'\b' + re.escape(name) + r'\s*=\s*(\d+)\s*;'
    match = re.search(pattern, THEME)
    if not match:
        raise ValueError(f'missing integer geometry constant {name}')
    return int(match.group(1))


SIZE = numeric_constant('kSize')
CENTER = SIZE // 2
OUTER = numeric_constant('kGridOuterRadius')
if (SIZE, OUTER) != (240, 107):
    raise ValueError('radar grid geometry changed: review image generator')

# Preserve original V17.9 fully-on ring shade in RGB332. The 8-bit green
# targets 112, 112, 80 and 45 quantize to 3, 3, 2 and 1 respectively.
RADIUS = tuple(OUTER * i // 4 for i in range(1, 5))
RING_GREEN = (3.0, 3.0, 2.0, 1.0)
RING_HALF_WIDTH = 0.78   # 1.56-pixel-wide ring with subpixel-smoothed edges.
CROSS_HALF_WIDTH = 0.74  # 1.48-pixel-wide crosshair with subpixel-smoothed edges.
AA = 8                   # 64 evenly spaced area samples per pixel.

def cross_green(radius):
    # Keep both inner rings at the original 3/7 green. Interpolate to the
    # next ring brightness, so the crosshairs get darker toward the edge.
    if radius <= RADIUS[1]:
        return RING_GREEN[1]
    if radius >= RADIUS[3]:
        return RING_GREEN[3]
    if radius <= RADIUS[2]:
        t = (radius - RADIUS[1]) / (RADIUS[2] - RADIUS[1])
        return RING_GREEN[1] * (1 - t) + RING_GREEN[2] * t
    t = (radius - RADIUS[2]) / (RADIUS[3] - RADIUS[2])
    return RING_GREEN[2] * (1 - t) + RING_GREEN[3] * t


def subpixel_color(dx, dy):
    r = math.hypot(dx, dy)
    green = 0.0
    if r <= OUTER + RING_HALF_WIDTH:
        for ring_radius, ring_green in zip(RADIUS, RING_GREEN):
            if abs(r - ring_radius) <= RING_HALF_WIDTH:
                green = ring_green
                break
        # Round-capped horizontal and vertical spokes, fading radially.
        if ((abs(dy) <= CROSS_HALF_WIDTH and abs(dx) <= OUTER) or
            (abs(dx) <= CROSS_HALF_WIDTH and abs(dy) <= OUTER) or
            ((abs(dx) > OUTER or abs(dy) > OUTER) and
             min(math.hypot(abs(dx) - OUTER, dy),
                 math.hypot(dx, abs(dy) - OUTER)) <= CROSS_HALF_WIDTH)):
            green = max(green, cross_green(min(r, OUTER)))
    return green


def main():
    img = bytearray(SIZE * SIZE)
    # Store offsets relative to integer pixel centers (and thus the existing
    # LovyanGFX integer center C=120), not half-pixel-shifted coordinates.
    sample_offsets = tuple((a + 0.5) / AA - 0.5 for a in range(AA))
    for y in range(SIZE):
        dy = y - CENTER
        for x in range(SIZE):
            dx = x - CENTER
            distance = math.hypot(dx, dy)
            # Pixels too far from every stroke cannot contain any coverage.
            near_ring = any(abs(distance - r) < RING_HALF_WIDTH + 0.71
                            for r in RADIUS)
            near_cross = (abs(dx) < CROSS_HALF_WIDTH + 0.71 and
                          abs(dy) < OUTER + CROSS_HALF_WIDTH + 0.71) or (
                          abs(dy) < CROSS_HALF_WIDTH + 0.71 and
                          abs(dx) < OUTER + CROSS_HALF_WIDTH + 0.71)
            if not (near_ring or near_cross):
                continue
            level = sum(subpixel_color(dx + sx, dy + sy)
                        for sy in sample_offsets for sx in sample_offsets) / (AA * AA)
            # Nearest RGB332 green level, with no spatial dithering.
            # Coverage still softens edges where intermediate palette values
            # exist; the darkest outer ring is naturally more quantized.
            quantized = math.floor(level + 0.5)
            img[y * SIZE + x] = max(0, min(7, quantized)) << 2

    path = ROOT / 'data/radar_grid.rgb332'
    path.write_bytes(img)
    assert len(img) == SIZE * SIZE
    print('Generated:', path, 'bytes:', len(img))
    print('Rings:', RADIUS, 'RGB332 green codes:', RING_GREEN)
    print('Palette:', sorted(set(img)))
    print('SHA256:', hashlib.sha256(img).hexdigest())


if __name__ == '__main__':
    main()
