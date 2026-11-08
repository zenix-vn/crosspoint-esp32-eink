#include "VocabDeckSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "vocab/VocabDeck.h"

namespace fui = freeink::ui;

VocabDeckSelectActivity::VocabDeckSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("VocabDeckSelect", renderer, mappedInput) {}

void VocabDeckSelectActivity::onEnter() {
  UiListActivity::onEnter();
  language = vocab::languageFromSetting(SETTINGS.vocabLanguage);
  vocab::ensureStarterDeck(language);
  decks = vocab::listDecks(language);
  vocab::TrainerState state;
  state.load();
  const std::string& chosen = state.deckFor(language);

  const auto it = std::find(decks.begin(), decks.end(), chosen);
  nav.selected = it != decks.end() ? static_cast<int>(it - decks.begin()) : 0;
  rowItems.clear();
  rowItems.reserve(decks.size());
  for (size_t i = 0; i < decks.size(); i++) {
    fui::ListItem item;
    item.label = decks[i].c_str();
    if (decks[i] == chosen) item.value = tr(STR_SELECTED);
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

const char* VocabDeckSelectActivity::headerTitle() const { return tr(STR_VOCAB_CHOOSE_DECK); }

void VocabDeckSelectActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index < 0 || index >= static_cast<int>(decks.size())) return;
  vocab::TrainerState state;
  state.load();
  state.deckFor(language) = decks[static_cast<size_t>(index)];
  state.save();
  finish();
}

void VocabDeckSelectActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (rowItems.empty()) {
    screen.centeredText(language == vocab::Language::Chinese ? tr(STR_VOCAB_NO_DECKS_ZH) : tr(STR_VOCAB_NO_DECKS));
    return;
  }
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
