#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverGridHomeUi.h"
#include "components/DesktopHomeUi.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
  enum class DesktopAction : uint8_t {
    None,
    FileBrowser,
    FileTransfer,
    LibrarySlot,
    ReadingStats,
    ControlCenter,
    RecentBooksPage,
    FavoriteBooksPage,
    ToggleFavorite,
    BookDetails,
    RemoveRecent,
    NotInterested,
    UnfinishedBooksPage,
    ContinueReading,
    DropBook,
    ReceiveBooks,
    OpenDictionary,
    OpenSettings
  };
  // Which book page is open over Home (Recommended or Unfinished).
  enum class DesktopListPage : uint8_t { None, Recommended, Unfinished };
  // Positions in the Home focus order; see desktopFocusMap().
  struct DesktopFocusMap {
    int shelfCount;
    int seeAll;    // "See all >" of the shelf
    int recMore;   // "See more >" of the recommendations
    int recBase;   // first recommendation card
    int recCount;
    int dockBase;
    int count;
  };
  static constexpr unsigned long DESKTOP_LONG_PRESS_MS = 700;
  bool desktopHome = false;
  bool desktopStatsOpen = false;
  bool desktopBookListOpen = false;
  DesktopListPage desktopListPage = DesktopListPage::None;  // shares the page/selection below with the book list
  bool desktopFavoritesTab = false;
  int desktopBookListPage = 0;
  int desktopBookListSelected = 0;
  int recommendedCoversPage = -1;  // page of the open book page whose covers are loaded
  DesktopHomeUi::BookInfo desktopBook;  // progress and chapter of recentBooks[0]
  int desktopMinute = -1;               // minute shown by the clock; redraws on change
  RecentBook desktopTargetBook;         // book the long-press action menu acts on
  bool desktopSheetOpen = false;
  int desktopSheetIndex = 0;
  std::string desktopSheetTitle;
  Rect desktopSheetAnchor;
  std::vector<DesktopAction> desktopMenuActions;
  std::vector<RecentBook> favoriteBooks;
  std::vector<DesktopHomeUi::RecommendedBook> recommendedBooks;
  // Started but stalled books (5-95%), with "45% · 12 days ago" as the reason.
  std::vector<DesktopHomeUi::RecommendedBook> unfinishedBooks;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool hasPlugins = false;
  bool hasLibrarySlot() const { return hasPlugins || hasOpdsServers; }
  bool hasContinueReading = false;
  bool coverRendered = false;
  bool coverBufferStored = false;
  uint8_t* coverBuffer = nullptr;
  size_t coverBufferSize = 0;
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::LIBRARY) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::LIBRARY;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onRecentBookSelected(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onPluginsOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary();
  void resolveGridCoverPaths();
  void loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect);
  int desktopShelfCount() const;
  int desktopRecommendedCount() const;
  int desktopFocusCount() const;
  DesktopFocusMap desktopFocusMap() const;
  const RecentBook* desktopFocusedBook(Rect* outAnchor = nullptr) const;
  void reloadDesktopBooks();
  void loadRecommendedBooks();
  void loadDesktopCovers();
  void loopDesktop();
  void loopDesktopBookList();
  void openRecommendedPage();
  void openVocab();
  void openUnfinishedPage();
  void loadUnfinishedBooks();
  // Books of the open book page (Recommended or Unfinished).
  std::vector<DesktopHomeUi::RecommendedBook>& pageBooks();
  void loopDesktopRecommended();
  void loadRecommendedPageCovers();
  void renderDesktop();
  bool handleDesktopTouch();
  void activateDesktopFocus();
  void showDesktopMenu(const char* title);
  void loopDesktopSheet();
  DesktopHomeUi::SheetItem sheetItemFor(DesktopAction action) const;
  void openMoreMenu();
  void openBookActionsFor(const RecentBook& book, const Rect& anchor);
  void runDesktopAction(DesktopAction action);
  void openBookDetails(const RecentBook& book);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  GrayText grayText() const override {
    return desktopStatsOpen ? GrayText::Static : GrayText::Interactive;
  }
  bool isHomeActivity() const override { return true; }
};
