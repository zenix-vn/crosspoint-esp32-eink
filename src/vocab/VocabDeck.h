#pragma once

#include <HalStorage.h>
#include <VocabSrs.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Word lists and study progress for the vocabulary trainer (docs/vocabulary.md).
//
// A deck is /Vocab/<name>.tsv; its progress is /.crosspoint/vocab/<name>.srs,
// a header plus one fixed 16-byte record per card. Nothing proportional to the
// deck stays in RAM: cards and records are read from the card when needed, so
// a 20 000-word deck costs the C3 boards no more heap than a 20-word one.
namespace vocab {

constexpr char DECK_DIR[] = "/Vocab";
constexpr uint16_t MAX_CARDS = 20000;

struct Card {
  std::string word;
  std::string ipa;
  std::string meaning;
  std::string example;
};

struct Counts {
  uint16_t due = 0;      // studied cards due today or earlier
  uint16_t fresh = 0;    // never studied
  uint16_t learned = 0;  // studied at least once
};

// How well the deck is known, by current interval, and what falls due soon.
struct Mastery {
  uint16_t fresh = 0;     // never studied
  uint16_t learning = 0;  // interval under a week
  uint16_t familiar = 0;  // one to three weeks
  uint16_t mastered = 0;  // three weeks or more
  uint16_t dueToday = 0;
  uint16_t dueTomorrow = 0;
  uint16_t dueWeek = 0;  // due in the next seven days, tomorrow included
};

// The language a deck teaches (Settings > Vocabulary > Language). Values are
// persisted as the vocabLanguage setting.
enum class Language : uint8_t { English = 0, Chinese = 1 };
inline Language languageFromSetting(const uint8_t value) {
  return value == static_cast<uint8_t>(Language::Chinese) ? Language::Chinese : Language::English;
}

// A deck is Chinese when it says "# lang: zh" in its leading comments or its
// first word is written in Han characters; otherwise English.
Language deckLanguage(const std::string& name);
// Deck names (file names without .tsv) in /Vocab, sorted.
std::vector<std::string> listDecks();
// Only the decks teaching `language`.
std::vector<std::string> listDecks(Language language);
// Writes the built-in starter deck of `language` when /Vocab holds no deck of
// that language. Returns its name, or an empty string when there was already
// one or the write failed.
std::string ensureStarterDeck(Language language = Language::English);

class Deck {
 public:
  Deck() = default;
  ~Deck() { close(); }
  Deck(const Deck&) = delete;
  Deck& operator=(const Deck&) = delete;

  // Opens the deck, rebuilding its index when the .tsv changed since the last
  // open. Progress survives edits: cards are matched by word.
  bool open(const std::string& name);
  void close();
  bool isOpen() const { return opened; }
  const std::string& name() const { return deckName; }
  uint16_t cardCount() const { return count; }

  bool readCard(uint16_t index, Card& out);
  bool readState(uint16_t index, CardState& out);
  bool writeState(uint16_t index, const CardState& state);

  Counts counts(int32_t today);
  Mastery mastery(int32_t today);
  // Today's session: due cards (oldest first in deck order, at most maxReviews)
  // then up to `newLimit` new cards, as card indexes.
  bool buildSession(int32_t today, uint16_t newLimit, uint16_t maxReviews, std::vector<uint16_t>& out);
  // Up to `max` studied cards, for quiz questions and distractors.
  bool studiedCards(uint16_t max, std::vector<uint16_t>& out);

  // New cards already introduced today (the daily new-card budget).
  uint16_t newIntroducedToday(int32_t today) const;
  void noteNewIntroduced(int32_t today);

 private:
  bool rebuildIndex(uint32_t deckSize, uint32_t deckTime);
  bool readRecord(uint16_t index, uint8_t* record);
  bool writeHeader();

  std::string deckName;
  std::string tsvPath;
  std::string srsPath;
  HalFile tsv;
  HalFile srs;
  bool opened = false;
  uint16_t count = 0;
  int32_t newDay = -1;
  uint16_t newToday = 0;
  uint32_t deckSize = 0;
  uint32_t deckTime = 0;
};

// Splits one deck line into its columns. Accepts word<TAB>meaning,
// word<TAB>ipa<TAB>meaning and word<TAB>ipa<TAB>meaning<TAB>example. Returns
// false for comments, blank lines and lines with no meaning.
bool parseLine(const char* line, size_t len, Card& out);

constexpr int HISTORY_DAYS = 7;

// One day of study, across all decks.
struct DayLog {
  int32_t day = -1;        // -1 = empty slot
  uint16_t cards = 0;      // flash cards graded
  uint16_t fresh = 0;      // of which new words
  uint16_t reviews = 0;    // of which scheduled reviews (not new, not relearned)
  uint16_t lapses = 0;     // reviews graded Again
  uint16_t quiz = 0;       // quiz questions answered
  uint16_t quizRight = 0;  // of which right
};

// The last HISTORY_DAYS days summed, for the statistics screen.
struct WeekSummary {
  uint32_t cards = 0;
  uint32_t fresh = 0;
  uint32_t reviews = 0;
  uint32_t lapses = 0;
  uint32_t quiz = 0;
  uint32_t quizRight = 0;
  int activeDays = 0;
};

// Trainer-wide state: the chosen deck of each language, the study streak and
// recent days (both across languages).
struct TrainerState {
  std::string deck;    // English
  std::string deckZh;  // Chinese
  std::string& deckFor(Language language) { return language == Language::Chinese ? deckZh : deck; }
  const std::string& deckFor(Language language) const { return language == Language::Chinese ? deckZh : deck; }
  int32_t lastDay = -1;  // last day anything was studied
  uint16_t streak = 0;
  uint16_t best = 0;
  // Ring indexed by day modulo its size; a slot holding another day is stale.
  std::array<DayLog, HISTORY_DAYS> days{};

  void load();
  bool save() const;
  // Counts `today` as a study day for the streak.
  void noteStudied(int32_t today);
  void noteCard(int32_t today, bool fresh, bool review, bool lapse);
  void noteQuiz(int32_t today, bool right);
  // The streak as of today: 0 once a day has been missed.
  uint16_t currentStreak(int32_t today) const;
  // The log of `day`, or an empty one when it is not in the history.
  DayLog dayLog(int32_t day) const;
  WeekSummary week(int32_t today) const;
};

// A 0-100 rating of the last week (regular study and recall), -1 with no study.
int weekScore(const WeekSummary& week);

// Today as the trainer counts it: the local calendar day, or the last day it
// saw when the clock is not set (so due cards still come up).
int32_t studyDay(const TrainerState& state);

}  // namespace vocab
