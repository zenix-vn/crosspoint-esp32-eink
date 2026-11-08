# Vocabulary trainer

A daily English vocabulary trainer built into the reader, modelled on the
pocket "word card" devices (Leaise and similar): pick a word list, learn a few
new words a day, and review old ones on a spaced-repetition schedule.

It is reached from the **Vocabulary** tile in the Home dock. (Settings moved to
the bottom of the **More** menu to make room.)

## What it does

- **Learn**: a flash card per word. The front shows the word and its phonetic
  transcription (IPA). Tap the card, or press Confirm, to see the meaning and an
  example. Then grade how well you knew it: **Again / Hard / Good / Easy**.
- **Spaced repetition**: each grade sets when the word comes back (SM-2, the
  algorithm behind Anki and the "Ebbinghaus mode" of word-card devices).
  Forgotten words come back in the same session; known words come back after
  1, 3, 8, 20... days.
- **Daily session**: all words that are due, plus a number of new words a day
  (Settings → Vocabulary → *New words per day*, default 20).
- **Quiz**: four-choice questions, word → meaning, from the words already
  learned. A wrong answer sends the word back to be relearned today.
- **Word lists**: any number of decks on the SD card; choose one under
  Settings → Vocabulary → *Choose word list*. The trainer itself shows only
  Study, Quiz and Statistics.
- **Streak**: consecutive days with at least one card studied, shown on the
  trainer's start screen.
- **Statistics**: a page rating the last seven days, laid out like the reading
  statistics page:
  - a 0-100 score: 40 % regularity (days studied of the last 7) and 60 %
    recall (reviews not graded *Again*, and quiz answers right, averaged),
    shown as *Excellent* (85+), *Good* (70+), *Fair* (50+) or *Needs work*,
    with one piece of advice (clear a backlog of more than 50 due words, learn
    fewer new words when recall is under 70 %, study more regularly, try the
    quiz);
  - streak and best streak, cards and new words this week, recall rate and
    quiz accuracy;
  - word mastery for the open deck by current interval: *learning* (under a
    week), *familiar* (1-3 weeks), *mastered* (3 weeks or more), *not started*,
    and how many words fall due today, tomorrow and in the next 7 days;
  - a 7-day chart of cards per day, new words solid and reviews shaded.

There is no audio (the X4 Pro has no speaker); pronunciation is the IPA line,
drawn with a built-in font that carries the IPA glyphs (`src/IpaFont.h`).

## Chinese

Settings → Vocabulary → *Language* switches the trainer between **English**
and **Chinese**. Each language keeps its own chosen word list (*Choose word
list* shows only the decks of the current language); the streak and the
statistics count both.

A deck is Chinese when a leading comment says `# lang: zh`, or when its first
word is written in Han characters. A Chinese deck uses the same four columns:

```
学习<TAB>xué xí<TAB>học; tìm hiểu<TAB>Hán Việt: học tập
```

The pinyin goes in the phonetic column (the IPA font carries the tone marks);
the fourth column holds the Sino-Vietnamese reading in the published decks.

The word is drawn with a built-in Noto Sans SC 18 subset holding the 2 968 Han
characters of the HSK decks (`src/HanFont.h`, about 380 KB of flash), so the
HSK decks need no font on the SD card. When the reader's SD-card font has Han
characters (Settings → Reader → Text settings; see
[sd-card-fonts.md](sd-card-fonts.md)) the trainer uses it instead: it covers
any character, for decks beyond HSK, and also backs the UI sizes, so Chinese
text quoted in meanings renders too. Both are drawn regular, not bold.

On first open with no Chinese deck, the trainer writes `/Vocab/HSK 1.tsv`, the
150 most frequent HSK 1 words. The library's full *HSK 1* deck has the same
name, so receiving it replaces the starter and keeps the progress.

