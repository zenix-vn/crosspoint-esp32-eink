#include "WeatherStore.h"

#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace weather {

namespace {

constexpr char PATH[] = "/.crosspoint/weather.json";
constexpr int MIN_VALID_YEAR = 2024;

}  // namespace

const Day* Forecast::day(const char* date) const {
  for (const Day& d : days) {
    if (d.code >= 0 && strcmp(d.date, date) == 0) return &d;
  }
  return nullptr;
}

bool Forecast::load() {
  std::string text;
  if (!Storage.exists(PATH) || !Storage.readFileToString("WTHR", PATH, 4096, text)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, text)) return false;
  place = doc["place"] | "";
  query = doc["query"] | "";
  lat = doc["lat"] | 0.0f;
  lon = doc["lon"] | 0.0f;
  located = doc["located"] | false;
  locatedAt = doc["locatedAt"] | static_cast<int64_t>(0);
  fetchedAt = doc["fetchedAt"] | static_cast<int64_t>(0);
  tempC = doc["temp"] | 0;
  code = doc["code"] | -1;
  humidity = doc["humidity"] | 0;
  windKmh = doc["wind"] | 0;
  int i = 0;
  for (JsonObjectConst d : doc["days"].as<JsonArrayConst>()) {
    if (i >= DAYS) break;
    Day& day = days[i++];
    snprintf(day.date, sizeof(day.date), "%s", d["date"] | "");
    day.code = d["code"] | -1;
    day.maxC = d["max"] | 0;
    day.minC = d["min"] | 0;
    day.rainPct = d["rain"] | 0;
  }
  return true;
}

bool Forecast::save() const {
  JsonDocument doc;
  doc["place"] = place;
  doc["query"] = query;
  doc["lat"] = lat;
  doc["lon"] = lon;
  doc["located"] = located;
  doc["locatedAt"] = locatedAt;
  doc["fetchedAt"] = fetchedAt;
  doc["temp"] = tempC;
  doc["code"] = code;
  doc["humidity"] = humidity;
  doc["wind"] = windKmh;
  JsonArray list = doc["days"].to<JsonArray>();
  for (const Day& d : days) {
    JsonObject o = list.add<JsonObject>();
    o["date"] = d.date;
    o["code"] = d.code;
    o["max"] = d.maxC;
    o["min"] = d.minC;
    o["rain"] = d.rainPct;
  }
  std::string text;
  serializeJson(doc, text);
  Storage.ensureDirectoryExists("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("WTHR", PATH, file)) return false;
  const bool ok = file.write(text.data(), text.size()) == text.size();
  file.close();
  return ok;
}

Kind kindOf(const int code) {
  if (code < 0) return Kind::Unknown;
  if (code == 0) return Kind::Clear;
  if (code <= 2) return Kind::PartlyCloudy;
  if (code == 3) return Kind::Cloudy;
  if (code == 45 || code == 48) return Kind::Fog;
  if (code >= 51 && code <= 57) return Kind::Drizzle;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return Kind::Rain;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Kind::Snow;
  if (code >= 95) return Kind::Thunder;
  return Kind::Unknown;
}

const char* describe(const int code) {
  switch (kindOf(code)) {
    case Kind::Clear:
      return tr(STR_WEATHER_CLEAR);
    case Kind::PartlyCloudy:
      return tr(STR_WEATHER_PARTLY_CLOUDY);
    case Kind::Cloudy:
      return tr(STR_WEATHER_CLOUDY);
    case Kind::Fog:
      return tr(STR_WEATHER_FOG);
    case Kind::Drizzle:
      return tr(STR_WEATHER_DRIZZLE);
    case Kind::Rain:
      return code >= 80 ? tr(STR_WEATHER_SHOWERS) : tr(STR_WEATHER_RAIN);
    case Kind::Snow:
      return tr(STR_WEATHER_SNOW);
    case Kind::Thunder:
      return tr(STR_WEATHER_THUNDER);
    case Kind::Unknown:
      break;
  }
  return "";
}

bool localDate(char* out, const size_t size, const int dayOffset) {
  struct tm now {};
  if (!halClock.localTime(now) || now.tm_year + 1900 < MIN_VALID_YEAR) return false;
  now.tm_mday += dayOffset;
  now.tm_hour = 12;  // away from DST edges when normalizing
  mktime(&now);
  snprintf(out, size, "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
  return true;
}

}  // namespace weather
