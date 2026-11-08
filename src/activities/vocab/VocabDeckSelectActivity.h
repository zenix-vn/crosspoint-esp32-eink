#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "vocab/VocabDeck.h"

class MappedInputManager;

// Settings > Vocabulary > Word list: the decks in /Vocab teaching the chosen
// language, the chosen one marked. Choosing one makes it that language's deck.
class VocabDeckSelectActivity final : public UiListActivity {
 public:
  explicit VocabDeckSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  vocab::Language language = vocab::Language::English;
  std::vector<std::string> decks;
  std::vector<freeink::ui::ListItem> rowItems;
};
