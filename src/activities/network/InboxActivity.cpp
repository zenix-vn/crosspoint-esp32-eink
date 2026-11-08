#include "InboxActivity.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first (same pin as OtaUpdater.cpp).
#include "network/HttpDownloader.h"
#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <WiFi.h>
// clang-format on

#include <strings.h>

#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/DeviceLink.h"
#include "util/PluginHttp.h"
#include "vocab/VocabDeck.h"

namespace {

constexpr char INBOX_DIR[] = "/Inbox";
constexpr size_t MAX_LIST_BYTES = 32 * 1024;


struct WaitingBook {
  std::string id;
  std::string name;
  size_t size;
};

// Word lists go straight to the vocabulary trainer's folder.
bool isWordList(const std::string& name) {
  return name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".tsv") == 0;
}

// "/Inbox/Book.epub", or "/Inbox/Book (2).epub" when a book of that name is
// already on the card: a second copy must never overwrite the first. A word
// list replaces its namesake in /Vocab instead: progress is kept by word, so
// an updated list keeps what was learned.
std::string freePath(const std::string& name) {
  if (isWordList(name)) return std::string(vocab::DECK_DIR) + "/" + name;
  const size_t dot = name.find_last_of('.');
  const std::string base = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
  const std::string ext = dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
  std::string path = std::string(INBOX_DIR) + "/" + name;
  for (int n = 2; Storage.exists(path.c_str()) && n < 100; n++) {
    path = std::string(INBOX_DIR) + "/" + base + " (" + std::to_string(n) + ")" + ext;
  }
  return path;
}

}  // namespace

bool InboxActivity::available() { return device_link::available(); }

void InboxActivity::setState(const State next) {
  RenderLock lock(*this);
  state = next;
}

void InboxActivity::onEnter() {
  Activity::onEnter();
  if (!available()) {
    state = State::NotConfigured;
    requestUpdate();
    return;
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void InboxActivity::onExit() {
  Activity::onExit();
  // Same teardown as the other Wi-Fi activities: restart to hand the TLS and
  // LWIP heap back in one piece.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void InboxActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }
  setState(State::Checking);
  requestUpdateAndWait();
  collect();
  requestUpdate();
}

void InboxActivity::collect() {
  // The device's own identity: no key to type (see DeviceLink.h).
  const std::string inboxUrl = device_link::serverUrl();
  const std::vector<HttpDownloader::Header> headers = device_link::headers();

  // 1. What is waiting.
  std::string listing;
  int status = 0;
  HttpDownloader::FetchOptions options;
  options.headers = headers;
  options.statusOut = &status;
  const bool listed = HttpDownloader::fetchUrl(
      inboxUrl + "?action=list",
      [&listing](const uint8_t* data, const size_t len) {
        if (listing.size() + len > MAX_LIST_BYTES) return false;
        listing.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      options);
  if (status == 403) {
    failure = tr(STR_INBOX_BAD_KEY);
    setState(State::Failed);
    return;
  }
  JsonDocument doc;
  if (listed && !deserializeJson(doc, listing) && (doc["pending"] | false)) {
    // Registered, but in no account yet: the user types the account's key.
    {
      RenderLock lock(*this);
      keyRejected = doc["bad_key"] | false;
    }
    setState(State::NotLinked);
    return;
  }
  if (!listed || doc.isNull() || !doc["books"].is<JsonArrayConst>()) {
    LOG_ERR("INBOX", "List failed (status %d)", status);
    failure = tr(STR_INBOX_CONNECT_FAILED);
    setState(State::Failed);
    return;
  }
  std::vector<WaitingBook> books;
  for (JsonObjectConst book : doc["books"].as<JsonArrayConst>()) {
    const char* id = book["id"] | "";
    const char* name = book["name"] | id;
    if (id[0] == '\0') continue;
    books.push_back(WaitingBook{id, name, book["size"] | 0u});
  }
  doc.clear();
  listing.clear();
  listing.shrink_to_fit();

  {
    RenderLock lock(*this);
    total = static_cast<int>(books.size());
    state = State::Downloading;
  }
  if (books.empty()) {
    setState(State::Done);
    return;
  }
  Storage.ensureDirectoryExists(INBOX_DIR);
  Storage.ensureDirectoryExists(vocab::DECK_DIR);

  // 2. Each book: download, and only once it is on the card, delete it there.
  for (size_t i = 0; i < books.size(); i++) {
    const WaitingBook& book = books[i];
    if (!FsHelpers::isSafePathComponent(book.name)) {
      LOG_ERR("INBOX", "Skipping unsafe name %s", book.name.c_str());
      failed++;
      continue;
    }
    {
      RenderLock lock(*this);
      current = static_cast<int>(i) + 1;
      currentName = book.name;
      bytesDone = 0;
      bytesTotal = book.size;
      lastPercent = -1;
    }
    requestUpdateAndWait();

    const std::string dest = freePath(book.name);
    const std::string url = inboxUrl + "?action=get&id=" + pluginhttp::urlEncodeQuery(book.id);
    const auto result = HttpDownloader::downloadToFile(
        url, dest,
        [this](const size_t done, const size_t size) {
          bytesDone = done;
          if (size > 0) bytesTotal = size;
          const int percent = bytesTotal > 0 ? static_cast<int>(static_cast<uint64_t>(done) * 100 / bytesTotal) : 0;
          // A repaint per 10%: e-ink cannot keep up with more, and each wake
          // of the render task competes with TLS for heap.
          if (percent / 10 != lastPercent / 10) {
            lastPercent = percent;
            requestUpdate(true);
          }
        },
        nullptr, "", "", headers);
    if (result != HttpDownloader::OK) {
      LOG_ERR("INBOX", "Download of %s failed (%d)", book.name.c_str(), static_cast<int>(result));
      failed++;
      continue;
    }

    std::string reply;
    const std::string form =
        "action=delete&" + device_link::formFields() + "&id=" + pluginhttp::urlEncodeQuery(book.id);
    if (!HttpDownloader::postForm(inboxUrl, form, reply)) {
      // The book is on the card; it will simply be offered again next time.
      LOG_ERR("INBOX", "Could not delete %s from the inbox", book.id.c_str());
    }
    received++;
    LOG_INF("INBOX", "Received %s -> %s", book.name.c_str(), dest.c_str());
  }

  // New files on the card: the Library rebuilds its index on next open.
  if (received > 0) library::markLibraryIndexDirty();
  setState(State::Done);
}

// Asks for the link key, saves it, and lists the inbox again with it (Wi-Fi is
// still up). The server links the device when the key matches an account.
void InboxActivity::enterKey() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_INBOX_KEY), SETTINGS.inboxKey,
                                              sizeof(SETTINGS.inboxKey) - 1, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto& kb = std::get<KeyboardResult>(result.data);
        snprintf(SETTINGS.inboxKey, sizeof(SETTINGS.inboxKey), "%s", kb.text.c_str());
        SETTINGS.saveToFile();
        setState(State::Checking);
        requestUpdateAndWait();
        collect();
        requestUpdate();
      });
}