`scripts/vocab/build_zh_decks.py` builds one deck per HSK 3.0 level (HSK 1 to
6 and 7-9, about 10 900 words), most frequent words first. Levels, frequency,
pinyin and traditional forms come from
[complete-hsk-vocabulary](https://github.com/drkameleon/complete-hsk-vocabulary)
(MIT); Vietnamese meanings from [CVDICT](https://github.com/ph0ngp/CVDICT)
(CC BY-SA 4.0, machine-translated from CC-CEDICT); Sino-Vietnamese readings
from [hanviet-pinyin-wordlist](https://github.com/ph0ngp/hanviet-pinyin-wordlist)
(MIT). For words with several readings the script takes the one a learner meets
first (都 dōu rather than the surname Dū, 了 le rather than liǎo).

```
python scripts/vocab/build_zh_decks.py <data-dir> <out-dir>
```

## Word list format

Decks are UTF-8 tab-separated text files in `/Vocab/` on the SD card, one word
per line:

```
word<TAB>ipa<TAB>meaning<TAB>example
```

| Column | Required | Example |
| --- | --- | --- |
| word | yes | `achieve` |
| ipa | no | `/əˈtʃiːv/` |
| meaning | yes | `đạt được, giành được` |
| example | no | `She achieved her goal of running a marathon.` |

- Lines starting with `#` and empty lines are ignored.
- A line is at most 1 KB; longer lines are skipped.
- The file name, without `.tsv`, is the deck name shown on the device.
- A deck can hold up to 20 000 words.

On first open, if `/Vocab/` has no deck, the trainer writes a starter deck
(`/Vocab/Common words.tsv`, about 200 everyday words with Vietnamese meanings).

Decks can be copied with File transfer, or sent from anywhere through the book
inbox (`https://crosspoint.zenix.vn/inbox/`, then *More → Receive books*).
The inbox puts `.tsv` files straight into `/Vocab`, replacing a deck of the same
name (progress is kept by word); other files land in `/Inbox`.

## Word-list library

`scripts/vocab/build_decks.py` builds nine decks, most frequent words first, so
the daily new words are always the most useful ones left:

| Deck | Words | Selection |
| --- | --- | --- |
| 1. Oxford 3000 | ~3 300 | Oxford 3000 core words |
| 2. 5000 từ thông dụng | 5 000 | most frequent words |
| 3. Tiếng Anh THCS | ~1 500 | lower secondary school |
| 4. Tiếng Anh THPT | ~3 500 | upper secondary / university entrance |
| 5. Trung cấp | ~3 800 | intermediate (CET-4 list) |
| 6. Nâng cao | ~5 300 | upper intermediate (CET-6 list) |
| 7. IELTS | ~4 700 | IELTS |
| 8. TOEFL | ~6 800 | TOEFL |
| 9. GRE học thuật | ~7 300 | advanced academic |

Word lists, levels and frequencies come from
[ECDICT](https://github.com/skywind3000/ECDICT); Vietnamese meanings, phonetics
and examples from the OVDP English–Vietnamese dictionary
([stardict-vi](https://github.com/dynamotn/stardict-vi)). For each word the
script picks the general (not specialist) entry, keeps up to three senses from
the first two parts of speech, skips archaic and slang senses, and takes the
first example with its translation. Old-style transcriptions are normalised to
IPA (`'` → `ˈ`, `:` → `ː`).

```
python scripts/vocab/build_decks.py <data-dir> <out-dir>
```

Editing a deck keeps your progress: cards are matched by their word, so
reordering lines, fixing a meaning or adding words does not reset anything.
A word whose spelling changes starts over as a new card.

## Storage

Progress for each deck lives in `/.crosspoint/vocab/<deck>.srs`; the streak and
the chosen decks in `/.crosspoint/vocab/state.json` (`deck` for English,
`deckZh` for Chinese).
`state.json` also keeps the last seven days of study, across all decks, as
`days`: `[day, cards, new, reviews, lapses, quiz, quizRight]` per day.

The `.srs` file is a 32-byte header followed by one 16-byte record per card, in
deck line order:

| Field | Type | Meaning |
| --- | --- | --- |
| `lineOffset` | u32 | byte offset of the card's line in the `.tsv` |
| `wordHash` | u32 | FNV-1a of the word, to keep progress across edits |
| `dueDay` | i32 | local day the card is due, `-1` = never studied |
| `interval` | u16 | current interval in days |
| `easeX100` | u16 | SM-2 ease factor × 100 (130–300) |

The header records the deck's size and modification time. When either changes,
the index is rebuilt in one pass over the deck, and progress is carried over by
matching word hashes.

Why this layout: the ESP32-C3 boards (X3/X4) have about 380 KB of RAM and no
PSRAM, so the trainer never loads a deck into memory. A session holds only the
indexes of the cards it will show (at most a few hundred `uint16_t`); each card
is read from the SD card when shown, through its record's `lineOffset`.

## Scheduling (SM-2 variant)

Days are local calendar days (`ReadingStatsStore::today()`). Without a set
clock the trainer keeps using the last day it saw, so cards still come due but
intervals only advance on days the clock is known.

| Grade | New card | Card in review |
| --- | --- | --- |
| Again | shown again later this session | interval 1, ease −0.20, shown again this session |
| Hard | interval 1 | interval ×1.2, ease −0.15 |
| Good | interval 1 | interval × ease |
| Easy | interval 4 | interval × ease × 1.3, ease +0.15 |

Every step adds at least one day; ease stays within 1.30–3.00; intervals are
capped at 10 years. A card relearned after Again counts as new when graded
again in the same session. The math is in `lib/Vocab/VocabSrs.h`, covered by
`test/vocab_srs`.

## Code map

| Path | Role |
| --- | --- |
| `lib/Vocab/VocabSrs.h` | Scheduling math (pure, host-tested) |
| `src/vocab/VocabDeck.{h,cpp}` | Deck discovery, `.tsv` parsing, `.srs` index and progress, session queue, starter deck |
| `src/activities/vocab/VocabActivity.{h,cpp}` | Start screen, deck picker, flash cards, quiz |
| `src/IpaFont.h`, `lib/EpdFont/builtinFonts/notosans_ipa_12_regular.h` | IPA-capable font |
| `src/components/icons/vocab.h` | Dock icon, from `scripts/vocab/make_vocab_icon.py` |
| `scripts/vocab/build_decks.py` | Builds the word-list library |
| `scripts/vocab/build_zh_decks.py` | Builds the Chinese HSK decks |
| `src/vocab/VocabStarterDeckZh.cpp` | Chinese starter deck (first 150 lines of *HSK 1*) |
| `src/HanFont.h`, `lib/EpdFont/builtinFonts/notosans_sc_18_hsk.h` | Built-in Han font for the HSK characters (`scripts/vocab/han_intervals.txt`) |

## Possible next steps

- Collect the words looked up in the dictionary while reading into a
  "Looked-up words" deck.
- Show a due word on the sleep screen.
- A Home card with today's counts.
