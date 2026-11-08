#include <gtest/gtest.h>

#include <cstring>

#include "VocabSrs.h"

using vocab::CardState;
using vocab::Grade;
using vocab::schedule;

TEST(VocabSrs, NewCardIntervals) {
  const CardState fresh;
  EXPECT_EQ(schedule(fresh, Grade::Hard, 100).next.dueDay, 101);
  EXPECT_EQ(schedule(fresh, Grade::Good, 100).next.dueDay, 101);
  EXPECT_EQ(schedule(fresh, Grade::Easy, 100).next.dueDay, 104);
  const auto again = schedule(fresh, Grade::Again, 100);
  EXPECT_TRUE(again.again);
  EXPECT_EQ(again.next.dueDay, 100);  // still due today
  EXPECT_EQ(again.next.easeX100, vocab::EASE_START);  // a new card keeps its ease
}

TEST(VocabSrs, GoodGrowsByEaseAndAlwaysAdvances) {
  CardState card{50, 10, 250};
  const auto good = schedule(card, Grade::Good, 60).next;
  EXPECT_EQ(good.interval, 25);
  EXPECT_EQ(good.dueDay, 85);

  CardState tiny{50, 1, 130};
  EXPECT_EQ(schedule(tiny, Grade::Hard, 60).next.interval, 2);  // 1 * 1.2 rounds down, +1 wins
  EXPECT_EQ(schedule(tiny, Grade::Good, 60).next.interval, 2);
}

TEST(VocabSrs, EaseMovesAndIsClamped) {
  CardState card{50, 10, 250};
  EXPECT_EQ(schedule(card, Grade::Again, 60).next.easeX100, 230);
  EXPECT_EQ(schedule(card, Grade::Hard, 60).next.easeX100, 235);
  EXPECT_EQ(schedule(card, Grade::Easy, 60).next.easeX100, 265);

  CardState floor{50, 10, 135};
  EXPECT_EQ(schedule(floor, Grade::Again, 60).next.easeX100, vocab::EASE_MIN);
  CardState ceiling{50, 10, 295};
  EXPECT_EQ(schedule(ceiling, Grade::Easy, 60).next.easeX100, vocab::EASE_MAX);
}

TEST(VocabSrs, LapseResetsAndRelearningIsLikeNew) {
  CardState card{50, 40, 250};
  const auto lapse = schedule(card, Grade::Again, 90);
  EXPECT_TRUE(lapse.again);
  EXPECT_EQ(lapse.next.interval, 1);
  // Right after failing, Good in the same session means "see you tomorrow".
  EXPECT_EQ(schedule(lapse.next, Grade::Good, 90, true).next.dueDay, 91);
}

TEST(VocabSrs, IntervalIsCapped) {
  CardState card{0, 3000, 300};
  EXPECT_EQ(schedule(card, Grade::Easy, 10).next.interval, vocab::INTERVAL_MAX);
}

TEST(VocabSrs, WordHashIsStable) {
  const char* word = "achieve";
  EXPECT_EQ(vocab::wordHash(word, strlen(word)), vocab::wordHash("achieve", 7));
  EXPECT_NE(vocab::wordHash("achieve", 7), vocab::wordHash("achieved", 8));
}
