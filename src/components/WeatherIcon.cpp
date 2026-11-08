#include "WeatherIcon.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>

namespace {

void disc(const GfxRenderer& renderer, const int cx, const int cy, const int r, const bool black) {
  if (r <= 0) return;
  renderer.fillRoundedRect(cx - r, cy - r, 2 * r, 2 * r, r, black ? Color::Black : Color::White);
}

void sun(const GfxRenderer& renderer, const int cx, const int cy, const int r) {
  const int stroke = std::max(2, r / 4);
  disc(renderer, cx, cy, r, true);
  disc(renderer, cx, cy, r - stroke, false);
  for (int i = 0; i < 8; i++) {
    const float a = static_cast<float>(i) * 0.785398f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    renderer.drawLine(cx + static_cast<int>(c * (r + stroke + 1)), cy + static_cast<int>(s * (r + stroke + 1)),
                      cx + static_cast<int>(c * (r * 2 - 1)), cy + static_cast<int>(s * (r * 2 - 1)), stroke, true);
  }
}

// An outlined cloud whose flat bottom sits on `bottom`, spanning [left, left + w).
void cloud(const GfxRenderer& renderer, const int left, const int bottom, const int w) {
  const int stroke = std::max(2, w / 16);
  const int r1 = w / 4;      // left lobe
  const int r2 = w * 3 / 10;  // middle lobe
  const int r3 = w / 5;      // right lobe
  const int baseH = r1;
  // Black silhouette, then the same shapes inset in white leave an outline.
  for (const int inset : {0, stroke}) {
    const bool black = inset == 0;
    disc(renderer, left + r1, bottom - r1, r1 - inset, black);
    disc(renderer, left + w / 2, bottom - r1 - r2 / 2, r2 - inset, black);
    disc(renderer, left + w - r3, bottom - r3, r3 - inset, black);
    renderer.fillRect(left + r1, bottom - baseH + inset, w - r1 - r3, baseH - 2 * inset, black);
  }
}

}  // namespace

void drawWeatherIcon(const GfxRenderer& renderer, const int x, const int y, const int size,
                     const weather::Kind kind) {
  using weather::Kind;
  const int cx = x + size / 2;
  const int stroke = std::max(2, size / 16);
  const int cloudW = size * 7 / 8;
  const int cloudLeft = x + (size - cloudW) / 2;
  // Clouds with something falling sit higher to leave room below.
  const int cloudBottom = y + size * 5 / 8;
  switch (kind) {
    case Kind::Clear:
      sun(renderer, cx, y + size / 2, size / 5);
      break;
    case Kind::PartlyCloudy:
      sun(renderer, x + size * 2 / 3, y + size / 3, size / 7);
      cloud(renderer, x, y + size * 7 / 8, size * 3 / 4);
      break;
    case Kind::Cloudy:
      cloud(renderer, cloudLeft, y + size * 3 / 4, cloudW);
      break;
    case Kind::Fog:
      cloud(renderer, cloudLeft, y + size / 2, cloudW);
      for (int i = 0; i < 3; i++) {
        const int ly = y + size * 5 / 8 + i * (size / 8);
        renderer.drawLine(x + size / 8 + (i % 2) * size / 8, ly, x + size * 7 / 8 - (i % 2) * size / 8, ly, stroke,
                          true);
      }
      break;
    case Kind::Drizzle:
    case Kind::Rain: {
      cloud(renderer, cloudLeft, cloudBottom, cloudW);
      const int drops = kind == Kind::Rain ? 3 : 2;
      const int len = kind == Kind::Rain ? size / 4 : size / 8;
      for (int i = 0; i < drops; i++) {
        const int dx = cloudLeft + cloudW * (i + 1) / (drops + 1);
        renderer.drawLine(dx, cloudBottom + size / 12, dx - size / 12, cloudBottom + size / 12 + len, stroke, true);
      }
      break;
    }
    case Kind::Snow:
      cloud(renderer, cloudLeft, cloudBottom, cloudW);
      for (int i = 0; i < 3; i++) {
        const int dx = cloudLeft + cloudW * (i + 1) / 4;
        const int dy = cloudBottom + size / 6 + (i % 2) * size / 10;
        disc(renderer, dx, dy, std::max(2, size / 20), true);
      }
      break;
    case Kind::Thunder: {
      cloud(renderer, cloudLeft, cloudBottom, cloudW);
      const int bx = cx;
      const int by = cloudBottom + 2;
      const int xs[] = {bx + size / 12, bx - size / 10, bx, bx - size / 8, bx + size / 10, bx + size / 50};
      const int ys[] = {by, by + size / 6, by + size / 6, by + size * 3 / 8 - 2, by + size / 8, by + size / 8};
      renderer.fillPolygon(xs, ys, 6, true);
      break;
    }
    case Kind::Unknown:
      break;
  }
}
