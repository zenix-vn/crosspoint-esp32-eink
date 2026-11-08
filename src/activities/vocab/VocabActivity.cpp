#include "VocabActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "HanFont.h"
#include "HapticFeedback.h"
#include "IpaFont.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

#ifndef OMIT_FONTS
constexpr int WORD_FONT = NOTOSANS_18_FONT_ID;
#else
constexpr int WORD_FONT = UI_12_FONT_ID;
#endif

// Chinese words are drawn at twice the font's size (8.8 fixed point).
constexpr int HAN_WORD_SCALE = 512;
// Overstrike offset in pixels that makes the magnified strokes bold.
constexpr int HAN_BOLD_PX = 2;

constexpr uint16_t MAX_REVIEWS_PER_SESSION = 200;
constexpr size_t QUIZ_LENGTH = 10;
constexpr uint16_t QUIZ_POOL = 400;
// The word list is chosen in Settings > Vocabulary.
constexpr int MENU_ITEMS = 3;  // Study, Quiz, Statistics
constexpr int MENU_STATS = 2;
// More cards due than this is a backlog worth clearing before new words.
constexpr int BACKLOG = 50;
// Below this percent of reviews remembered, the new words come too fast.
constexpr int LOW_RECALL = 70;
constexpr int ROW_HEIGHT = 64;
constexpr int GAP = 12;
constexpr int RADIUS = 12;

bool contains(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

uint32_t randomBelow(const uint32_t n) { return n == 0 ? 0 : esp_random() % n; }

const char* gradeLabel(const int grade) {
  switch (grade) {
    case 0:
      return tr(STR_VOCAB_AGAIN);
    case 1:
      return tr(STR_VOCAB_HARD);
    case 2:
      return tr(STR_VOCAB_GOOD);
    default:
      return tr(STR_VOCAB_EASY);
  }
}

}  // namespace

// --- lifecycle ---------------------------------------------------------------

void VocabActivity::onEnter() {
  Activity::onEnter();
  state.load();
  today = vocab::studyDay(state);
  language = vocab::languageFromSetting(SETTINGS.vocabLanguage);
  wordFont = WORD_FONT;
  wordStyle = EpdFontFamily::BOLD;
  wordScale = 256;
  const std::string starter = vocab::ensureStarterDeck(language);
  std::string name = state.deckFor(language);
  const auto decks = vocab::listDecks(language);
  if (name.empty() || std::find(decks.begin(), decks.end(), name) == decks.end()) {
    name = !starter.empty() ? starter : (decks.empty() ? std::string() : decks.front());
  }
  if (name.empty()) {
    message = language == vocab::Language::Chinese ? tr(STR_VOCAB_NO_DECKS_ZH) : tr(STR_VOCAB_NO_DECKS);
  } else {
    openDeck(name);
  }
  requestUpdate();
}

void VocabActivity::onExit() {
  if (studied > 0) state.save();
  deck.close();
  Activity::onExit();
}

void VocabActivity::openDeck(const std::string& name) {
  if (!deck.open(name)) {
    message = tr(STR_VOCAB_DECK_FAILED);
    return;
  }
  message.clear();
  if (state.deckFor(language) != name) {
    state.deckFor(language) = name;
    state.save();
  }
  if (language == vocab::Language::Chinese) chooseHanFont();
  refreshCounts();
}

// The word is drawn with the reader's SD font when it has Han characters (it
// covers any deck, and also backs the UI sizes, so meanings quoting characters
// render); otherwise with the built-in Han font, which covers the HSK decks.
void VocabActivity::chooseHanFont() {
  constexpr uint32_t PROBE = 0x6211;  // 我
  // SD fonts load when a book or Text settings opens; the trainer can come
  // first after boot, so load the chosen family (and its UI fallback) here.
  sdFontSystem.ensureLoaded(renderer);
  // Han glyphs are drawn regular: a synthesized bold blurs the strokes on
  // e-ink, and for an SD font costs a second face and glyph cache.
  wordStyle = EpdFontFamily::REGULAR;
  wordScale = HAN_WORD_SCALE;
  const int readerFont = SETTINGS.getReaderFontId();
  const auto& fonts = renderer.getFontMap();
  const auto it = fonts.find(readerFont);
  wordFont = it != fonts.end() && it->second.hasCodepoint(PROBE) ? readerFont : HAN_18_FONT_ID;
  LOG_INF("VOCAB", "Han word font %d (%s)", wordFont, wordFont == readerFont ? "reader" : "built-in");
}