void InboxActivity::loop() {
  int x = 0;
  int y = 0;
  if (state == State::NotLinked) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      enterKey();
    }
    return;
  }
  if (state == State::Done || state == State::Failed || state == State::NotConfigured) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}

void InboxActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int line = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - line) / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_INBOX_TITLE));

  const Rect textBounds{metrics.contentSidePadding, top + line + metrics.verticalSpacing,
                        pageWidth - metrics.contentSidePadding * 2, pageHeight / 3};
  char text[96];
  switch (state) {
    case State::WifiSelection:
    case State::Checking:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_INBOX_CHECKING));
      break;
    case State::Downloading: {
      snprintf(text, sizeof(text), tr(STR_INBOX_PROGRESS_FMT), current, total);
      renderer.drawCenteredText(UI_10_FONT_ID, top - line - metrics.verticalSpacing, text, true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(
          UI_10_FONT_ID, top,
          renderer.truncatedText(UI_10_FONT_ID, currentName.c_str(), pageWidth - metrics.contentSidePadding * 2)
              .c_str());
      const int percent =
          bytesTotal > 0 ? static_cast<int>(static_cast<uint64_t>(bytesDone) * 100 / bytesTotal) : 0;
      GUI.drawProgressBar(renderer,
                          Rect{metrics.contentSidePadding, top + line + metrics.verticalSpacing,
                               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                          percent, 100);
      break;
    }
    case State::Done:
      if (total == 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_INBOX_EMPTY), true, EpdFontFamily::BOLD);
      } else {
        snprintf(text, sizeof(text), tr(STR_INBOX_DONE_FMT), received);
        renderer.drawCenteredText(UI_10_FONT_ID, top, text, true, EpdFontFamily::BOLD);
        if (failed > 0) {
          snprintf(text, sizeof(text), tr(STR_INBOX_FAILED_FMT), failed);
          renderer.drawCenteredText(UI_10_FONT_ID, top + line + metrics.verticalSpacing, text);
        }
      }
      break;
    case State::NotConfigured:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_INBOX_NOT_CONFIGURED), true, EpdFontFamily::BOLD);
      UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID, tr(STR_INBOX_SETUP_HINT), 4, true,
                                       EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
      break;
    case State::NotLinked:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_INBOX_NOT_LINKED), true, EpdFontFamily::BOLD);
      UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID,
                                       keyRejected ? tr(STR_INBOX_KEY_REJECTED) : tr(STR_INBOX_LINK_HINT), 5, true,
                                       EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
      break;
    case State::Failed:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_INBOX_FAILED), true, EpdFontFamily::BOLD);
      if (failure) {
        UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID, failure, 3, true,
                                         EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
      }
      break;
  }
  if (state == State::NotLinked) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_INBOX_ENTER_KEY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == State::Done || state == State::Failed || state == State::NotConfigured) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
