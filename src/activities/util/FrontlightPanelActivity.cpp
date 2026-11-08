#include "FrontlightPanelActivity.h"

#include <BlePageTurner.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/readerToolbarIcons.h"
#include "util/SlideSwitch.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index

// Geometry aligned with DesktopHomeUi (SIDE_PADDING = 20) and ReaderToolbarUi
// (kScrubButton = 40, kScrubKnob = 22, labeled icon pills in bottom bar).
constexpr int16_t kPanelSideMargin = 20;
constexpr int16_t kTopSpacer = 10;
constexpr int16_t kHeaderBottomGap = 8;
constexpr int16_t kDividerGap = 12;

constexpr int16_t kCardPad = 14;
constexpr uint8_t kCardRadius = 12;
constexpr int16_t kCardHeaderH = 30;
constexpr int16_t kCardDividerGap = 14;
constexpr int16_t kCaptionGap = 4;
constexpr int16_t kSliderControlH = 40;
constexpr int16_t kSliderRowGap = 12;
constexpr int16_t kSliderGap = 12;
constexpr int16_t kTrackH = 6;
constexpr int16_t kKnobSize = 22;

constexpr int16_t kSectionGap = 12;
constexpr int16_t kQuickRowH = 72;
constexpr int16_t kQuickIconSize = 24;
constexpr uint8_t kQuickTileRadius = 12;

constexpr int16_t kGrabberMargin = 12;
constexpr int16_t kGrabberHeight = 5;
constexpr int16_t kGrabberInset = 12;

