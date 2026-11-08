#pragma once

#include <stdint.h>

#include "BlueNoise.h"

// Dither to 4 levels (0-3) with a blue-noise threshold: evenly spread dots and
// no repeating grid, unlike a small Bayer matrix. Stateless, so it works with
// any pixel processing order (block decoders, pixel caches).
inline uint8_t applyDither4Level(uint8_t gray, int x, int y) {
  const int noise = BLUE_NOISE[(y & (BLUE_NOISE_SIZE - 1)) * BLUE_NOISE_SIZE + (x & (BLUE_NOISE_SIZE - 1))];
  // Offset within +/-42, half of the quantization step 85.
  int adjusted = gray + (noise - 128) * 85 / 256;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}
