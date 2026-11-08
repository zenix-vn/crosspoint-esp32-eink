#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <array>
#include <cstdint>
#include <ctime>
#include <string>

// Reading time per local day (last week) and per month (last year), the
// consecutive-day streak, finished books, each recent book's reading pace, and
// a battery drain anchor. Fed by finished reader sessions.
class ReadingStatsStore : public PersistableStore<ReadingStatsStore> {
 public:
  static constexpr int HISTORY_DAYS = 7;
  static constexpr int HISTORY_MONTHS = 12;
  static constexpr int PACE_BOOKS = 12;
  static constexpr int FINISHED_MEMORY = 32;

  struct Session {
    std::string path;
    time_t endEpoch = 0;
    uint32_t seconds = 0;
    uint16_t pages = 0;
    uint16_t startBp = 0;  // whole-book progress, basis points (0-10000)
    uint16_t endBp = 0;
  };

 private:
  struct Day {
    int32_t day = -1;  // local days since 1970-01-01, -1 = empty slot
    uint32_t seconds = 0;
    uint16_t pages = 0;
  };
  struct Month {
    int32_t month = -1;  // year * 12 + (month - 1), -1 = empty slot
    uint32_t seconds = 0;
    uint16_t finished = 0;
  };
  // Seconds spent per progress gained, so a book's remaining time can be estimated.
  struct Pace {
    uint32_t pathHash = 0;
    uint32_t seconds = 0;
    uint32_t progressBp = 0;
    int32_t lastDay = -1;
  };

  // Rings indexed by day / month modulo their size; a slot holding another key is stale.
  std::array<Day, HISTORY_DAYS> days{};
  std::array<Month, HISTORY_MONTHS> months{};
  std::array<Pace, PACE_BOOKS> paces{};
  std::array<uint32_t, FINISHED_MEMORY> finished{};  // path hashes already counted as finished
  uint8_t finishedNext = 0;
  int32_t streakLastDay = -1;
  uint16_t streak = 0;
  uint16_t best = 0;
  int64_t batteryEpoch = 0;  // 0 = no anchor (charging or never sampled)
  uint8_t batteryPercent = 0;

  ReadingStatsStore() = default;
  ~ReadingStatsStore() = default;

  friend class PersistableStore<ReadingStatsStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/reading_stats.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static uint32_t pathHash(const std::string& path);
  // Local calendar day / month of a UTC epoch in the configured timezone, -1 if unset.
  static int32_t localDay(time_t epoch);
  static int32_t localMonth(time_t epoch);
  // Today / this month from the RTC, -1 when the clock is unavailable or unset.
  static int32_t today();
  static int32_t thisMonth();

  // Credits a finished session to the day and month it ended. Does not persist.
  void addSession(const Session& session);
  uint32_t secondsOn(int32_t day) const;
  uint16_t pagesOn(int32_t day) const;
  uint32_t secondsInMonth(int32_t month) const;
  int finishedInMonth(int32_t month) const;
  // Totals of the months of `year` still in the history.
  uint32_t secondsInYear(int year) const;
  int finishedInYear(int year) const;
  // Consecutive reading days ending today, or yesterday while today is unread.
  int currentStreak(int32_t today) const;
  int bestStreak() const { return best; }
  // Estimated reading seconds to finish the book from progressBp, or -1 when
  // too little of it has been read to know the pace.
  int32_t secondsLeft(const std::string& path, int progressBp) const;
  // Estimated reading seconds for `progressBp` more of the book, -1 if unknown.
  int32_t secondsForProgress(const std::string& path, int progressBp) const;
  // Seconds spent reading the book while it is among the PACE_BOOKS most
  // recently read with progress; -1 when it is not tracked.
  int32_t secondsReading(const std::string& path) const;
  // Whether the book is among the last FINISHED_MEMORY books read to the end.
  bool wasFinished(const std::string& path) const;
  // Local day the book was last read with progress, -1 when not tracked.
  int32_t lastReadDay(const std::string& path) const;

  // Records a battery sample; returns true when the anchor changed (persist then).
  bool noteBattery(time_t epoch, int percent, bool charging);
  // Days until empty at the drain rate since the last charge, or -1 if unknown.
  int batteryDaysLeft(time_t epoch, int percent) const;
};

#define READING_STATS ReadingStatsStore::getInstance()
