#pragma once

#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Stand-alone English dictionary (docs/dictionary.md): asks for a word on the
// on-screen keyboard, looks it up in the StarDict dictionary chosen in
// Settings > Reader > Dictionary (data lives on the SD card) and shows the
// definition. Back from the definition returns to the keyboard for the next
// word; cancelling the keyboard leaves the app.
class DictionaryActivity final : public Activity {
 public:
  explicit DictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Dictionary", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Prompt, Busy, Message };

  void askWord();
  void lookUp(std::string word);
  void showMessage(StrId id);

  Dictionary dict;
  State state = State::Prompt;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;
  bool promptPending = false;
  std::string lastWord;
};