// --- word drawing ------------------------------------------------------------

int VocabActivity::wordLineHeight() const {
  return renderer.getLineHeight(wordFont) * wordScale / 256;
}

int VocabActivity::wordWidth(const std::string& text) const {
  if (wordScale == 256) return renderer.getTextWidth(wordFont, text.c_str(), wordStyle);
  int w = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) {
    int32_t advanceFP = 0;
    int top = 0;
    if (renderer.getCodepointMetrics(wordFont, cp, wordStyle, advanceFP, top)) {
      w += (advanceFP * wordScale / 256 + 8) >> 4;  // 12.4 fixed point to pixels
    }
  }
  return w + HAN_BOLD_PX;
}

std::string VocabActivity::fitWord(const std::string& text, const int maxWidth) const {
  if (wordScale == 256) return renderer.truncatedText(wordFont, text.c_str(), maxWidth, wordStyle);
  std::string out;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text.c_str());
  const unsigned char* start = p;
  while (utf8NextCodepoint(&p)) {
    std::string candidate(reinterpret_cast<const char*>(start), p - start);
    if (wordWidth(candidate) > maxWidth) break;
    out = std::move(candidate);
  }
  return out;
}

// Han text has no spaces, so lines break between characters.
std::vector<std::string> VocabActivity::wrapWord(const std::string& text, const int maxWidth,
                                                 const int maxLines) const {
  if (wordScale == 256) return renderer.wrappedText(wordFont, text.c_str(), maxWidth, maxLines, wordStyle);
  std::vector<std::string> lines;
  const char* rest = text.c_str();
  while (*rest && static_cast<int>(lines.size()) < maxLines) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(rest);
    const char* fitEnd = rest;
    while (utf8NextCodepoint(&p)) {
      const char* end = reinterpret_cast<const char*>(p);
      // Always take at least one character so a narrow box cannot loop forever.
      if (fitEnd != rest && wordWidth(std::string(rest, end - rest)) > maxWidth) break;
      fitEnd = end;
    }
    if (fitEnd == rest) break;
    lines.emplace_back(rest, fitEnd - rest);
    rest = fitEnd;
  }
  return lines;
}

void VocabActivity::drawWord(const int x, const int y, const std::string& text) const {
  if (wordScale == 256) {
    renderer.drawText(wordFont, x, y, text.c_str(), true, wordStyle);
    return;
  }
  const int baseline = y + renderer.getFontAscenderSize(wordFont) * wordScale / 256;
  int cx = x;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) {
    int advance = 0;
    for (int dx = 0; dx <= HAN_BOLD_PX; dx++) {
      advance = renderer.drawScaledCodepoint(wordFont, cp, wordStyle, cx + dx, baseline, wordScale);
    }
    cx += advance;
  }
}

void VocabActivity::drawWordCentered(const int x, const int width, const int y, const std::string& text) const {
  drawWord(x + (width - wordWidth(text)) / 2, y, text);
}

void VocabActivity::refreshCounts() { counts = deck.isOpen() ? deck.counts(today) : vocab::Counts{}; }

// --- study session -----------------------------------------------------------

void VocabActivity::startSession() {
  const uint16_t introduced = deck.newIntroducedToday(today);
  const uint16_t budget = SETTINGS.vocabNewPerDay > introduced ? SETTINGS.vocabNewPerDay - introduced : 0;
  std::vector<uint16_t> cards;
  if (!deck.buildSession(today, budget, MAX_REVIEWS_PER_SESSION, cards) || cards.empty()) {
    screen = Screen::Done;
    lastWasQuiz = false;
    requestUpdate();
    return;
  }
  queue.clear();
  queue.reserve(cards.size() + 8);
  for (const uint16_t c : cards) queue.push_back(QueueItem{c, false});
  position = 0;
  studied = 0;
  screen = Screen::Card;
  loadCurrentCard();
  requestUpdate();
}

