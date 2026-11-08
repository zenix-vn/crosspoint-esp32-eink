#include "DictionaryActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void indexBuildYield(void*) { vTaskDelay(1); }
}  // namespace

void DictionaryActivity::onEnter() {
  Activity::onEnter();
  if (SETTINGS.dictionaryName[0] == '\0') {
    showMessage(StrId::STR_DICT_NO_DICT_SET);
    return;
  }
  state = State::Prompt;
  promptPending = true;
  requestUpdate();
}

void DictionaryActivity::showMessage(const StrId id) {
  message = id;
  state = State::Message;
  requestUpdate();
}

void DictionaryActivity::askWord() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_DICTIONARY), lastWord);
  if (!keyboard) {
    showMessage(StrId::STR_DICT_LOW_MEMORY);
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }
    lookUp(std::get<KeyboardResult>(result.data).text);
  });
}

void DictionaryActivity::lookUp(std::string word) {
  lastWord = word;
  const std::string query = Dictionary::cleanWord(word.c_str());
  if (query.empty()) {
    promptPending = true;
    return;
  }

  state = State::Busy;
  message = StrId::STR_DICT_LOOKING_UP;
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
    dictNeedsIndex = dictOpenOk && dict.needsIndex();
  }
  if (dictNeedsIndex) message = StrId::STR_DICT_INDEXING;
  requestUpdateAndWait();  // paint the busy message before blocking on the SD card

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;
  }

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  if (ok && dict.lookup(word.c_str(), definition, headword, &result)) {
    auto activity = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                    std::move(definition), dict.definitionsAreHtml());
    if (!activity || !activity->enableLookup(dict, std::move(word))) {
      LOG_ERR("DICT", "OOM: definition activity");
      showMessage(StrId::STR_DICT_LOW_MEMORY);
      return;
    }
    // Back from the definition goes on to the next word.
    startActivityForResult(std::move(activity), [this](const ActivityResult&) {
      state = State::Prompt;
      promptPending = true;
      requestUpdate();
    });
    return;
  }

  if (!ok) {
    showMessage(indexResult == Dictionary::IndexResult::LowMemory   ? StrId::STR_DICT_LOW_MEMORY
                : indexResult == Dictionary::IndexResult::ReadError ? StrId::STR_DICT_READ_FAILED
                                                                    : StrId::STR_DICT_ERROR);
    return;
  }
  switch (result) {
    case Dictionary::LookupResult::Decompress:
      showMessage(StrId::STR_DICT_DECOMPRESS_ERROR);
      break;
    case Dictionary::LookupResult::LowMemory:
      showMessage(StrId::STR_DICT_LOW_MEMORY);
      break;
    case Dictionary::LookupResult::ReadError:
      showMessage(StrId::STR_DICT_READ_FAILED);
      break;
    default:
      showMessage(StrId::STR_DICT_NOT_FOUND);
      break;
  }
}

void DictionaryActivity::loop() {
  if (promptPending) {
    promptPending = false;
    askWord();
    return;
  }
  if (state == State::Message) {
    // Confirm tries another word (unless there is no dictionary); Back leaves.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    } else if (message != StrId::STR_DICT_NO_DICT_SET && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = State::Prompt;
      promptPending = true;
      requestUpdate();
    }
  }
}

void DictionaryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_DICTIONARY));
  if (state != State::Prompt) {
    const int line = renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawCenteredText(UI_12_FONT_ID, (renderer.getScreenHeight() - line) / 2, I18n::getInstance().get(message), true,
                              EpdFontFamily::BOLD);
  }
  if (state == State::Message) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), message == StrId::STR_DICT_NO_DICT_SET ? "" : tr(STR_SEARCH),
                                              "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
