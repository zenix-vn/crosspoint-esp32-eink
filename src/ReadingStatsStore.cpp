#include "ReadingStatsStore.h"

#include <HalClock.h>
#include <Logging.h>

#include <algorithm>

namespace {

// RTC before this year means the clock was never set.
constexpr int MIN_VALID_YEAR = 2024;
// Progress a book needs before its pace is trusted (1%).
constexpr uint32_t MIN_PACE_BP = 100;
// Battery drain needs this much drop over this long before it is extrapolated.
constexpr int MIN_BATTERY_DROP = 3;
constexpr int64_t MIN_BATTERY_SECONDS = 6 * 3600;
// Progress at which a session counts its book as finished.
constexpr uint16_t FINISHED_BP = 9900;

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
int32_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<int32_t>(dayOfEra) - 719468;
}

bool valid(const struct tm& t) { return t.tm_year + 1900 >= MIN_VALID_YEAR; }

int32_t dayOf(const struct tm& t) {
  if (!valid(t)) return -1;
  return daysFromCivil(t.tm_year + 1900, static_cast<unsigned>(t.tm_mon + 1), static_cast<unsigned>(t.tm_mday));
}

int32_t monthOf(const struct tm& t) { return valid(t) ? (t.tm_year + 1900) * 12 + t.tm_mon : -1; }

bool localTm(const time_t epoch, struct tm& out) { return epoch > 0 && localtime_r(&epoch, &out); }

}  // namespace

uint32_t ReadingStatsStore::pathHash(const std::string& path) {
  // FNV-1a; 0 is reserved for empty slots.
  uint32_t hash = 2166136261u;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash == 0 ? 1 : hash;
}

int32_t ReadingStatsStore::localDay(const time_t epoch) {
  struct tm t {};
  return localTm(epoch, t) ? dayOf(t) : -1;
}

int32_t ReadingStatsStore::localMonth(const time_t epoch) {
  struct tm t {};
  return localTm(epoch, t) ? monthOf(t) : -1;
}

int32_t ReadingStatsStore::today() {
  struct tm t {};
  return halClock.localTime(t) ? dayOf(t) : -1;
}

int32_t ReadingStatsStore::thisMonth() {
  struct tm t {};
  return halClock.localTime(t) ? monthOf(t) : -1;
}

void ReadingStatsStore::toJson(JsonDocument& doc) const {
  JsonArray dayArr = doc["days"].to<JsonArray>();
  for (const Day& d : days) {
    if (d.day < 0) continue;
    JsonObject obj = dayArr.add<JsonObject>();
    obj["day"] = d.day;
    obj["seconds"] = d.seconds;
    obj["pages"] = d.pages;
  }
  JsonArray monthArr = doc["months"].to<JsonArray>();
  for (const Month& m : months) {
    if (m.month < 0) continue;
    JsonObject obj = monthArr.add<JsonObject>();
    obj["month"] = m.month;
    obj["seconds"] = m.seconds;
    obj["finished"] = m.finished;
  }
  JsonArray paceArr = doc["paces"].to<JsonArray>();
  for (const Pace& p : paces) {
    if (p.pathHash == 0) continue;
    JsonObject obj = paceArr.add<JsonObject>();
    obj["hash"] = p.pathHash;
    obj["seconds"] = p.seconds;
    obj["bp"] = p.progressBp;
    obj["day"] = p.lastDay;
  }
  JsonArray finishedArr = doc["finished"].to<JsonArray>();
  for (const uint32_t hash : finished) {
    if (hash != 0) finishedArr.add(hash);
  }
  doc["streakLastDay"] = streakLastDay;
  doc["streak"] = streak;
  doc["best"] = best;
  doc["batteryEpoch"] = batteryEpoch;
  doc["batteryPercent"] = batteryPercent;
}

