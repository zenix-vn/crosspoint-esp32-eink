#pragma once

#include <string>

#include "activities/Activity.h"

// Collects the books waiting in the remote inbox (CROSSPOINT_INBOX_URL, a small
// API on the self-hosted server) for this device, identified by DeviceLink:
// joins Wi-Fi, downloads each book into /Inbox (word lists into /Vocab), and
// deletes it from the server once it is safely on the card. A device not yet
// in an account asks for the account's link key and retries with it.
class InboxActivity final : public Activity {
  enum class State { WifiSelection, Checking, Downloading, Done, NotConfigured, NotLinked, Failed };

  State state = State::WifiSelection;
  int total = 0;
  int current = 0;  // 1-based index of the book being downloaded
  int received = 0;
  int failed = 0;
  size_t bytesDone = 0;
  size_t bytesTotal = 0;
  int lastPercent = -1;
  std::string currentName;
  bool keyRejected = false;  // State::NotLinked: the server did not know the key sent
  const char* failure = nullptr;  // i18n string, flash resident

  void onWifiSelectionComplete(bool success);
  void enterKey();
  void collect();
  void setState(State next);

 public:
  explicit InboxActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Inbox", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Checking || state == State::Downloading; }
  bool skipLoopDelay() override { return true; }

  // Whether this build has an inbox to collect from.
  static bool available();
};
