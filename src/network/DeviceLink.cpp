#include "DeviceLink.h"

#include <BoardConfig.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "util/PluginHttp.h"

namespace device_link {

namespace {

#ifdef CROSSPOINT_INBOX_URL
constexpr char SERVER_URL[] = CROSSPOINT_INBOX_URL;
#else
constexpr char SERVER_URL[] = "";
#endif

constexpr char NVS_NAMESPACE[] = "crosspoint";
constexpr char NVS_SECRET_KEY[] = "devsecret";

std::string hex(const uint8_t* bytes, const size_t len) {
  std::string out;
  out.reserve(len * 2);
  char pair[3];
  for (size_t i = 0; i < len; i++) {
    snprintf(pair, sizeof(pair), "%02x", bytes[i]);
    out += pair;
  }
  return out;
}

const std::string& deviceId() {
  static std::string id;
  if (id.empty()) {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    id = hex(mac, sizeof(mac));
  }
  return id;
}

const std::string& deviceSecret() {
  static std::string secret;
  if (!secret.empty()) return secret;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    secret = prefs.getString(NVS_SECRET_KEY, "").c_str();
    if (secret.size() != 32) {
      uint8_t bytes[16];
      esp_fill_random(bytes, sizeof(bytes));
      secret = hex(bytes, sizeof(bytes));
      prefs.putString(NVS_SECRET_KEY, secret.c_str());
    }
    prefs.end();
  }
  return secret;
}

std::string deviceName() { return SETTINGS.deviceName[0] ? SETTINGS.deviceName : tr(STR_DEVICE_NAME_DEFAULT); }

}  // namespace

bool available() { return SERVER_URL[0] != '\0'; }

const char* serverUrl() { return SERVER_URL; }

std::vector<HttpDownloader::Header> headers() {
  std::vector<HttpDownloader::Header> out = {
      {"X-Device-Id", deviceId()},
      {"X-Device-Secret", deviceSecret()},
      // Percent-encoded: header values are ASCII, device names may not be.
      {"X-Device-Name", pluginhttp::urlEncodeQuery(deviceName())},
      {"X-Device-Model", BoardConfig::ACTIVE.name},
      {"X-Device-Firmware", CROSSPOINT_VERSION},
      {"X-Device-Battery", std::to_string(powerManager.getBatteryPercentage())},
  };
  // The account link key (or the older shared inbox key).
  if (SETTINGS.inboxKey[0]) out.emplace_back("X-Inbox-Key", SETTINGS.inboxKey);
  return out;
}

std::string formFields() {
  std::string out = "device_id=" + deviceId() + "&device_secret=" + deviceSecret();
  if (SETTINGS.inboxKey[0]) out += "&key=" + pluginhttp::urlEncodeQuery(SETTINGS.inboxKey);
  return out;
}

void hello() {
  if (!available()) return;
  HttpDownloader::FetchOptions options;
  options.headers = headers();
  std::string ignored;
  const bool ok = HttpDownloader::fetchUrl(
      std::string(SERVER_URL) + "?action=hello",
      [&ignored](const uint8_t* data, const size_t len) {
        if (ignored.size() < 256) ignored.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      options);
  if (!ok) LOG_ERR("DEV", "hello failed (status %d)", HttpDownloader::lastStatus());
}

}  // namespace device_link
