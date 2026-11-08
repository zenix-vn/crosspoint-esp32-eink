#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

// User-named shelves of books, by SD path. Paths stay sorted so the library
// filter can test membership with a binary search.
struct BookCollection {
  std::string name;
  std::vector<std::string> paths;
};

class CollectionsStore : public PersistableStore<CollectionsStore> {
 public:
  static constexpr int MAX_COLLECTIONS = 32;
  static constexpr int MAX_BOOKS_PER_COLLECTION = 1024;
  static constexpr size_t MAX_NAME = 48;

 private:
  std::vector<BookCollection> collections;

  CollectionsStore() = default;
  ~CollectionsStore() = default;

  friend class PersistableStore<CollectionsStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/collections.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<BookCollection>& all() const { return collections; }
  int count() const { return static_cast<int>(collections.size()); }
  bool valid(int index) const { return index >= 0 && index < count(); }

  // Each mutation persists; they return false on bad input or when full.
  int create(const std::string& name);  // new index, or -1
  bool rename(int index, const std::string& name);
  bool remove(int index);
  bool addBook(int index, const std::string& path);
  bool removeBook(int index, const std::string& path);
  bool contains(int index, const std::string& path) const;
  // Keeps memberships when a book file moves (see RecentBooksStore::updatePath).
  void updatePath(const std::string& oldPath, const std::string& newPath);
};

#define COLLECTIONS CollectionsStore::getInstance()
