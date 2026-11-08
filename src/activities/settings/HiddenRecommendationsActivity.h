#pragma once
#include <string>
#include <vector>

#include "HiddenRecommendations.h"
#include "activities/UiListActivity.h"

// Books dismissed from the Recommended shelf with "Not interested". Selecting
// one gives it back to the recommender.
class HiddenRecommendationsActivity final : public UiListActivity {
 public:
  explicit HiddenRecommendationsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void rebuildRows();

  std::vector<hidden_recommendations::Entry> entries_;
  std::vector<freeink::ui::ListItem> rowItems_;
};
