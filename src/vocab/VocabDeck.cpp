#include "VocabDeck.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>

#include <strings.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "ReadingStatsStore.h"

namespace vocab {

extern const char STARTER_DECK_NAME[];
extern const char STARTER_DECK[];
extern const char STARTER_DECK_ZH_NAME[];
extern const char STARTER_DECK_ZH[];

namespace {

constexpr char PROGRESS_DIR[] = "/.crosspoint/vocab";
constexpr char STATE_PATH[] = "/.crosspoint/vocab/state.json";
constexpr char MAGIC[4] = {'V', 'S', 'R', '1'};
constexpr uint16_t FORMAT_VERSION = 1;
constexpr size_t MAX_LINE = 1024;
constexpr size_t RECORD_SIZE = 16;
constexpr size_t HEADER_SIZE = 32;
// Records move through this many at a time: 512 bytes, one SD sector.
constexpr size_t RECORD_BATCH = 32;

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint16_t version;
  uint16_t reserved0;
  uint32_t deckSize;
  uint32_t deckTime;
  uint32_t count;
  int32_t newDay;
  uint16_t newToday;
  uint8_t reserved1[6];
};
struct Record {
  uint32_t lineOffset;
  uint32_t wordHash;
  int32_t dueDay;
  uint16_t interval;
  uint16_t easeX100;
};
#pragma pack(pop)
static_assert(sizeof(Header) == HEADER_SIZE, "vocab header must be 32 bytes");
static_assert(sizeof(Record) == RECORD_SIZE, "vocab record must be 16 bytes");

struct Span {
  const char* data;
  size_t len;
};

std::string_view trimmed(const char* data, size_t len) {
  while (len > 0 && (*data == ' ' || *data == '\r')) {
    data++;
    len--;
  }
  while (len > 0 && (data[len - 1] == ' ' || data[len - 1] == '\r')) len--;
  return std::string_view(data, len);
}

// Up to four tab-separated columns of one line, trimmed.
size_t splitColumns(const char* line, const size_t len, Span* out) {
  size_t n = 0;
  size_t start = 0;
  for (size_t i = 0; i <= len && n < 4; i++) {
    if (i == len || line[i] == '\t') {
      const std::string_view col = trimmed(line + start, i - start);
      out[n++] = Span{col.data(), col.size()};
      start = i + 1;
    }
  }
  return n;
}

// The word and meaning columns of a line, per the accepted layouts.
bool wordAndMeaning(const Span* cols, const size_t n, Span& word, Span& meaning) {
  if (n < 2) return false;
  word = cols[0];
  meaning = n == 2 ? cols[1] : cols[2];
  return word.len > 0 && meaning.len > 0 && word.data[0] != '#';
}

std::string progressPathFor(const std::string& name) { return std::string(PROGRESS_DIR) + "/" + name + ".srs"; }

bool endsWithTsv(const char* name) {
  const size_t len = strlen(name);
  return len > 4 && strcasecmp(name + len - 4, ".tsv") == 0;
}

// True when the UTF-8 text holds a Han character (U+3400-U+9FFF: lead bytes E3
// 90 and up through E9) or a CJK compatibility one (U+F900-U+FAFF: EF A4-AB).
bool hasHan(const char* data, const size_t len) {
  for (size_t i = 0; i + 1 < len; i++) {
    const auto lead = static_cast<uint8_t>(data[i]);
    const auto next = static_cast<uint8_t>(data[i + 1]);
    if ((lead >= 0xE4 && lead <= 0xE9) || (lead == 0xE3 && next >= 0x90) ||
        (lead == 0xEF && next >= 0xA4 && next <= 0xAB)) {
      return true;
    }
  }
  return false;
}

}  // namespace

Language deckLanguage(const std::string& name) {
  const std::string path = std::string(DECK_DIR) + "/" + name + ".tsv";
  HalFile file = Storage.open(path.c_str());
  if (!file) return Language::English;
  // The leading comments and the first card fit in the first 1 KB.
  char buf[MAX_LINE];
  const int got = file.read(buf, sizeof(buf));
  file.close();
  if (got <= 0) return Language::English;
  size_t start = 0;
  for (size_t i = 0; i <= static_cast<size_t>(got); i++) {
    if (i < static_cast<size_t>(got) && buf[i] != '\n') continue;
    const std::string_view line = trimmed(buf + start, i - start);
    start = i + 1;
    if (line.empty()) continue;
    if (line[0] == '#') {
      if (line.find("lang: zh") != std::string_view::npos || line.find("lang:zh") != std::string_view::npos) {
        return Language::Chinese;
      }
      continue;
    }
    const size_t tab = line.find('\t');
    const std::string_view word = line.substr(0, tab);
    return hasHan(word.data(), word.size()) ? Language::Chinese : Language::English;
  }
  return Language::English;
}