void VocabActivity::loadCurrentCard() {
  revealed = false;
  selected = static_cast<int>(vocab::Grade::Good);
  const QueueItem& item = queue[position];
  if (!deck.readCard(item.card, card) || !deck.readState(item.card, cardState)) {
    card = vocab::Card{"?", "", tr(STR_VOCAB_DECK_FAILED), ""};
    cardState = vocab::CardState{};
  }
}

void VocabActivity::grade(const vocab::Grade g) {
  const QueueItem item = queue[position];
  const bool wasNew = cardState.dueDay == vocab::NEW_CARD && !item.learning;
  const bool wasReview = !wasNew && !item.learning;
  const auto review = vocab::schedule(cardState, g, today, item.learning);
  deck.writeState(item.card, review.next);
  if (wasNew) deck.noteNewIntroduced(today);
  if (review.again) queue.push_back(QueueItem{item.card, true});
  studied++;
  state.noteStudied(today);
  state.noteCard(today, wasNew, wasReview, wasReview && g == vocab::Grade::Again);
  haptic_feedback::touchAction();

  position++;
  if (position >= queue.size()) {
    finishSession();
    return;
  }
  loadCurrentCard();
  requestUpdate();
}

void VocabActivity::finishSession() {
  state.save();
  lastWasQuiz = false;
  screen = Screen::Done;
  refreshCounts();
  requestUpdate();
}

// --- quiz --------------------------------------------------------------------

void VocabActivity::startQuiz() {
  std::vector<uint16_t> pool;
  deck.studiedCards(QUIZ_POOL, pool);
  // Too few studied words for four distinct choices: draw from the whole deck.
  if (pool.size() < 4) {
    pool.clear();
    for (uint16_t i = 0; i < deck.cardCount() && pool.size() < QUIZ_POOL; i++) pool.push_back(i);
  }
  quiz.clear();
  if (pool.size() < 4) {
    message = tr(STR_VOCAB_QUIZ_TOO_FEW);
    requestUpdate();
    return;
  }
  // Questions: distinct cards in random order.
  for (size_t i = pool.size() - 1; i > 0; i--) std::swap(pool[i], pool[randomBelow(static_cast<uint32_t>(i + 1))]);
  const size_t questions = std::min(QUIZ_LENGTH, pool.size());
  for (size_t q = 0; q < questions; q++) {
    QuizQuestion question{};
    question.card = pool[q];
    question.correct = static_cast<uint8_t>(randomBelow(4));
    for (uint8_t slot = 0; slot < 4; slot++) {
      if (slot == question.correct) {
        question.options[slot] = question.card;
        continue;
      }
      // A distractor: any other card not already among the options.
      uint16_t pick = question.card;
      for (int attempt = 0; attempt < 16; attempt++) {
        pick = pool[randomBelow(static_cast<uint32_t>(pool.size()))];
        bool used = pick == question.card;
        for (uint8_t k = 0; k < slot && !used; k++) used = question.options[k] == pick;
        if (!used) break;
      }
      question.options[slot] = pick;
    }
    quiz.push_back(question);
  }
  quizPosition = 0;
  quizCorrect = 0;
  studied = 0;
  message.clear();
  screen = Screen::Quiz;
  loadQuizQuestion();
  requestUpdate();
}

void VocabActivity::loadQuizQuestion() {
  answered = -1;
  selected = 0;
  const QuizQuestion& q = quiz[quizPosition];
  if (!deck.readCard(q.card, card)) card = vocab::Card{"?", "", "", ""};
  optionText.assign(4, std::string());
  vocab::Card other;
  for (int i = 0; i < 4; i++) {
    optionText[i] = i == q.correct ? card.meaning : (deck.readCard(q.options[i], other) ? other.meaning : "?");
  }
}

void VocabActivity::answerQuiz(const int option) {
  if (answered >= 0 || option < 0 || option > 3) return;
  answered = option;
  const QuizQuestion& q = quiz[quizPosition];
  haptic_feedback::touchAction();
  if (option == q.correct) {
    quizCorrect++;
  } else {
    // A miss puts the word back into today's reviews.
    vocab::CardState s;
    if (deck.readState(q.card, s) && s.dueDay != vocab::NEW_CARD) {
      deck.writeState(q.card, vocab::schedule(s, vocab::Grade::Again, today).next);
    }
  }
  studied++;
  state.noteStudied(today);
  state.noteQuiz(today, option == q.correct);
  requestUpdate();
}