bool ReadingStatsStore::fromJson(JsonVariantConst doc) {
  days = {};
  for (JsonObjectConst obj : doc["days"].as<JsonArrayConst>()) {
    const int32_t day = obj["day"] | -1;
    if (day < 0) continue;
    days[day % HISTORY_DAYS] = Day{day, obj["seconds"] | 0u, static_cast<uint16_t>(obj["pages"] | 0u)};
  }
  months = {};
  for (JsonObjectConst obj : doc["months"].as<JsonArrayConst>()) {
    const int32_t month = obj["month"] | -1;
    if (month < 0) continue;
    months[month % HISTORY_MONTHS] = Month{month, obj["seconds"] | 0u, static_cast<uint16_t>(obj["finished"] | 0u)};
  }
  paces = {};
  size_t paceIndex = 0;
  for (JsonObjectConst obj : doc["paces"].as<JsonArrayConst>()) {
    if (paceIndex >= paces.size()) break;
    paces[paceIndex++] = Pace{obj["hash"] | 0u, obj["seconds"] | 0u, obj["bp"] | 0u, obj["day"] | -1};
  }
  finished = {};
  finishedNext = 0;
  for (JsonVariantConst hash : doc["finished"].as<JsonArrayConst>()) {
    if (finishedNext >= finished.size()) break;
    finished[finishedNext++] = hash | 0u;
  }
  finishedNext %= FINISHED_MEMORY;
  streakLastDay = doc["streakLastDay"] | -1;
  streak = doc["streak"] | 0u;
  best = doc["best"] | 0u;
  batteryEpoch = doc["batteryEpoch"] | static_cast<int64_t>(0);
  batteryPercent = doc["batteryPercent"] | 0u;
  return true;
}

void ReadingStatsStore::addSession(const Session& session) {
  const int32_t day = localDay(session.endEpoch);
  const int32_t month = localMonth(session.endEpoch);
  if (day < 0 || month < 0) return;

  Day& daySlot = days[day % HISTORY_DAYS];
  if (daySlot.day != day) daySlot = Day{day, 0, 0};
  daySlot.seconds += session.seconds;
  daySlot.pages = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, daySlot.pages + session.pages));

  Month& monthSlot = months[month % HISTORY_MONTHS];
  if (monthSlot.month != month) monthSlot = Month{month, 0, 0};
  monthSlot.seconds += session.seconds;

  const uint32_t hash = pathHash(session.path);
  if (session.endBp > session.startBp) {
    // Reuse the book's slot, else replace the least recently read one.
    Pace* slot = std::find_if(paces.begin(), paces.end(), [hash](const Pace& p) { return p.pathHash == hash; });
    if (slot == paces.end()) {
      slot = std::min_element(paces.begin(), paces.end(),
                              [](const Pace& a, const Pace& b) { return a.lastDay < b.lastDay; });
      *slot = Pace{hash, 0, 0, day};
    }
    slot->seconds += session.seconds;
    slot->progressBp += session.endBp - session.startBp;
    slot->lastDay = day;
  }

  if (session.endBp >= FINISHED_BP && std::find(finished.begin(), finished.end(), hash) == finished.end()) {
    finished[finishedNext] = hash;
    finishedNext = static_cast<uint8_t>((finishedNext + 1) % FINISHED_MEMORY);
    ++monthSlot.finished;
  }

  if (day <= streakLastDay) return;  // same day, or the clock moved backwards
  streak = day == streakLastDay + 1 ? static_cast<uint16_t>(streak + 1) : 1;
  streakLastDay = day;
  best = std::max(best, streak);
  LOG_DBG("STATS", "Reading streak %u (best %u)", streak, best);
}

uint32_t ReadingStatsStore::secondsOn(const int32_t day) const {
  if (day < 0) return 0;
  const Day& slot = days[day % HISTORY_DAYS];
  return slot.day == day ? slot.seconds : 0;
}

uint16_t ReadingStatsStore::pagesOn(const int32_t day) const {
  if (day < 0) return 0;
  const Day& slot = days[day % HISTORY_DAYS];
  return slot.day == day ? slot.pages : 0;
}

