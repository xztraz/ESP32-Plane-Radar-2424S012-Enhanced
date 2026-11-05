#!/usr/bin/env python3
"""Generate 240x240, 8x8 AA, palette-index radar background patterns.

Retains the V17.12 approved Original data intact. New patterns store their
brightness as RGB332 green codes (level << 2), to preserve backward-compatible
flash content. At runtime any of these becomes the selected theme via an 8-entry
lookup while copying the flash bytes into the single RGB332 framebuffer.

One-time PC generation; no extra CPU or RAM for shape calculation on the ESP32.
"""
from pathlib import Path
import hashlib
import math

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'data'
SIZE = 240
C = SIZE // 2
OUTER = 107
RINGS = [OUTER * i // 4 for i in range(1, 5)]  # 26, 53, 80, 107
RING_LEVELS = [3.0, 3.0, 2.0, 1.0]
AA = 8
OFFSETS = tuple((i + .5) / AA - .5 for i in range(AA))
RING_HALF = .78
CROSS_HALF = .74

PATTERNS = (
    'original',
    'classic_ppi',
    'minimal',
    'polar',
    'dashed',
    'super_minimal',
)


def spoke_level(r):
    if r <= RINGS[1]:
        return 3.0
    if r >= RINGS[3]:
        return 1.0
    if r <= RINGS[2]:
        t = (r - RINGS[1]) / (RINGS[2] - RINGS[1])
        return 3.0 - t
    t = (r - RINGS[2]) / (RINGS[3] - RINGS[2])
    return 2.0 - t


def ring_level(r, theta, pattern):
    if pattern == 'super_minimal':
        return 1.0 if abs(r - RINGS[2]) <= .77 else 0.0
    for i, radius in enumerate(RINGS):
        if abs(r - radius) > RING_HALF:
            continue
        if pattern == 'dashed' and i != 2:
            # Actual selected-range ring (third) remains solid in this pattern.
            # Every other ring has 30 equal-spaced 7.2-degree dashes.
            angle = (theta * 180.0 / math.pi + 360.0) % 360.0
            if (angle % 12.0) > 7.2:
                return 0.0
        return RING_LEVELS[i]
    return 0.0


def pattern_sample(dx, dy, pattern):
    r = math.hypot(dx, dy)
    theta = math.atan2(dx, -dy)   # 0 deg north, clockwise positive
    level = ring_level(r, theta, pattern)
    if pattern in ('original', 'classic_ppi'):
        # Identical round-ended V17.12 crosshair geometry.
        if r <= OUTER + CROSS_HALF and (
            (abs(dy) <= CROSS_HALF and abs(dx) <= OUTER) or
            (abs(dx) <= CROSS_HALF and abs(dy) <= OUTER) or
            ((abs(dx) > OUTER or abs(dy) > OUTER) and
             min(math.hypot(abs(dx) - OUTER, dy),
                 math.hypot(dx, abs(dy) - OUTER)) <= CROSS_HALF)
        ):
            level = max(level, spoke_level(min(r, OUTER)))
    if pattern == 'classic_ppi' and 108.2 <= r <= 118.1:
        deg = (math.degrees(theta) + 360.0) % 360.0
        nearest = round(deg / 10.0) * 10
        if nearest % 90 != 0:  # N/E/S/W glyphs occupy these positions.
            delta = math.radians(deg - nearest)
            big = nearest % 30 == 0
            end_r = 117.2 if big else 113.7
            if 109.2 <= r <= end_r:
                # Rounded ends for AA, independent of radial resolution.
                sideways = abs(math.sin(delta) * r)
                if sideways <= (.72 if big else .65):
                    level = max(level, 2.6 if big else 2.15)
    if pattern == 'polar' and 6.0 <= r <= OUTER:
        # Faint 30-deg radials, avoiding the central-dot area.
        deg = (math.degrees(theta) + 360.0) % 360.0
        nearest = round(deg / 30.0) * 30
        if abs(math.sin(math.radians(deg - nearest)) * r) <= .63:
            level = max(level, 1.8)
    return level


def pixel_near_shape(x, y, pattern):
    dx = x - C
    dy = y - C
    r = math.hypot(dx, dy)
    if pattern == 'original':
        return False
    targets = [RINGS[2]] if pattern == 'super_minimal' else RINGS
    if any(abs(r - rr) <= 1.6 for rr in targets):
        return True
    if pattern == 'classic_ppi':
        if (abs(dx) <= 1.6 and abs(dy) <= OUTER + 1.6) or (abs(dy) <= 1.6 and abs(dx) <= OUTER + 1.6):
            return True
        if 107.5 <= r <= 118.8:
            return True
    if pattern == 'polar' and 5 <= r <= OUTER + 1.0:
        # Conservative bounding check for radiating spokes.
        theta = (math.degrees(math.atan2(dx, -dy)) + 360) % 360
        delta = theta - round(theta / 30) * 30
        if abs(math.sin(math.radians(delta)) * r) <= 1.6:
            return True
    return False


def generate(pattern):
    if pattern == 'original':
        source = (DATA / 'radar_grid.rgb332').read_bytes()
        if len(source) != SIZE * SIZE:
            raise ValueError('Original V17.12 grid bitmap missing or wrong length')
        return source
    bitmap = bytearray(SIZE * SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            if not pixel_near_shape(x, y, pattern):
                continue
            dx = x - C
            dy = y - C
            level = sum(pattern_sample(dx + sx, dy + sy, pattern)
                        for sy in OFFSETS for sx in OFFSETS) / (AA * AA)
            quantized = min(7, max(0, int(math.floor(level + .5))))
            bitmap[y * SIZE + x] = quantized << 2
    return bytes(bitmap)


def main():
    for pattern in PATTERNS:
        data = generate(pattern)
        if pattern != 'original':
            path = DATA / ('radar_%s.rgb332' % pattern)
            path.write_bytes(data)
        else:
            path = DATA / 'radar_grid.rgb332'
        unique = sorted(set(data))
        print(pattern, path.name, len(data), unique,
              hashlib.sha256(data).hexdigest()[:16], flush=True)


if __name__ == '__main__':
    main()
