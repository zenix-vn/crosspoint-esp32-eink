#include "RecentBooksStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }
  if (!favoriteBooks.empty()) {
    JsonArray favArr = doc["favorites"].to<JsonArray>();
    for (const auto& book : favoriteBooks) {
      JsonObject obj = favArr.add<JsonObject>();
      obj["path"] = book.path;
      obj["title"] = book.title;
      obj["author"] = book.author;
      obj["coverBmpPath"] = book.coverBmpPath;
    }
  }
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) {
  recentBooks.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  recentBooks.reserve(std::min(arr.size(), static_cast<size_t>(MAX_RECENT_BOOKS)));
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_RECENT_BOOKS) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    if (!book.path.empty()) recentBooks.push_back(std::move(book));
  }

  favoriteBooks.clear();
  JsonArrayConst favArr = doc["favorites"].as<JsonArrayConst>();
  favoriteBooks.reserve(std::min(favArr.size(), static_cast<size_t>(MAX_FAVORITE_BOOKS)));
  for (JsonObjectConst obj : favArr) {
    if (getFavoriteCount() >= MAX_FAVORITE_BOOKS) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    if (!book.path.empty()) favoriteBooks.push_back(std::move(book));
  }

  LOG_DBG("RBS", "Recent books loaded (%d recent, %d favorites)", getCount(), getFavoriteCount());
  return true;
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  pruneMissing();

  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    std::rotate(recentBooks.begin(), it, it + 1);
    RecentBook& book = recentBooks.front();
    if (book.title != title) book.title = title;
    if (book.author != author) book.author = author;
    if (book.coverBmpPath != coverBmpPath) book.coverBmpPath = coverBmpPath;
  } else {
    recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});
    if (recentBooks.size() > MAX_RECENT_BOOKS) {
      recentBooks.resize(MAX_RECENT_BOOKS);
    }
  }

  auto favIt = std::find_if(favoriteBooks.begin(), favoriteBooks.end(),
                            [&](const RecentBook& book) { return book.path == path; });
  if (favIt != favoriteBooks.end()) {
    if (!title.empty() && favIt->title != title) favIt->title = title;
    if (!author.empty() && favIt->author != author) favIt->author = author;
    if (!coverBmpPath.empty() && favIt->coverBmpPath != coverBmpPath) favIt->coverBmpPath = coverBmpPath;
  }

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  bool changed = false;
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    it->title = title;
    it->author = author;
    it->coverBmpPath = coverBmpPath;
    changed = true;
  }
  auto favIt = std::find_if(favoriteBooks.begin(), favoriteBooks.end(),
                            [&](const RecentBook& book) { return book.path == path; });
  if (favIt != favoriteBooks.end()) {
    favIt->title = title;
    favIt->author = author;
    favIt->coverBmpPath = coverBmpPath;
    changed = true;
  }
  if (changed) saveToFile();
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  bool changed = false;
  auto updateEntry = [&](std::vector<RecentBook>& list) {
    auto it = std::find_if(list.begin(), list.end(), [&](const RecentBook& book) { return book.path == oldPath; });
    if (it == list.end()) return;
    it->path = newPath;
    if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
      it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
    }
    changed = true;
  };
  updateEntry(recentBooks);
  updateEntry(favoriteBooks);
  if (changed) saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t beforeRecent = recentBooks.size();
  const size_t beforeFav = favoriteBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  favoriteBooks.erase(std::remove_if(favoriteBooks.begin(), favoriteBooks.end(), &isMissing), favoriteBooks.end());
  return recentBooks.size() != beforeRecent || favoriteBooks.size() != beforeFav;
}

bool RecentBooksStore::isFavorite(const std::string& path) const {
  return std::any_of(favoriteBooks.begin(), favoriteBooks.end(),
                     [&](const RecentBook& book) { return book.path == path; });
}

bool RecentBooksStore::toggleFavorite(const RecentBook& book) {
  auto it = std::find_if(favoriteBooks.begin(), favoriteBooks.end(),
                         [&](const RecentBook& existing) { return existing.path == book.path; });
  if (it != favoriteBooks.end()) {
    favoriteBooks.erase(it);
    saveToFile();
    return false;
  }
  favoriteBooks.insert(favoriteBooks.begin(), book);
  if (favoriteBooks.size() > MAX_FAVORITE_BOOKS) {
    favoriteBooks.resize(MAX_FAVORITE_BOOKS);
  }
  saveToFile();
  return true;
}

bool RecentBooksStore::removeFavoriteByPath(const std::string& path) {
  auto it = std::find_if(favoriteBooks.begin(), favoriteBooks.end(),
                         [&](const RecentBook& book) { return book.path == path; });
  if (it == favoriteBooks.end()) return false;
  favoriteBooks.erase(it);
  saveToFile();
  return true;
}
