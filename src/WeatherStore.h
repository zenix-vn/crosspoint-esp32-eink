#pragma once

#include <cstdint>
#include <string>

// The cached weather forecast shown on Home and on the "Today" sleep screen.
// Data comes from Open-Meteo (no account needed) through WeatherFetcher, which
// runs only on the way to sleep: the device restarts on wake anyway, so the
// Wi-Fi and TLS heap is handed back without an extra restart.
namespace weather {

constexpr int DAYS = 3;

enum class Kind : uint8_t { Unknown, Clear, PartlyCloudy, Cloudy, Fog, Drizzle, Rain, Snow, Thunder };

struct Day {
  char date[11] = "";  // YYYY-MM-DD in the place's time zone
  int16_t code = -1;   // WMO weather code, -1 = none
  int16_t maxC = 0;
  int16_t minC = 0;
  uint8_t rainPct = 0;  // highest chance of precipitation that day
};

struct Forecast {
  // Where the forecast is for; `query` is the location setting it was looked
  // up from (empty = found from the network address).
  std::string place;
  std::string query;
  float lat = 0;
  float lon = 0;
  bool located = false;
  int64_t locatedAt = 0;  // UTC epoch

  int64_t fetchedAt = 0;  // UTC epoch, 0 = never
  int16_t tempC = 0;
  int16_t code = -1;
  uint8_t humidity = 0;
  uint16_t windKmh = 0;
  Day days[DAYS];

  bool valid() const { return fetchedAt > 0 && code >= 0; }
  // The forecast day for a local date (YYYY-MM-DD), or null.
  const Day* day(const char* date) const;

  bool load();
  bool save() const;
};

Kind kindOf(int code);
// Short translated description of a WMO weather code.
const char* describe(int code);
// Today's local date as YYYY-MM-DD; false while the clock is not set.
bool localDate(char* out, size_t size, int dayOffset = 0);

}  // namespace weather