// --- statistics --------------------------------------------------------------

void VocabActivity::openStats() {
  const vocab::Mastery m = deck.mastery(today);
  const vocab::WeekSummary week = state.week(today);
  stats = DesktopHomeUi::VocabStats{};
  stats.today = today;
  stats.score = vocab::weekScore(week);
  stats.activeDays = week.activeDays;
  stats.streak = state.currentStreak(today);
  stats.best = state.best;
  stats.weekCards = static_cast<int>(week.cards);
  stats.weekNew = static_cast<int>(week.fresh);
  if (week.reviews > 0) stats.recall = static_cast<int>((week.reviews - week.lapses) * 100 / week.reviews);
  if (week.quiz > 0) stats.quiz = static_cast<int>(week.quizRight * 100 / week.quiz);
  stats.fresh = m.fresh;
  stats.learning = m.learning;
  stats.familiar = m.familiar;
  stats.mastered = m.mastered;
  stats.dueToday = m.dueToday;
  stats.dueTomorrow = m.dueTomorrow;
  stats.dueWeek = m.dueWeek;
  for (int i = 0; i < DesktopHomeUi::VocabStats::DAYS; i++) {
    const vocab::DayLog log = state.dayLog(today - (DesktopHomeUi::VocabStats::DAYS - 1) + i);
    stats.dayCards[i] = log.cards;
    stats.dayNew[i] = log.fresh;
  }

  // One piece of advice, the most pressing first.
  if (week.activeDays == 0) {
    stats.tip = tr(STR_VOCAB_TIP_START);
  } else if (m.dueToday > BACKLOG) {
    stats.tip = tr(STR_VOCAB_TIP_BACKLOG);
  } else if (stats.recall >= 0 && stats.recall < LOW_RECALL) {
    stats.tip = tr(STR_VOCAB_TIP_RECALL);
  } else if (week.activeDays < vocab::HISTORY_DAYS - 2) {
    stats.tip = tr(STR_VOCAB_TIP_REGULAR);
  } else if (week.quiz == 0) {
    stats.tip = tr(STR_VOCAB_TIP_QUIZ);
  } else {
    stats.tip = tr(STR_VOCAB_TIP_GREAT);
  }
  screen = Screen::Stats;
  requestUpdate();
}

// --- input -------------------------------------------------------------------

void VocabActivity::loop() {
  switch (screen) {
    case Screen::Overview:
      loopOverview();
      break;
    case Screen::Card:
      loopCard();
      break;
    case Screen::Quiz:
      loopQuiz();
      break;
    case Screen::Done:
      loopDone();
      break;
    case Screen::Stats:
      loopStats();
      break;
  }
}

