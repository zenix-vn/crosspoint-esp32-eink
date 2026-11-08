#pragma once

#include "WeatherStore.h"

class GfxRenderer;

// A weather symbol drawn from shapes (sun, cloud, rain, snow, fog, lightning)
// in a size x size box at (x, y), so it scales without bitmaps.
void drawWeatherIcon(const GfxRenderer& renderer, int x, int y, int size, weather::Kind kind);
