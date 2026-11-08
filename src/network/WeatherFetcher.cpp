#include "WeatherFetcher.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <ctime>

#include <I18n.h>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "network/DeviceLink.h"
#include "network/HttpDownloader.h"
#include "util/PluginHttp.h"

namespace weather {

namespace {

constexpr int64_t MAX_AGE_S = 3 * 3600;
constexpr int64_t RELOCATE_S = 7 * 24 * 3600;  // automatic location: look again weekly
constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
constexpr size_t MAX_BODY = 8192;
constexpr time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01

bool fetchJson(const std::string& url, JsonDocument& doc) {
  std::string body;
  const bool ok = HttpDownloader::fetchUrl(url, [&body](const uint8_t* data, const size_t len) {
    if (body.size() + len > MAX_BODY) return false;
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("WTHR", "Fetch failed (status %d)", HttpDownloader::lastStatus());
    return false;
  }
  return !deserializeJson(doc, body);
}

bool joinSavedNetwork() {
  WIFI_STORE.loadFromFile();
  std::optional<WifiCredential> cred;
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty()) cred = WIFI_STORE.findCredential(last);
  if (!cred) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) return false;

  WiFi.persistent(false);  // credentials live in WifiCredentialStore
  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT_MS) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("WTHR", "Could not join %s", cred->ssid.c_str());
    return false;
  }
  return true;
}

void leaveNetwork() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Coordinates from the location setting (Open-Meteo geocoding) or, when it is
// empty, from the public address of the network.
bool locate(Forecast& forecast, const std::string& query, const time_t now) {
  JsonDocument doc;
  if (query.empty()) {
    if (!fetchJson("http://ip-api.com/json/?fields=status,city,lat,lon", doc) || doc["status"] != "success") {
      return false;
    }
    forecast.lat = doc["lat"] | 0.0f;
    forecast.lon = doc["lon"] | 0.0f;
    forecast.place = doc["city"] | "";
  } else {
    std::string language = LANGUAGE_CODES[static_cast<int>(I18N.getLanguage())];
    for (char& c : language) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const std::string url = "https://geocoding-api.open-meteo.com/v1/search?count=1&format=json&language=" +
                            language + "&name=" + pluginhttp::urlEncodeQuery(query);
    if (!fetchJson(url, doc) || !doc["results"][0].is<JsonObjectConst>()) {
      LOG_ERR("WTHR", "Place not found: %s", query.c_str());
      return false;
    }
    JsonObjectConst hit = doc["results"][0];
    forecast.lat = hit["latitude"] | 0.0f;
    forecast.lon = hit["longitude"] | 0.0f;
    forecast.place = hit["name"] | query.c_str();
  }
  forecast.query = query;
  forecast.located = true;
  forecast.locatedAt = now;
  return true;
}

bool download(Forecast& forecast, const time_t now) {
  char url[320];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=auto&forecast_days=%d",
           forecast.lat, forecast.lon, DAYS);
  JsonDocument doc;
  if (!fetchJson(url, doc) || !doc["current"].is<JsonObjectConst>()) return false;
  JsonObjectConst current = doc["current"];
  forecast.tempC = static_cast<int16_t>(lroundf(current["temperature_2m"] | 0.0f));
  forecast.code = current["weather_code"] | -1;
  forecast.humidity = current["relative_humidity_2m"] | 0;
  forecast.windKmh = static_cast<uint16_t>(lroundf(current["wind_speed_10m"] | 0.0f));
  JsonObjectConst daily = doc["daily"];
  for (int i = 0; i < DAYS; i++) {
    Day& day = forecast.days[i];
    snprintf(day.date, sizeof(day.date), "%s", daily["time"][i] | "");
    day.code = daily["weather_code"][i] | -1;
    day.maxC = static_cast<int16_t>(lroundf(daily["temperature_2m_max"][i] | 0.0f));
    day.minC = static_cast<int16_t>(lroundf(daily["temperature_2m_min"][i] | 0.0f));
    day.rainPct = daily["precipitation_probability_max"][i] | 0;
  }
  forecast.fetchedAt = now;
  return forecast.code >= 0;
}

}  // namespace

bool refreshBeforeSleep(Forecast& forecast) {
  if (!SETTINGS.weatherEnabled) return false;
  const time_t now = time(nullptr);
  if (now < MIN_VALID_EPOCH) return false;
  if (forecast.valid() && now - forecast.fetchedAt < MAX_AGE_S && now >= forecast.fetchedAt) return false;
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("WTHR", "Not enough heap for TLS; keeping the old forecast");
    return false;
  }
  if (!joinSavedNetwork()) {
    leaveNetwork();
    return false;
  }

  // While online anyway: keep this device's row on the server's Devices page current.
  device_link::hello();

  const std::string query = SETTINGS.weatherLocation;
  const bool relocate = !forecast.located || forecast.query != query ||
                        (query.empty() && now - forecast.locatedAt > RELOCATE_S);
  bool ok = !relocate || locate(forecast, query, now);
  ok = ok && download(forecast, now);
  leaveNetwork();
  if (!ok) return false;
  forecast.save();
  LOG_INF("WTHR", "Forecast for %s: %d C, code %d", forecast.place.c_str(), forecast.tempC, forecast.code);
  return true;
}

}  // namespace weather