bool parseLine(const char* line, const size_t len, Card& out) {
  Span cols[4];
  const size_t n = splitColumns(line, len, cols);
  Span word{};
  Span meaning{};
  if (!wordAndMeaning(cols, n, word, meaning)) return false;
  out.word.assign(word.data, word.len);
  out.meaning.assign(meaning.data, meaning.len);
  out.ipa.clear();
  out.example.clear();
  if (n >= 3) out.ipa.assign(cols[1].data, cols[1].len);
  if (n >= 4) out.example.assign(cols[3].data, cols[3].len);
  return true;
}

std::vector<std::string> listDecks() {
  std::vector<std::string> decks;
  HalFile dir = Storage.open(DECK_DIR);
  if (!dir || !dir.isDirectory()) return decks;
  char name[128];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    if (name[0] == '.' || !endsWithTsv(name)) continue;
    decks.emplace_back(name, strlen(name) - 4);
  }
  std::sort(decks.begin(), decks.end());
  return decks;
}

std::vector<std::string> listDecks(const Language language) {
  std::vector<std::string> decks = listDecks();
  decks.erase(std::remove_if(decks.begin(), decks.end(),
                             [language](const std::string& name) { return deckLanguage(name) != language; }),
              decks.end());
  return decks;
}

std::string ensureStarterDeck(const Language language) {
  if (!listDecks(language).empty()) return {};
  const bool zh = language == Language::Chinese;
  const char* name = zh ? STARTER_DECK_ZH_NAME : STARTER_DECK_NAME;
  const char* content = zh ? STARTER_DECK_ZH : STARTER_DECK;
  Storage.ensureDirectoryExists(DECK_DIR);
  const std::string path = std::string(DECK_DIR) + "/" + name + ".tsv";
  // A deck of that name teaching the other language stays; no starter then.
  if (Storage.exists(path.c_str())) return {};
  HalFile file;
  if (!Storage.openFileForWrite("VOCAB", path.c_str(), file)) return {};
  // The embedded deck uses '|' between columns so the source stays readable.
  char buf[256];
  size_t used = 0;
  bool ok = true;
  for (const char* p = content; *p && ok; p++) {
    buf[used++] = *p == '|' ? '\t' : *p;
    if (used == sizeof(buf)) {
      ok = file.write(buf, used) == used;
      used = 0;
    }
  }
  if (ok && used > 0) ok = file.write(buf, used) == used;
  file.close();
  if (!ok) {
    Storage.remove(path.c_str());
    return {};
  }
  LOG_INF("VOCAB", "Wrote starter deck %s", path.c_str());
  return name;
}

bool Deck::open(const std::string& name) {
  close();
  deckName = name;
  tsvPath = std::string(DECK_DIR) + "/" + name + ".tsv";
  srsPath = progressPathFor(name);
  tsv = Storage.open(tsvPath.c_str());
  if (!tsv) {
    LOG_ERR("VOCAB", "Cannot open %s", tsvPath.c_str());
    return false;
  }
  const uint32_t size = static_cast<uint32_t>(tsv.fileSize());
  const uint32_t time = tsv.modificationTime();

  Header header{};
  bool fresh = false;
  {
    HalFile probe = Storage.open(srsPath.c_str());
    fresh = !probe || probe.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
            memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0 || header.version != FORMAT_VERSION ||
            header.deckSize != size || header.deckTime != time || header.count > MAX_CARDS;
  }
  if (fresh && !rebuildIndex(size, time)) return false;

  srs = Storage.open(srsPath.c_str(), O_RDWR);
  if (!srs || srs.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERR("VOCAB", "Cannot open %s", srsPath.c_str());
    close();
    return false;
  }
  count = static_cast<uint16_t>(header.count);
  newDay = header.newDay;
  newToday = header.newToday;
  deckSize = size;
  deckTime = time;
  opened = true;
  return true;
}

