#pragma once

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Top-anchored control center opened by a top-edge down-swipe, a status-bar tap,
// or a button bound to "Control Center": status header, a unified frontlight
// card (header with lamp toggle + symmetric sleek sliders matching the reader
// bottom toolbar), and a labeled quick-setting icon bar synchronized with the
// bottom menu layout.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  bool lightOnChanged = false;
  bool draggingSlider = false;
  uint8_t touchModeRestore = CrossPointSettings::TOUCH_READER_ON;
  int panelBottom = 0;

  // Quick settings, left to right: night mode, refresh, orientation, touch, Bluetooth.
  static constexpr int kTileCount = 5;

  // ButtonProps embeds a 324-byte StyleSet, so keep it off the render stack.
  freeink::ui::ButtonProps stepProps;

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTileEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  void drawSliderControl(UiScreen& screen, const freeink::ui::Rect& rect, const char* label, uint8_t value,
                         freeink::ui::ActionId sliderAction, freeink::ui::ActionId stepAction);
  int computePanelBottom() const;
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  void runTile(int idx);
  void persistLightSettings();
  void close();

  bool cleanRefreshPending = false;

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
};
