#pragma once

#include <map>
#include <optional>
#include <string>

#include "HalStorage.h"

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  bool success = true;
  std::string subjects;
  std::string series;
  std::optional<float> seriesIndex;
};

inline std::map<std::string, FakeMetadata> bookMetadata;

class Epub {
  std::string path;

 public:
  struct LibraryMetadata {
    std::string subjects;
    std::string series;
    std::optional<float> seriesIndex;
  };

  Epub(const std::string& path, const char*) : path(path) {}

  bool loadMetadata(std::string& title, std::string& author) {
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    return true;
  }

  bool loadMetadata(std::string& title, std::string& author, LibraryMetadata& extra) {
    if (!loadMetadata(title, author)) return false;
    const auto& metadata = bookMetadata[path];
    extra = {metadata.subjects, metadata.series, metadata.seriesIndex};
    return true;
  }
};
