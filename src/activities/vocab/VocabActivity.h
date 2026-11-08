#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/DesktopHomeUi.h"
#include "util/ButtonNavigator.h"
#include "vocab/VocabDeck.h"

struct Rect;

// The vocabulary trainer (docs/vocabulary.md): a start screen with today's
// counts for the word list chosen in Settings > Vocabulary, spaced-repetition
// flash cards, a four-choice quiz and a statistics page rating the last week.
// Works by touch (tap the card, tap a button) and by buttons (Up/Down select,
// Confirm acts, Back leaves).
class VocabActivity final : public Activity {
  enum class Screen : uint8_t { Overview, Card, Quiz, Done, Stats };

  struct QueueItem {
    uint16_t card;
    bool learning;  // failed earlier this session
  };
  struct QuizQuestion {
    uint16_t card;
    uint16_t options[4];  // card indexes whose meanings are the choices
    uint8_t correct;      // which option is the card itself
  };

  Screen screen = Screen::Overview;
  vocab::Deck deck;
  vocab::TrainerState state;
  vocab::Counts counts;
  int32_t today = 0;
  std::string message;  // shown on the overview when no deck could be opened
  int selected = 0;     // focused row/button of the current screen
  vocab::Language language = vocab::Language::English;  // Settings > Vocabulary > Language
  // Font of the word on cards and quiz questions: the built-in large font, or
  // for Chinese the reader's SD font when it carries Han characters.
  int wordFont = 0;
  EpdFontFamily::Style wordStyle = EpdFontFamily::BOLD;
  // Chinese words are drawn magnified (256 = 1x) and emboldened by overstriking.
  int wordScale = 256;

  // Word drawing: honours wordScale, falls back to the plain renderer at 1x.
  int wordLineHeight() const;
  int wordWidth(const std::string& text) const;
  std::string fitWord(const std::string& text, int maxWidth) const;
  std::vector<std::string> wrapWord(const std::string& text, int maxWidth, int maxLines) const;
  void drawWord(int x, int y, const std::string& text) const;
  void drawWordCentered(int x, int width, int y, const std::string& text) const;

  // Study session
  std::vector<QueueItem> queue;
  size_t position = 0;
  bool revealed = false;
  vocab::Card card;
  vocab::CardState cardState;
  int studied = 0;

  // Quiz
  std::vector<QuizQuestion> quiz;
  std::vector<std::string> optionText;  // meanings of the current question's options
  size_t quizPosition = 0;
  int answered = -1;  // option picked for the current question, -1 before answering
  int quizCorrect = 0;
  bool lastWasQuiz = false;

  // Statistics, gathered when the page opens
  DesktopHomeUi::VocabStats stats;

  ButtonNavigator buttonNavigator;

  void openDeck(const std::string& name);
  void chooseHanFont();
  void refreshCounts();
  void startSession();
  void loadCurrentCard();
  void grade(vocab::Grade grade);
  void finishSession();
  void startQuiz();
  void loadQuizQuestion();
  void answerQuiz(int option);
  void openStats();

  void loopOverview();
  void loopCard();
  void loopQuiz();
  void loopDone();
  void loopStats();

  void renderOverview(const Rect& area);
  void renderCard(const Rect& area);
  void renderQuiz(const Rect& area);
  void renderDone(const Rect& area);
  Rect contentArea() const;
  Rect menuRow(const Rect& area, int index, int top) const;
  Rect gradeButton(const Rect& area, int index) const;
  Rect cardBox(const Rect& area) const;
  Rect optionRow(const Rect& area, int index) const;

 public:
  explicit VocabActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Vocab", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Cards, results and statistics stay up while read; menus and the quiz react to each tap.
  GrayText grayText() const override {
    return screen == Screen::Card || screen == Screen::Done || screen == Screen::Stats ? GrayText::Static
                                                                                       : GrayText::Interactive;
  }
};