void Deck::close() {
  if (srs) srs.close();
  if (tsv) tsv.close();
  opened = false;
  count = 0;
}

// One pass over the .tsv writing a fresh index, carrying each card's progress
// over from the old index by word hash.
bool Deck::rebuildIndex(const uint32_t size, const uint32_t time) {
  struct Kept {
    uint32_t hash;
    CardState state;
  };
  // Old progress, sorted by hash. 12 bytes a card, released before returning;
  // without the memory the deck still opens, it just starts over.
  std::unique_ptr<Kept[]> kept;
  uint32_t keptCount = 0;
  int32_t keptNewDay = -1;
  uint16_t keptNewToday = 0;
  {
    HalFile old = Storage.open(srsPath.c_str());
    Header header{};
    if (old && old.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
        memcmp(header.magic, MAGIC, sizeof(MAGIC)) == 0 && header.version == FORMAT_VERSION &&
        header.count <= MAX_CARDS) {
      keptNewDay = header.newDay;
      keptNewToday = header.newToday;
      kept = makeUniqueNoThrow<Kept[]>(header.count > 0 ? header.count : 1);
      if (!kept) {
        LOG_ERR("VOCAB", "No memory to keep progress of %u cards", static_cast<unsigned>(header.count));
      } else {
        Record batch[RECORD_BATCH];
        uint32_t done = 0;
        while (done < header.count) {
          const uint32_t want = std::min<uint32_t>(RECORD_BATCH, header.count - done);
          if (old.read(batch, want * RECORD_SIZE) != static_cast<int>(want * RECORD_SIZE)) break;
          for (uint32_t i = 0; i < want; i++) {
            kept[keptCount++] = Kept{batch[i].wordHash, CardState{batch[i].dueDay, batch[i].interval, batch[i].easeX100}};
          }
          done += want;
        }
        std::sort(kept.get(), kept.get() + keptCount, [](const Kept& a, const Kept& b) { return a.hash < b.hash; });
      }
    }
  }

  Storage.ensureDirectoryExists(PROGRESS_DIR);
  const std::string tmpPath = srsPath + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("VOCAB", tmpPath.c_str(), out)) return false;
  Header header{};
  memcpy(header.magic, MAGIC, sizeof(MAGIC));
  header.version = FORMAT_VERSION;
  header.deckSize = size;
  header.deckTime = time;
  header.newDay = keptNewDay;
  header.newToday = keptNewToday;
  bool ok = out.write(&header, sizeof(header)) == sizeof(header);

  auto buffer = makeUniqueNoThrow<char[]>(MAX_LINE * 2);
  if (!buffer) {
    out.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  char* chunk = buffer.get();
  char* line = buffer.get() + MAX_LINE;
  size_t lineLen = 0;
  bool lineTooLong = false;
  uint32_t lineStart = 0;
  uint32_t pos = 0;
  uint32_t cards = 0;
  Record batch[RECORD_BATCH];
  size_t batched = 0;

  const auto flushBatch = [&]() {
    if (batched > 0) ok = ok && out.write(batch, batched * RECORD_SIZE) == batched * RECORD_SIZE;
    batched = 0;
  };
  const auto endLine = [&]() {
    Span cols[4];
    Span word{};
    Span meaning{};
    const size_t n = lineTooLong ? 0 : splitColumns(line, lineLen, cols);
    if (cards < MAX_CARDS && wordAndMeaning(cols, n, word, meaning)) {
      const uint32_t hash = wordHash(word.data, word.len);
      CardState state;
      if (keptCount > 0) {
        const Kept* begin = kept.get();
        const Kept* end = begin + keptCount;
        const Kept* hit =
            std::lower_bound(begin, end, hash, [](const Kept& k, const uint32_t h) { return k.hash < h; });
        if (hit != end && hit->hash == hash) state = hit->state;
      }
      batch[batched++] = Record{lineStart, hash, state.dueDay, state.interval, state.easeX100};
      cards++;
      if (batched == RECORD_BATCH) flushBatch();
    }
    lineLen = 0;
    lineTooLong = false;
  };

  // A UTF-8 byte-order mark (Notepad, Excel) is not part of the first word.
  uint8_t bom[3] = {};
  tsv.seekSet(0);
  if (tsv.read(bom, sizeof(bom)) == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
    pos = lineStart = 3;
  }
  tsv.seekSet(pos);
  int got = 0;
  while (ok && (got = tsv.read(chunk, MAX_LINE)) > 0) {
    for (int i = 0; i < got; i++, pos++) {
      const char c = chunk[i];
      if (c == '\n') {
        endLine();
        lineStart = pos + 1;
        continue;
      }
      if (lineLen < MAX_LINE) {
        line[lineLen++] = c;
      } else {
        lineTooLong = true;
      }
    }
  }
  if (lineLen > 0) endLine();
  flushBatch();
  kept.reset();

  header.count = cards;
  ok = ok && out.seekSet(0) && out.write(&header, sizeof(header)) == sizeof(header);
  out.close();
  if (!ok) {
    Storage.remove(tmpPath.c_str());
    LOG_ERR("VOCAB", "Index of %s failed", tsvPath.c_str());
    return false;
  }
  Storage.remove(srsPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), srsPath.c_str())) return false;
  LOG_INF("VOCAB", "Indexed %s: %u cards (%u with kept progress)", deckName.c_str(), static_cast<unsigned>(cards),
          static_cast<unsigned>(keptCount));
  return true;
}