void VocabActivity::loopOverview() {
  const Rect area = contentArea();
  const int menuTop = area.y + area.height - MENU_ITEMS * (ROW_HEIGHT + GAP);
  const auto activate = [this](const int item) {
    haptic_feedback::touchAction();
    if (!deck.isOpen()) return;
    if (item == 0) {
      startSession();
    } else if (item == 1) {
      startQuiz();
    } else {
      openStats();
    }
  };
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (y < area.y && x < 100) {
      finish();
      return;
    }
    for (int i = 0; i < MENU_ITEMS; i++) {
      if (contains(menuRow(area, i, menuTop), x, y)) {
        selected = i;
        activate(i);
        return;
      }
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selected);
    return;
  }
  buttonNavigator.onNext([this] {
    selected = (selected + 1) % MENU_ITEMS;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = (selected + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void VocabActivity::loopCard() {
  const Rect area = contentArea();
  const auto leave = [this] {
    // Graded cards are already saved; the rest simply stay due.
    state.save();
    refreshCounts();
    selected = 0;
    screen = Screen::Overview;
    requestUpdate();
  };
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (y < area.y && x < 100) {
      leave();
      return;
    }
    if (!revealed) {
      revealed = true;
      haptic_feedback::touchAction();
      requestUpdate();
      return;
    }
    for (int i = 0; i < 4; i++) {
      if (contains(gradeButton(area, i), x, y)) {
        grade(static_cast<vocab::Grade>(i));
        return;
      }
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    leave();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!revealed) {
      revealed = true;
      requestUpdate();
    } else {
      grade(static_cast<vocab::Grade>(selected));
    }
    return;
  }
  if (!revealed) return;
  buttonNavigator.onNext([this] {
    selected = std::min(selected + 1, 3);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = std::max(selected - 1, 0);
    requestUpdate();
  });
}

void VocabActivity::loopQuiz() {
  const Rect area = contentArea();
  const auto next = [this] {
    quizPosition++;
    if (quizPosition >= quiz.size()) {
      state.save();
      lastWasQuiz = true;
      screen = Screen::Done;
      refreshCounts();
    } else {
      loadQuizQuestion();
    }
    requestUpdate();
  };
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (y < area.y && x < 100) {
      state.save();
      selected = 1;
      screen = Screen::Overview;
      refreshCounts();
      requestUpdate();
      return;
    }
    if (answered >= 0) {
      next();
      return;
    }
    for (int i = 0; i < 4; i++) {
      if (contains(optionRow(area, i), x, y)) {
        answerQuiz(i);
        return;
      }
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    state.save();
    selected = 1;
    screen = Screen::Overview;
    refreshCounts();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (answered >= 0) {
      next();
    } else {
      answerQuiz(selected);
    }
    return;
  }
  if (answered >= 0) return;
  buttonNavigator.onNext([this] {
    selected = (selected + 1) % 4;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = (selected + 3) % 4;
    requestUpdate();
  });
}

void VocabActivity::loopDone() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y) || mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selected = 0;
    screen = Screen::Overview;
    refreshCounts();
    requestUpdate();
  }
}

void VocabActivity::loopStats() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y) || mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selected = MENU_STATS;
    screen = Screen::Overview;
    requestUpdate();
  }
}

// --- layout ------------------------------------------------------------------

Rect VocabActivity::contentArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  const int top = metrics.topPadding + metrics.headerHeight + GAP;
  const int side = std::max<int>(metrics.contentSidePadding, 16);
  return Rect{content.x + side, top, content.width - 2 * side,
              content.y + content.height - metrics.buttonHintsHeight - GAP - top};
}

Rect VocabActivity::menuRow(const Rect& area, const int index, const int top) const {
  return Rect{area.x, top + index * (ROW_HEIGHT + GAP), area.width, ROW_HEIGHT};
}

Rect VocabActivity::gradeButton(const Rect& area, const int index) const {
  const int w = (area.width - 3 * GAP / 2) / 4;
  const int h = 72;
  return Rect{area.x + index * (w + GAP / 2), area.y + area.height - h, w, h};
}

Rect VocabActivity::cardBox(const Rect& area) const {
  // Leaves room for the grade buttons and the progress line.
  const int top = area.y + renderer.getLineHeight(SMALL_FONT_ID) + GAP;
  return Rect{area.x, top, area.width, area.y + area.height - 72 - GAP - top};
}

Rect VocabActivity::optionRow(const Rect& area, const int index) const {
  const int h = 88;
  const int top = area.y + area.height - 4 * (h + GAP) + GAP;
  return Rect{area.x, top + index * (h + GAP), area.width, h};
}

// --- rendering ---------------------------------------------------------------

void VocabActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  const char* title = tr(STR_VOCAB_TITLE);
  if (screen == Screen::Quiz) title = tr(STR_VOCAB_QUIZ);
  if (screen == Screen::Stats) title = tr(STR_VOCAB_STATS);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);

  const Rect area = contentArea();
  switch (screen) {
    case Screen::Overview:
      renderOverview(area);
      break;
    case Screen::Card:
      renderCard(area);
      break;
    case Screen::Quiz:
      renderQuiz(area);
      break;
    case Screen::Done:
      renderDone(area);
      break;
    case Screen::Stats: {
      // The full content width, as on the reading statistics page.
      const Rect content = UITheme::getContentArea(renderer);
      const int top = metrics.topPadding + metrics.headerHeight;
      DesktopHomeUi::drawVocabStatsPage(
          renderer, Rect{content.x, top, content.width, content.y + content.height - metrics.buttonHintsHeight - top},
          stats);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
  }

  const char* confirm = tr(STR_SELECT);
  if (screen == Screen::Card && !revealed) confirm = tr(STR_VOCAB_SHOW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void VocabActivity::renderOverview(const Rect& area) {
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int line = renderer.getLineHeight(UI_10_FONT_ID);
  int y = area.y;

  if (!message.empty()) {
    for (const auto& l : renderer.wrappedText(UI_10_FONT_ID, message.c_str(), area.width, 4)) {
      renderer.drawText(UI_10_FONT_ID, area.x, y, l.c_str());
      y += line;
    }
    y += GAP;
  }

  if (deck.isOpen()) {
    // Deck card: name, today's work, totals and streak.
    const int boxH = titleLine + 4 * line + 3 * GAP;
    renderer.drawRoundedRect(area.x, y, area.width, boxH, 1, RADIUS, true);
    int ty = y + GAP;
    renderer.drawText(UI_12_FONT_ID, area.x + GAP, ty,
                      renderer.truncatedText(UI_12_FONT_ID, deck.name().c_str(), area.width - 2 * GAP,
                                             EpdFontFamily::BOLD)
                          .c_str(),
                      true, EpdFontFamily::BOLD);
    ty += titleLine + GAP / 2;
    const uint16_t introduced = deck.newIntroducedToday(today);
    const int newToday = std::min<int>(counts.fresh, SETTINGS.vocabNewPerDay > introduced
                                                         ? SETTINGS.vocabNewPerDay - introduced
                                                         : 0);
    char text[96];
    snprintf(text, sizeof(text), tr(STR_VOCAB_TODAY_FMT), static_cast<int>(counts.due), newToday);
    renderer.drawText(UI_10_FONT_ID, area.x + GAP, ty, text, true, EpdFontFamily::BOLD);
    ty += line;
    snprintf(text, sizeof(text), tr(STR_VOCAB_LEARNED_FMT), static_cast<int>(counts.learned),
             static_cast<int>(deck.cardCount()));
    renderer.drawText(UI_10_FONT_ID, area.x + GAP, ty, text);
    ty += line;
    snprintf(text, sizeof(text), tr(STR_VOCAB_STREAK_FMT), static_cast<int>(state.currentStreak(today)),
             static_cast<int>(state.best));
    renderer.drawText(UI_10_FONT_ID, area.x + GAP, ty, text);
    ty += line;
    // Progress bar: share of the deck studied at least once.
    const int barW = area.width - 2 * GAP;
    const int filled = deck.cardCount() > 0 ? barW * counts.learned / deck.cardCount() : 0;
    renderer.drawRoundedRect(area.x + GAP, ty + 6, barW, 10, 1, 5, true);
    if (filled > 0) renderer.fillRoundedRect(area.x + GAP, ty + 6, std::max(filled, 10), 10, 5, Color::Black);
  }

  const int menuTop = area.y + area.height - MENU_ITEMS * (ROW_HEIGHT + GAP);
  for (int i = 0; i < MENU_ITEMS; i++) {
    const Rect row = menuRow(area, i, menuTop);
    char label[64];
    if (i == 0) {
      const uint16_t introduced = deck.newIntroducedToday(today);
      const int budget = SETTINGS.vocabNewPerDay > introduced ? SETTINGS.vocabNewPerDay - introduced : 0;
      snprintf(label, sizeof(label), tr(STR_VOCAB_STUDY_FMT),
               static_cast<int>(counts.due) + std::min<int>(counts.fresh, budget));
    } else {
      snprintf(label, sizeof(label), "%s", i == 1 ? tr(STR_VOCAB_QUIZ) : tr(STR_VOCAB_STATS));
    }
    const bool focus = i == selected;
    if (focus) {
      renderer.fillRoundedRect(row.x, row.y, row.width, row.height, RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(row.x, row.y, row.width, row.height, 1, RADIUS, true);
    }
    renderer.drawCenteredText(UI_12_FONT_ID, row.y + (row.height - titleLine) / 2, label, !focus,
                              EpdFontFamily::BOLD);
  }
}

void VocabActivity::renderCard(const Rect& area) {
  const int small = renderer.getLineHeight(SMALL_FONT_ID);
  const int wordLine = wordLineHeight();
  const int ipaLine = renderer.getLineHeight(IPA_12_FONT_ID);
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int line = renderer.getLineHeight(UI_10_FONT_ID);

  // Progress line: deck name left, "3 / 25" right, and whether the card is new.
  char progress[32];
  snprintf(progress, sizeof(progress), "%d / %d", static_cast<int>(position + 1), static_cast<int>(queue.size()));
  const int progressW = renderer.getTextWidth(SMALL_FONT_ID, progress, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, area.x + area.width - progressW, area.y, progress, true, EpdFontFamily::BOLD);
  const char* kind = cardState.dueDay == vocab::NEW_CARD && !queue[position].learning ? tr(STR_VOCAB_NEW_CARD)
                                                                                       : tr(STR_VOCAB_REVIEW_CARD);
  renderer.drawText(SMALL_FONT_ID, area.x, area.y, kind);

  const Rect box = cardBox(area);
  renderer.drawRoundedRect(box.x, box.y, box.width, box.height, 2, RADIUS, true);
  const int inner = box.width - 2 * GAP;

  if (!revealed) {
    // Front: the word and its pronunciation, centred.
    const auto wordLines = wrapWord(card.word, inner, 2);
    const int blockH = static_cast<int>(wordLines.size()) * wordLine + (card.ipa.empty() ? 0 : GAP + ipaLine);
    int y = box.y + (box.height - blockH) / 2;
    for (const auto& l : wordLines) {
      drawWordCentered(0, renderer.getScreenWidth(), y, l);
      y += wordLine;
    }
    if (!card.ipa.empty()) renderer.drawCenteredText(IPA_12_FONT_ID, y + GAP, card.ipa.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, box.y + box.height - small - GAP, tr(STR_VOCAB_TAP_TO_REVEAL));
    return;
  }

  // Back: word and IPA on top, then meaning and example.
  int y = box.y + GAP;
  drawWord(box.x + GAP, y, fitWord(card.word, inner));
  y += wordLine;
  if (!card.ipa.empty()) {
    renderer.drawText(IPA_12_FONT_ID, box.x + GAP, y, card.ipa.c_str());
    y += ipaLine;
  }
  y += GAP / 2;
  renderer.drawLine(box.x + GAP, y, box.x + box.width - GAP, y, true);
  y += GAP;
  for (const auto& l : renderer.wrappedText(UI_12_FONT_ID, card.meaning.c_str(), inner, 4, EpdFontFamily::BOLD)) {
    renderer.drawText(UI_12_FONT_ID, box.x + GAP, y, l.c_str(), true, EpdFontFamily::BOLD);
    y += titleLine;
  }
  if (!card.example.empty()) {
    y += GAP;
    const int bottom = box.y + box.height - GAP;
    for (const auto& l : renderer.wrappedText(UI_10_FONT_ID, card.example.c_str(), inner, 5)) {
      if (y + line > bottom) break;
      renderer.drawText(UI_10_FONT_ID, box.x + GAP, y, l.c_str());
      y += line;
    }
  }

  // Grade buttons, each with the interval it would give.
  for (int i = 0; i < 4; i++) {
    const Rect b = gradeButton(area, i);
    const bool focus = i == selected;
    if (focus) {
      renderer.fillRoundedRect(b.x, b.y, b.width, b.height, RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(b.x, b.y, b.width, b.height, 1, RADIUS, true);
    }
    const int top = b.y + (b.height - titleLine - small) / 2;
    const std::string label = renderer.truncatedText(UI_12_FONT_ID, gradeLabel(i), b.width - 8, EpdFontFamily::BOLD);
    const int labelW = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, b.x + (b.width - labelW) / 2, top, label.c_str(), !focus, EpdFontFamily::BOLD);
    char when[24];
    const uint16_t days = vocab::previewInterval(cardState, static_cast<vocab::Grade>(i), queue[position].learning);
    if (days == 0) {
      snprintf(when, sizeof(when), "%s", tr(STR_VOCAB_AGAIN_SOON));
    } else {
      snprintf(when, sizeof(when), tr(STR_VOCAB_DAYS_FMT), static_cast<int>(days));
    }
    const int whenW = renderer.getTextWidth(SMALL_FONT_ID, when);
    renderer.drawText(SMALL_FONT_ID, b.x + (b.width - whenW) / 2, top + titleLine, when, !focus);
  }
}

void VocabActivity::renderQuiz(const Rect& area) {
  const int small = renderer.getLineHeight(SMALL_FONT_ID);
  const int wordLine = wordLineHeight();
  const int ipaLine = renderer.getLineHeight(IPA_12_FONT_ID);
  const int line = renderer.getLineHeight(UI_10_FONT_ID);
  if (quiz.empty()) return;

  char progress[48];
  snprintf(progress, sizeof(progress), tr(STR_VOCAB_QUIZ_PROGRESS_FMT), static_cast<int>(quizPosition + 1),
           static_cast<int>(quiz.size()), quizCorrect);
  renderer.drawText(SMALL_FONT_ID, area.x, area.y, progress, true, EpdFontFamily::BOLD);

  // The word asked about, centred above the options.
  const int firstOption = optionRow(area, 0).y;
  const int blockH = wordLine + (card.ipa.empty() ? 0 : ipaLine + GAP / 2);
  int y = area.y + small + (firstOption - area.y - small - blockH) / 2;
  drawWordCentered(0, renderer.getScreenWidth(), y, fitWord(card.word, area.width));
  y += wordLine + GAP / 2;
  if (!card.ipa.empty()) renderer.drawCenteredText(IPA_12_FONT_ID, y, card.ipa.c_str());

  const QuizQuestion& q = quiz[quizPosition];
  for (int i = 0; i < 4; i++) {
    const Rect r = optionRow(area, i);
    // Before answering the focus is filled; after, the right answer is filled
    // and a wrong pick keeps a heavy outline.
    const bool fill = answered < 0 ? i == selected : i == q.correct;
    const bool wrongPick = answered >= 0 && i == answered && i != q.correct;
    if (fill) {
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(r.x, r.y, r.width, r.height, wrongPick ? 4 : 1, RADIUS, true);
    }
    std::string text = optionText[i];
    if (wrongPick) text = std::string(tr(STR_VOCAB_WRONG)) + " · " + text;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, text.c_str(), r.width - 2 * GAP, 2);
    int ty = r.y + (r.height - static_cast<int>(lines.size()) * line) / 2;
    for (const auto& l : lines) {
      renderer.drawText(UI_10_FONT_ID, r.x + GAP, ty, l.c_str(), !fill);
      ty += line;
    }
  }
  if (answered >= 0) {
    renderer.drawCenteredText(SMALL_FONT_ID, area.y + small + 2,
                              answered == q.correct ? tr(STR_VOCAB_CORRECT) : tr(STR_VOCAB_TAP_NEXT), true,
                              EpdFontFamily::BOLD);
  }
}

void VocabActivity::renderDone(const Rect& area) {
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int line = renderer.getLineHeight(UI_10_FONT_ID);
  int y = area.y + area.height / 3;
  char text[96];
  if (lastWasQuiz) {
    snprintf(text, sizeof(text), tr(STR_VOCAB_QUIZ_RESULT_FMT), quizCorrect, static_cast<int>(quiz.size()));
  } else if (studied == 0) {
    snprintf(text, sizeof(text), "%s", tr(STR_VOCAB_NOTHING_DUE));
  } else {
    snprintf(text, sizeof(text), tr(STR_VOCAB_DONE_FMT), studied);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, y, text, true, EpdFontFamily::BOLD);
  y += titleLine + GAP;
  snprintf(text, sizeof(text), tr(STR_VOCAB_STREAK_FMT), static_cast<int>(state.currentStreak(today)),
           static_cast<int>(state.best));
  renderer.drawCenteredText(UI_10_FONT_ID, y, text);
  y += line + GAP;
  if (!lastWasQuiz) {
    snprintf(text, sizeof(text), tr(STR_VOCAB_LEARNED_FMT), static_cast<int>(counts.learned),
             static_cast<int>(deck.cardCount()));
    renderer.drawCenteredText(UI_10_FONT_ID, y, text);
  }
}
