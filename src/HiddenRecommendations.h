#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Books the reader dismissed from the Recommended shelf with "Not interested".
// Kept as one "path\ttitle\tauthor" line per book in /.crosspoint/rec_hidden.txt
// so Settings can list them by name and give them back; the recommender only
// needs their path hashes. Oldest entries fall off past MAX_ENTRIES.
namespace hidden_recommendations {

constexpr size_t MAX_ENTRIES = 64;

struct Entry {
  std::string path;
  std::string title;
  std::string author;
};

std::vector<Entry> load();
// Appends the book (no-op when already hidden). Returns false on a write failure.
bool hide(const std::string& path, const std::string& title, const std::string& author);
// Returns false when the book was not hidden or the write failed.
bool unhide(const std::string& path);
// clixPathHash of every hidden book, for excluding them from a library scan.
std::vector<uint64_t> pathHashes();

}  // namespace hidden_recommendations