bool Deck::readRecord(const uint16_t index, uint8_t* record) {
  if (!opened || index >= count) return false;
  return srs.seekSet(HEADER_SIZE + static_cast<uint32_t>(index) * RECORD_SIZE) &&
         srs.read(record, RECORD_SIZE) == static_cast<int>(RECORD_SIZE);
}

bool Deck::readCard(const uint16_t index, Card& out) {
  Record record{};
  if (!readRecord(index, reinterpret_cast<uint8_t*>(&record))) return false;
  char line[MAX_LINE];
  if (!tsv.seekSet(record.lineOffset)) return false;
  const int got = tsv.read(line, sizeof(line));
  if (got <= 0) return false;
  size_t len = 0;
  while (len < static_cast<size_t>(got) && line[len] != '\n') len++;
  return parseLine(line, len, out);
}

bool Deck::readState(const uint16_t index, CardState& out) {
  Record record{};
  if (!readRecord(index, reinterpret_cast<uint8_t*>(&record))) return false;
  out = CardState{record.dueDay, record.interval, record.easeX100};
  return true;
}

bool Deck::writeState(const uint16_t index, const CardState& state) {
  if (!opened || index >= count) return false;
  struct __attribute__((packed)) Tail {
    int32_t dueDay;
    uint16_t interval;
    uint16_t easeX100;
  } tail{state.dueDay, state.interval, state.easeX100};
  // Only the progress half of the record; the offset and hash stay as indexed.
  const bool ok = srs.seekSet(HEADER_SIZE + static_cast<uint32_t>(index) * RECORD_SIZE + 8) &&
                  srs.write(&tail, sizeof(tail)) == sizeof(tail);
  srs.flush();
  return ok;
}

Counts Deck::counts(const int32_t today) {
  Counts c;
  if (!opened || !srs.seekSet(HEADER_SIZE)) return c;
  Record batch[RECORD_BATCH];
  uint32_t done = 0;
  while (done < count) {
    const uint32_t want = std::min<uint32_t>(RECORD_BATCH, count - done);
    if (srs.read(batch, want * RECORD_SIZE) != static_cast<int>(want * RECORD_SIZE)) break;
    for (uint32_t i = 0; i < want; i++) {
      if (batch[i].dueDay == NEW_CARD) {
        c.fresh++;
      } else {
        c.learned++;
        if (batch[i].dueDay <= today) c.due++;
      }
    }
    done += want;
  }
  return c;
}

Mastery Deck::mastery(const int32_t today) {
  Mastery m;
  if (!opened || !srs.seekSet(HEADER_SIZE)) return m;
  Record batch[RECORD_BATCH];
  uint32_t done = 0;
  while (done < count) {
    const uint32_t want = std::min<uint32_t>(RECORD_BATCH, count - done);
    if (srs.read(batch, want * RECORD_SIZE) != static_cast<int>(want * RECORD_SIZE)) break;
    for (uint32_t i = 0; i < want; i++) {
      const Record& r = batch[i];
      if (r.dueDay == NEW_CARD) {
        m.fresh++;
        continue;
      }
      if (r.interval >= 21) {
        m.mastered++;
      } else if (r.interval >= 7) {
        m.familiar++;
      } else {
        m.learning++;
      }
      if (r.dueDay <= today) {
        m.dueToday++;
      } else if (r.dueDay <= today + 7) {
        m.dueWeek++;
        if (r.dueDay == today + 1) m.dueTomorrow++;
      }
    }
    done += want;
  }
  return m;
}

