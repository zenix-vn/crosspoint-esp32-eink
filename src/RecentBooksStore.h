#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore : public PersistableStore<RecentBooksStore> {
 public:
  static constexpr int MAX_RECENT_BOOKS = 16;
  static constexpr int MAX_FAVORITE_BOOKS = 40;

 private:
  std::vector<RecentBook> recentBooks;
  std::vector<RecentBook> favoriteBooks;

  RecentBooksStore() = default;
  ~RecentBooksStore() = default;

  friend class PersistableStore<RecentBooksStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/recent.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);

  bool removeByPath(const std::string& path);

  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  static bool isMissing(const RecentBook& book);

  bool pruneMissing();

  const std::vector<RecentBook>& getBooks() const { return recentBooks; }
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  // Favorite books management
  bool isFavorite(const std::string& path) const;
  bool toggleFavorite(const RecentBook& book);
  bool removeFavoriteByPath(const std::string& path);
  const std::vector<RecentBook>& getFavorites() const { return favoriteBooks; }
  int getFavoriteCount() const { return static_cast<int>(favoriteBooks.size()); }
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