constexpr int BRIGHTNESS_STEP = 1;
constexpr uint8_t MIN_BRIGHTNESS = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
    touchModeRestore = SETTINGS.touchReaderControls;
  }

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  if (!Frontlight.present()) return;
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn && !slideSwitchOwnsLight()) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case 0:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 1:  // Ghost-cleanup refresh of the whole frame
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      close();
      break;
    case 2:  // Cycle the reading orientation
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case 3:  // Touch reader controls (for reading with the palm on the glass)
      if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
        touchModeRestore = SETTINGS.touchReaderControls;
        SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_OFF;
      } else {
        SETTINGS.touchReaderControls = touchModeRestore;
      }
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case 4:  // Bluetooth page turner
      if (!bleturner::status().compiledIn) break;
      SETTINGS.ble.enabled = SETTINGS.ble.enabled ? 0 : 1;
      if (!SETTINGS.ble.enabled) bleturner::switchOff();
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn && !slideSwitchOwnsLight()) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      close();
      return;
    }
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) {
      draggingSlider = false;
      cleanRefreshPending = true;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (!Frontlight.present()) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!slideSwitchOwnsLight()) toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto tokens = uiThemeTokens(uiTarget);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  const int16_t bandH = std::max<int16_t>(static_cast<int16_t>(metrics.batteryHeight), lineHeight);

  int y = kTopSpacer + bandH + kHeaderBottomGap + kDividerGap;
  if (Frontlight.present()) {
    const int16_t sliderRowH = static_cast<int16_t>(lineHeight + kCaptionGap + kSliderControlH);
    int cardH = kCardPad + kCardHeaderH + kCardDividerGap + sliderRowH + kCardPad;
    if (Frontlight.hasColorTemperature()) {
      cardH += kSliderRowGap + sliderRowH;
    }
    y += cardH + kSectionGap;
  }
  if (mappedInput.hasTouch()) {
    y += kQuickRowH;
  }
  y += kGrabberMargin + kGrabberHeight + kGrabberInset;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::drawSliderControl(UiScreen& screen, const fui::Rect& rect, const char* label,
                                                const uint8_t value, const fui::ActionId sliderAction,
                                                const fui::ActionId stepAction) {
  const auto& tokens = screen.theme();
  const int16_t captionH = screen.target().lineHeight(tokens.smallText.font);
  const fui::Rect captionRect{rect.x, rect.y, rect.width, captionH};

  fui::TextStyle labelStyle = tokens.smallText;
  labelStyle.bold = true;
  screen.target().text(captionRect, label, labelStyle);

  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));
  fui::TextStyle valueStyle = tokens.smallText;
  valueStyle.bold = true;
  valueStyle.align = fui::TextAlign::Right;
  screen.target().text(captionRect, pct, valueStyle);

  const fui::Rect band{rect.x, static_cast<int16_t>(rect.y + captionH + kCaptionGap), rect.width, kSliderControlH};

  // Step buttons: circular 40x40 outline buttons matching ReaderToolbarUi::buildToolbar.
  stepProps.icon = fui::BitmapRef{};
  stepProps.text = tokens.titleText;
  stepProps.text.bold = true;
  stepProps.inputMask = fui::InputTouch;
  stepProps.styles.explicitlySet = true;
  stepProps.styles.normal.background = fui::Paint::solid(fui::Color::White);
  stepProps.styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  stepProps.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  stepProps.styles.normal.borderWidth = 2;
  stepProps.styles.normal.radius = static_cast<uint8_t>(kSliderControlH / 2);
  stepProps.styles.selected = stepProps.styles.normal;
  stepProps.styles.focused = stepProps.styles.normal;
  stepProps.styles.disabled = stepProps.styles.normal;
  stepProps.styles.active = stepProps.styles.normal;
  stepProps.styles.active.background = fui::Paint::solid(fui::Color::Black);
  stepProps.styles.active.foreground = fui::Paint::solid(fui::Color::White);

  stepProps.label = "-";
  stepProps.action = stepAction;
  stepProps.value = -BRIGHTNESS_STEP;
  screen.button(stepProps, fui::Rect{band.x, band.y, kSliderControlH, kSliderControlH});

  stepProps.label = "+";
  stepProps.action = stepAction;
  stepProps.value = BRIGHTNESS_STEP;
  screen.button(
      stepProps,
      fui::Rect{static_cast<int16_t>(band.right() - kSliderControlH), band.y, kSliderControlH, kSliderControlH});

  // Sleek track + ringed knob matching ReaderToolbarUi's bottom scrub bar.
  const int16_t trackX = static_cast<int16_t>(band.x + kSliderControlH + kSliderGap);
  const int16_t trackW = static_cast<int16_t>(band.width - 2 * (kSliderControlH + kSliderGap));
  if (trackW <= kKnobSize) return;

  const int16_t trackY = static_cast<int16_t>(band.y + band.height / 2);
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const int clamped = std::clamp<int>(value, 0, 100);
  const int16_t knobX = static_cast<int16_t>(trackX + (static_cast<int32_t>(trackW - 1) * clamped) / 100);

  const fui::Rect trackRect{trackX, static_cast<int16_t>(trackY - kTrackH / 2), trackW, kTrackH};
  screen.target().stroke(trackRect, ink, 1, static_cast<uint8_t>(kTrackH / 2));
  if (knobX > trackX) {
    screen.target().fill(fui::Rect{trackX, trackRect.y, static_cast<int16_t>(knobX - trackX), kTrackH}, ink,
                         static_cast<uint8_t>(kTrackH / 2));
  }

  const fui::Rect knob{static_cast<int16_t>(knobX - kKnobSize / 2), static_cast<int16_t>(trackY - kKnobSize / 2),
                       kKnobSize, kKnobSize};
  screen.target().fill(knob, fui::Paint::solid(fui::Color::White), static_cast<uint8_t>(kKnobSize / 2));
  screen.target().stroke(knob, ink, 3, static_cast<uint8_t>(kKnobSize / 2));
  screen.frame().hit(fui::Rect{trackX, band.y, trackW, band.height}, sliderAction, 0,
                     fui::InputTouch | fui::InputDrag);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const fui::Paint paper = fui::Paint::solid(fui::Color::White);

  fui::SheetProps sheetProps;
  sheetProps.grabberWidth = 64;
  sheetProps.grabberHeight = kGrabberHeight;
  sheetProps.grabberMargin = kGrabberMargin;
  sheetProps.grabberInset = kGrabberInset;
  screen.sheet(sheetProps, static_cast<int16_t>(panelBottom));
  screen.insetContent(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});

  // Top status bar (Clock left, Battery right) + clean horizontal divider.
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    screen.spacer(kTopSpacer);
    const int16_t bandH = std::max<int16_t>(static_cast<int16_t>(metrics.batteryHeight),
                                            screen.target().lineHeight(theme.smallText.font));
    const fui::Rect statusBand = screen.takeTop(bandH, kHeaderBottomGap);
    UITheme::getInstance().getTheme().BaseTheme::drawHeader(
        renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.homeTopPadding - metrics.topPadding},
        nullptr);
    const fui::Rect divBand = screen.takeTop(kDividerGap);
    screen.target().fill(fui::Rect{statusBand.x, divBand.y, statusBand.width, 1}, ink);
  }

  // Unified Frontlight Card: header with lamp toggle pill + symmetric sleek sliders.
  if (Frontlight.present()) {
    const int16_t captionH = screen.target().lineHeight(theme.smallText.font);
    const int16_t sliderRowH = static_cast<int16_t>(captionH + kCaptionGap + kSliderControlH);
    int16_t cardH = static_cast<int16_t>(kCardPad + kCardHeaderH + kCardDividerGap + sliderRowH + kCardPad);
    if (Frontlight.hasColorTemperature()) {
      cardH = static_cast<int16_t>(cardH + kSliderRowGap + sliderRowH);
    }

    const fui::Rect card = screen.takeTop(cardH, kSectionGap);
    screen.target().stroke(card, ink, 1, kCardRadius);

    const int16_t innerX = static_cast<int16_t>(card.x + kCardPad);
    const int16_t innerW = static_cast<int16_t>(card.width - 2 * kCardPad);
    int16_t curY = static_cast<int16_t>(card.y + kCardPad);

    // Card header: [Sun Icon] Đèn nền ... [Bật / Tắt pill]
    {
      const fui::Rect iconRect{innerX, static_cast<int16_t>(curY + (kCardHeaderH - 24) / 2), 24, 24};
      screen.target().bitmap(iconRect, fui::bitmapFromIcon(lightOn ? icon_sun_filled_24 : icon_sun_24),
                             fui::BitmapMode::Center, ink);

      fui::TextStyle titleStyle = theme.bodyText;
      titleStyle.bold = true;
      const int16_t titleH = screen.target().lineHeight(titleStyle.font);
      const int16_t titleX = static_cast<int16_t>(innerX + 24 + 8);
      screen.target().text(
          fui::Rect{titleX, static_cast<int16_t>(curY + (kCardHeaderH - titleH) / 2),
                    static_cast<int16_t>(innerW - 24 - 8 - 84), titleH},
          tr(STR_FRONTLIGHT), titleStyle);

      if (!slideSwitchOwnsLight()) {
        const char* stateLabel = lightOn ? tr(STR_ENABLED) : tr(STR_DISABLED);
        fui::TextStyle pillText = theme.smallText;
        pillText.bold = true;
        pillText.align = fui::TextAlign::Center;
        pillText.color = lightOn ? fui::Color::White : fui::Color::Black;
        const int16_t textW = screen.target().measureText(pillText.font, stateLabel, pillText).width;
        const int16_t pillW = std::max<int16_t>(68, static_cast<int16_t>(textW + 28));
        const fui::Rect pill{static_cast<int16_t>(innerX + innerW - pillW), curY, pillW, kCardHeaderH};
        const uint8_t pillRadius = static_cast<uint8_t>(kCardHeaderH / 2);
        if (lightOn) {
          screen.target().fill(pill, ink, pillRadius);
        } else {
          screen.target().stroke(pill, ink, 2, pillRadius);
        }
        const int16_t pillLineH = screen.target().lineHeight(pillText.font);
        screen.target().text(
            fui::Rect{pill.x, static_cast<int16_t>(pill.y + (pill.height - pillLineH) / 2), pill.width, pillLineH},
            stateLabel, pillText);
        screen.frame().hit(pill, ACTION_TOGGLE, 0, fui::InputTouch);
      }
      curY = static_cast<int16_t>(curY + kCardHeaderH);
    }

    // Subtle divider inside the card.
    screen.target().fill(fui::Rect{innerX, static_cast<int16_t>(curY + kCardDividerGap / 2), innerW, 1}, ink);
    curY = static_cast<int16_t>(curY + kCardDividerGap);

    // Brightness slider row.
    drawSliderControl(screen, fui::Rect{innerX, curY, innerW, sliderRowH}, tr(STR_BRIGHTNESS), brightness,
                      ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP);
    curY = static_cast<int16_t>(curY + sliderRowH);

    // Warmth slider row (aligned to exact same width).
    if (Frontlight.hasColorTemperature()) {
      curY = static_cast<int16_t>(curY + kSliderRowGap);
      drawSliderControl(screen, fui::Rect{innerX, curY, innerW, sliderRowH}, tr(STR_WARMTH), warmth, ACTION_WARMTH,
                        ACTION_WARMTH_STEP);
    }
  }

  // Labeled quick-action bar synchronized with ReaderToolbarUi::buildToolRow & DesktopHomeUi::drawDock.
  if (mappedInput.hasTouch()) {
    const int tileCount = bleturner::status().compiledIn ? kTileCount : kTileCount - 1;
    const fui::BitmapRef icons[kTileCount] = {
        fui::bitmapFromIcon(SETTINGS.screenInverted ? icon_moon_filled_24 : icon_moon_24),
        fui::bitmapFromIcon(icon_refresh_cw_24),
        fui::bitmapFromIcon(icon_reader_text_24),
        fui::bitmapFromIcon(icon_pointer_24),
        fui::bitmapFromIcon(SETTINGS.ble.enabled ? icon_bluetooth_24 : icon_bluetooth_off_24)};
    const char* labels[kTileCount] = {tr(STR_QS_NIGHT), tr(STR_QS_REFRESH), tr(STR_QS_ROTATE), tr(STR_TOUCH_TOGGLE),
                                      tr(STR_QS_BLUETOOTH)};
    const bool enabled[kTileCount] = {SETTINGS.screenInverted != 0, false, false,
                                      SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF,
                                      SETTINGS.ble.enabled != 0};
    static constexpr fui::Rotation rotations[] = {fui::Rotation::None, fui::Rotation::CW90, fui::Rotation::R180,
                                                  fui::Rotation::CCW90};
    // Underline inside 24x24 icon box for the 4 orientations.
    static constexpr fui::Rect underlines[] = {{3, 21, 18, 2}, {1, 3, 2, 18}, {3, 1, 18, 2}, {21, 3, 2, 18}};

    const fui::Rect row = screen.takeTop(kQuickRowH);
    const int16_t labelH = screen.target().lineHeight(theme.smallText.font);
    const int16_t contentTop = static_cast<int16_t>(row.y + (row.height - kQuickIconSize - 4 - labelH) / 2);

    for (int id = 0; id < tileCount; ++id) {
      const int16_t left = static_cast<int16_t>(row.x + row.width * id / tileCount);
      const int16_t right = static_cast<int16_t>(row.x + row.width * (id + 1) / tileCount);
      const fui::Rect slot{left, row.y, static_cast<int16_t>(right - left), row.height};
      const fui::Rect pill = slot.inset(fui::Insets{2, 3, 2, 3});

      if (enabled[id]) {
        screen.target().fill(pill, ink, kQuickTileRadius);
      } else {
        screen.target().stroke(pill, ink, 1, kQuickTileRadius);
      }

      const fui::Paint fg = enabled[id] ? paper : ink;
      const fui::Rect iconRect{static_cast<int16_t>(slot.x + (slot.width - kQuickIconSize) / 2), contentTop,
                               kQuickIconSize, kQuickIconSize};
      const int orientation = SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT;
      screen.target().bitmap(iconRect, icons[id], fui::BitmapMode::Center, fg,
                             id == 2 ? rotations[orientation] : fui::Rotation::None);
      if (id == 2) {
        const auto& line = underlines[orientation];
        screen.target().fill(fui::Rect{static_cast<int16_t>(iconRect.x + line.x),
                                       static_cast<int16_t>(iconRect.y + line.y), line.width, line.height},
                             fg);
      }

      fui::TextStyle caption = theme.smallText;
      caption.align = fui::TextAlign::Center;
      caption.bold = enabled[id];
      caption.color = enabled[id] ? fui::Color::White : fui::Color::Black;
      screen.target().text(
          fui::Rect{pill.x + 2, static_cast<int16_t>(contentTop + kQuickIconSize + 4),
                    static_cast<int16_t>(pill.width - 4), labelH},
          labels[id], caption);

      const bool switchOwned = (id == 0 && slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_NIGHT_MODE)) ||
                               (id == 3 && slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_TOUCH_CONTROLS));
      if (!switchOwned) screen.frame().hit(slot, ACTION_TILE, static_cast<int16_t>(id), fui::InputTouch);
    }
  }
}

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();
  renderUi();
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
