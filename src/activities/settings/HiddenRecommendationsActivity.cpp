#include "HiddenRecommendationsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

HiddenRecommendationsActivity::HiddenRecommendationsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("HiddenRecommendations", renderer, mappedInput) {}

void HiddenRecommendationsActivity::onEnter() {
  UiListActivity::onEnter();
  entries_ = hidden_recommendations::load();
  // Most recently hidden first: the one just dismissed by mistake is on top.
  std::reverse(entries_.begin(), entries_.end());
  rebuildRows();
  nav.reset(0);
}

void HiddenRecommendationsActivity::rebuildRows() {
  rowItems_.resize(entries_.size());
  for (size_t i = 0; i < entries_.size(); i++) {
    const auto& e = entries_[i];
    rowItems_[i] = fui::ListItem{};
    rowItems_[i].label = e.title.empty() ? e.path.c_str() : e.title.c_str();
    rowItems_[i].value = e.author.empty() ? nullptr : e.author.c_str();
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int HiddenRecommendationsActivity::listCount() const { return static_cast<int>(entries_.size()); }

const char* HiddenRecommendationsActivity::headerTitle() const { return tr(STR_HIDDEN_RECOMMENDATIONS); }

void HiddenRecommendationsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  hidden_recommendations::unhide(entries_[static_cast<size_t>(index)].path);
  entries_.erase(entries_.begin() + index);
  rebuildRows();
  nav.reset(std::min(index, std::max(0, listCount() - 1)));
  requestUpdate();
}

void HiddenRecommendationsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (entries_.empty()) {
    screen.centeredText(tr(STR_HIDDEN_RECOMMENDATIONS_EMPTY));
    return;
  }
  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.valueText = screen.theme().smallText;
  syncListViewport(screen, props);
  screen.list(props);
}
