#pragma once

// Spaced-repetition scheduling for the vocabulary trainer: an SM-2 variant
// with four grades (see docs/vocabulary.md). Pure and allocation-free so it is
// host-testable (test/vocab_srs).

#include <algorithm>
#include <cstdint>

namespace vocab {

enum class Grade : uint8_t { Again = 0, Hard = 1, Good = 2, Easy = 3 };

constexpr int32_t NEW_CARD = -1;  // dueDay of a card never studied
constexpr uint16_t EASE_MIN = 130;
constexpr uint16_t EASE_MAX = 300;
constexpr uint16_t EASE_START = 250;
constexpr uint16_t INTERVAL_MAX = 3650;

struct CardState {
  int32_t dueDay = NEW_CARD;
  uint16_t interval = 0;
  uint16_t easeX100 = EASE_START;
};

struct Review {
  CardState next;
  bool again;  // show the card again later in this session
};

// `learning` marks a card already failed in this session: it is graded like a
// new card, so getting it right schedules it for tomorrow, not after its old
// interval.
inline Review schedule(const CardState& card, const Grade grade, const int32_t today, const bool learning = false) {
  Review r{card, false};
  const bool fresh = learning || card.dueDay == NEW_CARD || card.interval == 0;
  const uint32_t ease = card.easeX100 == 0 ? EASE_START : card.easeX100;
  uint32_t interval = card.interval;
  int32_t easeDelta = 0;

  switch (grade) {
    case Grade::Again:
      r.again = true;
      interval = 1;
      if (!fresh) easeDelta = -20;
      break;
    case Grade::Hard:
      interval = fresh ? 1 : std::max<uint32_t>(interval + 1, interval * 12 / 10);
      if (!fresh) easeDelta = -15;
      break;
    case Grade::Good:
      interval = fresh ? 1 : std::max<uint32_t>(interval + 1, interval * ease / 100);
      break;
    case Grade::Easy:
      interval = fresh ? 4 : std::max<uint32_t>(interval + 1, interval * ease * 13 / 1000);
      easeDelta = 15;
      break;
  }

  r.next.interval = static_cast<uint16_t>(std::min<uint32_t>(interval, INTERVAL_MAX));
  r.next.easeX100 =
      static_cast<uint16_t>(std::clamp<int32_t>(static_cast<int32_t>(ease) + easeDelta, EASE_MIN, EASE_MAX));
  // A failed card is due today so a later session the same day still has it.
  r.next.dueDay = r.again ? today : today + r.next.interval;
  return r;
}

// The interval a grade would give, for the label under each grade button.
inline uint16_t previewInterval(const CardState& card, const Grade grade, const bool learning = false) {
  return grade == Grade::Again ? 0 : schedule(card, grade, 0, learning).next.interval;
}

// FNV-1a over the word: identifies a card across deck edits.
inline uint32_t wordHash(const char* word, const size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= static_cast<unsigned char>(word[i]);
    h *= 16777619u;
  }
  return h;
}

}  // namespace vocab
