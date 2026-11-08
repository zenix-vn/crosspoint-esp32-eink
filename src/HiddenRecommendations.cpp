#include "HiddenRecommendations.h"

#include <HalStorage.h>
#include <LibraryFormat.h>
#include <Logging.h>

#include <algorithm>

namespace hidden_recommendations {
namespace {

constexpr char PATH[] = "/.crosspoint/rec_hidden.txt";
// The first build kept bare hashes, which cannot be listed by name; they are
// dropped rather than left as books the reader can never get back.
constexpr char LEGACY_PATH[] = "/.crosspoint/rec_hidden.bin";
constexpr size_t MAX_FILE_BYTES = 64 * 1024;

// Tabs and newlines are the file's separators; a title carrying one would
// split its line.
std::string clean(const std::string& value) {
  std::string out = value;
  std::replace_if(out.begin(), out.end(), [](const char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
  return out;
}

bool save(const std::vector<Entry>& entries) {
  std::string text;
  for (const auto& e : entries) {
    text += clean(e.path) + '\t' + clean(e.title) + '\t' + clean(e.author) + '\n';
  }
  HalFile file;
  if (!Storage.openFileForWrite("RECHIDE", PATH, file)) return false;
  const bool ok = file.write(text.data(), text.size()) == text.size();
  file.close();
  return ok;
}

}  // namespace

std::vector<Entry> load() {
  if (Storage.exists(LEGACY_PATH)) Storage.remove(LEGACY_PATH);
  std::vector<Entry> entries;
  std::string text;
  if (!Storage.exists(PATH) || !Storage.readFileToString("RECHIDE", PATH, MAX_FILE_BYTES, text)) return entries;
  size_t pos = 0;
  while (pos < text.size() && entries.size() < MAX_ENTRIES) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t tab1 = line.find('\t');
    if (tab1 == std::string::npos || tab1 == 0) continue;
    const size_t tab2 = line.find('\t', tab1 + 1);
    Entry entry;
    entry.path = line.substr(0, tab1);
    entry.title = line.substr(tab1 + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab1 - 1);
    if (tab2 != std::string::npos) entry.author = line.substr(tab2 + 1);
    entries.push_back(std::move(entry));
  }
  return entries;
}

bool hide(const std::string& path, const std::string& title, const std::string& author) {
  std::vector<Entry> entries = load();
  if (std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.path == path; })) return true;
  if (entries.size() >= MAX_ENTRIES) entries.erase(entries.begin());
  entries.push_back(Entry{path, title, author});
  if (!save(entries)) {
    LOG_ERR("RECHIDE", "Could not save hidden recommendations");
    return false;
  }
  return true;
}

bool unhide(const std::string& path) {
  std::vector<Entry> entries = load();
  const auto it = std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.path == path; });
  if (it == entries.end()) return false;
  entries.erase(it, entries.end());
  return save(entries);
}

std::vector<uint64_t> pathHashes() {
  std::vector<uint64_t> hashes;
  for (const auto& e : load()) hashes.push_back(library::clixPathHash(e.path.data(), e.path.size()));
  return hashes;
}

}  // namespace hidden_recommendations