bool Deck::buildSession(const int32_t today, const uint16_t newLimit, const uint16_t maxReviews,
                        std::vector<uint16_t>& out) {
  out.clear();
  if (!opened || !srs.seekSet(HEADER_SIZE)) return false;
  // Two small lists of indexes; the session never exceeds maxReviews + newLimit.
  std::vector<uint16_t> fresh;
  out.reserve(std::min<uint32_t>(count, maxReviews));
  fresh.reserve(std::min<uint32_t>(count, newLimit));
  Record batch[RECORD_BATCH];
  uint32_t done = 0;
  while (done < count) {
    const uint32_t want = std::min<uint32_t>(RECORD_BATCH, count - done);
    if (srs.read(batch, want * RECORD_SIZE) != static_cast<int>(want * RECORD_SIZE)) break;
    for (uint32_t i = 0; i < want; i++) {
      const auto index = static_cast<uint16_t>(done + i);
      if (batch[i].dueDay == NEW_CARD) {
        if (fresh.size() < newLimit) fresh.push_back(index);
      } else if (batch[i].dueDay <= today && out.size() < maxReviews) {
        out.push_back(index);
      }
    }
    done += want;
  }
  out.insert(out.end(), fresh.begin(), fresh.end());
  return true;
}

bool Deck::studiedCards(const uint16_t max, std::vector<uint16_t>& out) {
  out.clear();
  if (!opened || !srs.seekSet(HEADER_SIZE)) return false;
  Record batch[RECORD_BATCH];
  uint32_t done = 0;
  while (done < count && out.size() < max) {
    const uint32_t want = std::min<uint32_t>(RECORD_BATCH, count - done);
    if (srs.read(batch, want * RECORD_SIZE) != static_cast<int>(want * RECORD_SIZE)) break;
    for (uint32_t i = 0; i < want && out.size() < max; i++) {
      if (batch[i].dueDay != NEW_CARD) out.push_back(static_cast<uint16_t>(done + i));
    }
    done += want;
  }
  return true;
}

uint16_t Deck::newIntroducedToday(const int32_t today) const { return newDay == today ? newToday : 0; }

void Deck::noteNewIntroduced(const int32_t today) {
  if (newDay != today) {
    newDay = today;
    newToday = 0;
  }
  if (newToday < UINT16_MAX) newToday++;
  writeHeader();
}

bool Deck::writeHeader() {
  if (!opened) return false;
  Header header{};
  memcpy(header.magic, MAGIC, sizeof(MAGIC));
  header.version = FORMAT_VERSION;
  header.deckSize = deckSize;
  header.deckTime = deckTime;
  header.count = count;
  header.newDay = newDay;
  header.newToday = newToday;
  const bool ok = srs.seekSet(0) && srs.write(&header, sizeof(header)) == sizeof(header);
  srs.flush();
  return ok;
}

void TrainerState::load() {
  std::string text;
  if (!Storage.exists(STATE_PATH) || !Storage.readFileToString("VOCAB", STATE_PATH, 4096, text)) return;
  JsonDocument doc;
  if (deserializeJson(doc, text)) return;
  deck = doc["deck"] | "";
  deckZh = doc["deckZh"] | "";
  lastDay = doc["lastDay"] | -1;
  streak = doc["streak"] | 0;
  best = doc["best"] | 0;
  // Each day is [day, cards, fresh, reviews, lapses, quiz, quizRight].
  for (JsonArrayConst entry : doc["days"].as<JsonArrayConst>()) {
    DayLog log;
    log.day = entry[0] | -1;
    if (log.day < 0) continue;
    log.cards = entry[1] | 0;
    log.fresh = entry[2] | 0;
    log.reviews = entry[3] | 0;
    log.lapses = entry[4] | 0;
    log.quiz = entry[5] | 0;
    log.quizRight = entry[6] | 0;
    days[log.day % HISTORY_DAYS] = log;
  }
}