uint32_t ReadingStatsStore::secondsInMonth(const int32_t month) const {
  if (month < 0) return 0;
  const Month& slot = months[month % HISTORY_MONTHS];
  return slot.month == month ? slot.seconds : 0;
}

int ReadingStatsStore::finishedInMonth(const int32_t month) const {
  if (month < 0) return 0;
  const Month& slot = months[month % HISTORY_MONTHS];
  return slot.month == month ? slot.finished : 0;
}

uint32_t ReadingStatsStore::secondsInYear(const int year) const {
  uint32_t total = 0;
  for (const Month& m : months) {
    if (m.month >= 0 && m.month / 12 == year) total += m.seconds;
  }
  return total;
}

int ReadingStatsStore::finishedInYear(const int year) const {
  int total = 0;
  for (const Month& m : months) {
    if (m.month >= 0 && m.month / 12 == year) total += m.finished;
  }
  return total;
}

int ReadingStatsStore::currentStreak(const int32_t today) const {
  if (today < 0 || streakLastDay < 0) return 0;
  return today - streakLastDay <= 1 ? streak : 0;
}

int32_t ReadingStatsStore::secondsLeft(const std::string& path, const int progressBp) const {
  if (progressBp < 0) return -1;
  return secondsForProgress(path, std::max(0, 10000 - progressBp));
}

int32_t ReadingStatsStore::secondsForProgress(const std::string& path, const int progressBp) const {
  const uint32_t hash = pathHash(path);
  const auto slot = std::find_if(paces.begin(), paces.end(), [hash](const Pace& p) { return p.pathHash == hash; });
  if (slot == paces.end() || slot->progressBp < MIN_PACE_BP || progressBp < 0) return -1;
  return static_cast<int32_t>(static_cast<int64_t>(progressBp) * slot->seconds / slot->progressBp);
}

int32_t ReadingStatsStore::secondsReading(const std::string& path) const {
  const uint32_t hash = pathHash(path);
  if (hash == 0) return -1;  // 0 marks an empty slot
  const auto slot = std::find_if(paces.begin(), paces.end(), [hash](const Pace& p) { return p.pathHash == hash; });
  return slot == paces.end() ? -1 : static_cast<int32_t>(std::min<uint32_t>(slot->seconds, INT32_MAX));
}

int32_t ReadingStatsStore::lastReadDay(const std::string& path) const {
  const uint32_t hash = pathHash(path);
  if (hash == 0) return -1;
  const auto slot = std::find_if(paces.begin(), paces.end(), [hash](const Pace& p) { return p.pathHash == hash; });
  return slot == paces.end() ? -1 : slot->lastDay;
}

bool ReadingStatsStore::wasFinished(const std::string& path) const {
  const uint32_t hash = pathHash(path);
  return hash != 0 && std::find(finished.begin(), finished.end(), hash) != finished.end();
}

bool ReadingStatsStore::noteBattery(const time_t epoch, const int percent, const bool charging) {
  if (charging) {
    if (batteryEpoch == 0) return false;
    batteryEpoch = 0;
    return true;
  }
  // First sample after a charge (or a level that rose without one) starts a new anchor.
  if (epoch <= 0 || (batteryEpoch != 0 && percent <= batteryPercent)) return false;
  batteryEpoch = epoch;
  batteryPercent = static_cast<uint8_t>(std::clamp(percent, 0, 100));
  return true;
}

int ReadingStatsStore::batteryDaysLeft(const time_t epoch, const int percent) const {
  if (batteryEpoch == 0 || epoch <= batteryEpoch) return -1;
  const int drop = batteryPercent - percent;
  const int64_t elapsed = static_cast<int64_t>(epoch) - batteryEpoch;
  if (drop < MIN_BATTERY_DROP || elapsed < MIN_BATTERY_SECONDS) return -1;
  return static_cast<int>(static_cast<int64_t>(percent) * elapsed / drop / 86400);
}
