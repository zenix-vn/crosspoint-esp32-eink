#include "CollectionsStore.h"

#include <Logging.h>

#include <algorithm>

namespace {

std::string cleanName(const std::string& name) {
  const size_t first = name.find_first_not_of(' ');
  if (first == std::string::npos) return {};
  const size_t last = name.find_last_not_of(' ');
  std::string out = name.substr(first, last - first + 1);
  if (out.size() > CollectionsStore::MAX_NAME) out.resize(CollectionsStore::MAX_NAME);
  return out;
}

}  // namespace

void CollectionsStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["collections"].to<JsonArray>();
  for (const auto& collection : collections) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = collection.name;
    JsonArray paths = obj["books"].to<JsonArray>();
    for (const auto& path : collection.paths) paths.add(path);
  }
}

bool CollectionsStore::fromJson(JsonVariantConst doc) {
  collections.clear();
  for (JsonObjectConst obj : doc["collections"].as<JsonArrayConst>()) {
    if (count() >= MAX_COLLECTIONS) break;
    BookCollection collection;
    collection.name = cleanName(obj["name"] | "");
    if (collection.name.empty()) continue;
    for (JsonVariantConst path : obj["books"].as<JsonArrayConst>()) {
      if (static_cast<int>(collection.paths.size()) >= MAX_BOOKS_PER_COLLECTION) break;
      const char* value = path | "";
      if (*value) collection.paths.emplace_back(value);
    }
    std::sort(collection.paths.begin(), collection.paths.end());
    collection.paths.erase(std::unique(collection.paths.begin(), collection.paths.end()), collection.paths.end());
    collections.push_back(std::move(collection));
  }
  LOG_DBG("COL", "Collections loaded (%d)", count());
  return true;
}

int CollectionsStore::create(const std::string& name) {
  const std::string clean = cleanName(name);
  if (clean.empty() || count() >= MAX_COLLECTIONS) return -1;
  collections.push_back(BookCollection{clean, {}});
  saveToFile();
  return count() - 1;
}

bool CollectionsStore::rename(const int index, const std::string& name) {
  const std::string clean = cleanName(name);
  if (!valid(index) || clean.empty()) return false;
  collections[index].name = clean;
  return saveToFile();
}

bool CollectionsStore::remove(const int index) {
  if (!valid(index)) return false;
  collections.erase(collections.begin() + index);
  return saveToFile();
}

bool CollectionsStore::addBook(const int index, const std::string& path) {
  if (!valid(index) || path.empty()) return false;
  auto& paths = collections[index].paths;
  const auto it = std::lower_bound(paths.begin(), paths.end(), path);
  if (it != paths.end() && *it == path) return true;
  if (static_cast<int>(paths.size()) >= MAX_BOOKS_PER_COLLECTION) return false;
  paths.insert(it, path);
  return saveToFile();
}

bool CollectionsStore::removeBook(const int index, const std::string& path) {
  if (!valid(index)) return false;
  auto& paths = collections[index].paths;
  const auto it = std::lower_bound(paths.begin(), paths.end(), path);
  if (it == paths.end() || *it != path) return false;
  paths.erase(it);
  return saveToFile();
}

bool CollectionsStore::contains(const int index, const std::string& path) const {
  if (!valid(index)) return false;
  const auto& paths = collections[index].paths;
  return std::binary_search(paths.begin(), paths.end(), path);
}

void CollectionsStore::updatePath(const std::string& oldPath, const std::string& newPath) {
  bool changed = false;
  for (auto& collection : collections) {
    auto& paths = collection.paths;
    const auto it = std::lower_bound(paths.begin(), paths.end(), oldPath);
    if (it == paths.end() || *it != oldPath) continue;
    paths.erase(it);
    paths.insert(std::lower_bound(paths.begin(), paths.end(), newPath), newPath);
    changed = true;
  }
  if (changed) saveToFile();
}