bool TrainerState::save() const {
  Storage.ensureDirectoryExists(PROGRESS_DIR);
  JsonDocument doc;
  doc["deck"] = deck;
  if (!deckZh.empty()) doc["deckZh"] = deckZh;
  doc["lastDay"] = lastDay;
  doc["streak"] = streak;
  doc["best"] = best;
  JsonArray list = doc["days"].to<JsonArray>();
  for (const DayLog& log : days) {
    if (log.day < 0) continue;
    JsonArray entry = list.add<JsonArray>();
    entry.add(log.day);
    entry.add(log.cards);
    entry.add(log.fresh);
    entry.add(log.reviews);
    entry.add(log.lapses);
    entry.add(log.quiz);
    entry.add(log.quizRight);
  }
  std::string text;
  serializeJson(doc, text);
  HalFile file;
  if (!Storage.openFileForWrite("VOCAB", STATE_PATH, file)) return false;
  const bool ok = file.write(text.data(), text.size()) == text.size();
  file.close();
  return ok;
}

void TrainerState::noteStudied(const int32_t today) {
  if (today == lastDay) return;
  streak = today == lastDay + 1 ? static_cast<uint16_t>(streak + 1) : 1;
  best = std::max(best, streak);
  lastDay = today;
}

namespace {

DayLog& slotFor(std::array<DayLog, HISTORY_DAYS>& days, const int32_t today) {
  DayLog& log = days[today % HISTORY_DAYS];
  if (log.day != today) log = DayLog{today};
  return log;
}

uint16_t bump(const uint16_t v) { return v < UINT16_MAX ? static_cast<uint16_t>(v + 1) : v; }

}  // namespace

void TrainerState::noteCard(const int32_t today, const bool fresh, const bool review, const bool lapse) {
  if (today < 0) return;
  DayLog& log = slotFor(days, today);
  log.cards = bump(log.cards);
  if (fresh) log.fresh = bump(log.fresh);
  if (review) log.reviews = bump(log.reviews);
  if (lapse) log.lapses = bump(log.lapses);
}

void TrainerState::noteQuiz(const int32_t today, const bool right) {
  if (today < 0) return;
  DayLog& log = slotFor(days, today);
  log.quiz = bump(log.quiz);
  if (right) log.quizRight = bump(log.quizRight);
}

DayLog TrainerState::dayLog(const int32_t day) const {
  if (day < 0) return DayLog{};
  const DayLog& log = days[day % HISTORY_DAYS];
  return log.day == day ? log : DayLog{};
}

WeekSummary TrainerState::week(const int32_t today) const {
  WeekSummary w;
  for (int i = 0; i < HISTORY_DAYS && today - i >= 0; i++) {
    const DayLog log = dayLog(today - i);
    w.cards += log.cards;
    w.fresh += log.fresh;
    w.reviews += log.reviews;
    w.lapses += log.lapses;
    w.quiz += log.quiz;
    w.quizRight += log.quizRight;
    if (log.cards > 0 || log.quiz > 0) w.activeDays++;
  }
  return w;
}

int weekScore(const WeekSummary& week) {
  if (week.activeDays == 0) return -1;
  const int regularity = week.activeDays * 100 / HISTORY_DAYS;
  // Recall: reviews remembered and quiz answers right, whichever there are.
  int recallSum = 0;
  int recallParts = 0;
  if (week.reviews > 0) {
    recallSum += static_cast<int>((week.reviews - week.lapses) * 100 / week.reviews);
    recallParts++;
  }
  if (week.quiz > 0) {
    recallSum += static_cast<int>(week.quizRight * 100 / week.quiz);
    recallParts++;
  }
  if (recallParts == 0) return regularity;
  return (regularity * 4 + recallSum / recallParts * 6) / 10;
}

uint16_t TrainerState::currentStreak(const int32_t today) const {
  return lastDay >= 0 && (today == lastDay || today == lastDay + 1) ? streak : 0;
}

int32_t studyDay(const TrainerState& state) {
  const int32_t today = ReadingStatsStore::today();
  if (today >= 0) return today;
  return state.lastDay >= 0 ? state.lastDay : 0;
}

}  // namespace vocab
