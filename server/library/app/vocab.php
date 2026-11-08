<?php
// Word lists: the published decks, the decks made from books, the combined
// dictionary built from them, and the "words of this book" generator.

declare(strict_types=1);

require_once __DIR__ . '/lib.php';

// Words this common are skipped when making a deck from a book: a reader of
// English books knows them already. Rank = position in the frequency deck.
const BOOK_DECK_SKIP_RANK = 1500;
const BOOK_DECK_MAX_WORDS = 400;
const FREQUENCY_DECK = '2. 5000 từ thông dụng';

// One deck line: word, ipa, meaning, example (the trainer's accepted layouts).
function parseDeckLine(string $line): ?array {
    $line = rtrim($line, "\r\n");
    if ($line === '' || $line[0] === '#') return null;
    $cols = array_map('trim', explode("\t", $line));
    if (count($cols) < 2 || $cols[0] === '') return null;
    if (count($cols) === 2) return ['word' => $cols[0], 'ipa' => '', 'meaning' => $cols[1], 'example' => ''];
    return ['word' => $cols[0], 'ipa' => $cols[1], 'meaning' => $cols[2] ?? '', 'example' => $cols[3] ?? ''];
}

// Every deck: ['name', 'file', 'custom', 'words', 'size'].
// The published decks, the admin's ("custom") and, given a user, their own.
// 'key' names a deck in links and forms: its name, or "my/<name>" for a user's own.
function listDecks(?array $user = null): array {
    $decks = [];
    $dirs = [[PUBLIC_DECKS_DIR, false, ''], [CUSTOM_DECKS_DIR, true, '']];
    if ($user) $dirs[] = [userDir((int)$user['id'], 'decks'), true, 'my/'];
    foreach ($dirs as [$dir, $custom, $prefix]) {
        foreach (glob($dir . '/*.tsv') ?: [] as $file) {
            $name = basename($file, '.tsv');
            $decks[] = ['key' => $prefix . $name, 'name' => $name, 'file' => $file, 'custom' => $custom, 'own' => $prefix !== '',
                        'words' => deckWordCount($file), 'size' => filesize($file), 'lang' => deckLanguage($file)];
        }
    }
    usort($decks, fn($a, $b) => [$a['own'], $a['custom'], $a['name']] <=> [$b['own'], $b['custom'], $b['name']]);
    return $decks;
}

// Languages a deck can teach, as the reader's Settings > Vocabulary > Language.
const VOCAB_LANGS = ['en' => 'Tiếng Anh', 'zh' => 'Tiếng Trung'];

// "zh" when the deck says "# lang: zh" in its leading comments or its first
// word is written in Han characters, as the reader decides; otherwise "en".
function deckLanguage(string $file): string {
    $key = 'lang:' . $file . ':' . filemtime($file);
    $cached = meta($key);
    if ($cached !== null) return $cached;
    $lang = 'en';
    foreach (new SplFileObject($file) as $line) {
        if (!is_string($line)) continue;
        if (preg_match('/^#.*lang:\s*zh/u', $line)) { $lang = 'zh'; break; }
        $row = parseDeckLine($line);
        if (!$row) continue;
        if (preg_match('/[\x{3400}-\x{9FFF}\x{F900}-\x{FAFF}]/u', $row['word'])) $lang = 'zh';
        break;
    }
    meta($key, $lang);
    return $lang;
}

function deckWordCount(string $file): int {
    $key = 'count:' . $file . ':' . filemtime($file);
    $cached = meta($key);
    if ($cached !== null) return (int)$cached;
    $n = 0;
    foreach (new SplFileObject($file) as $line) {
        if (is_string($line) && parseDeckLine($line)) $n++;
    }
    meta($key, (string)$n);
    return $n;
}

function deckByName(string $key, ?array $user = null): ?array {
    foreach (listDecks($user) as $d) {
        if ($d['key'] === $key) return $d;
    }
    return null;
}

// Rebuilds the words table when a published deck changed. The frequency
// deck goes first so a word's rank is how common it is.
function rebuildDictionary(): bool {
    $files = glob(PUBLIC_DECKS_DIR . '/*.tsv') ?: [];
    usort($files, function ($a, $b) {
        $fa = basename($a, '.tsv') === FREQUENCY_DECK;
        $fb = basename($b, '.tsv') === FREQUENCY_DECK;
        return [$fb, $a] <=> [$fa, $b];
    });
    $stamp = md5(implode('|', array_map(fn($f) => $f . filemtime($f), $files)));
    if (meta('dictionary') === $stamp) return false;
    $pdo = db();
    $pdo->beginTransaction();
    $pdo->exec('DELETE FROM words');
    $insert = $pdo->prepare('INSERT OR IGNORE INTO words(word, ipa, meaning, example, rank, deck) VALUES (?,?,?,?,?,?)');
    $rank = 0;
    foreach ($files as $file) {
        $deck = basename($file, '.tsv');
        foreach (new SplFileObject($file) as $line) {
            $row = is_string($line) ? parseDeckLine($line) : null;
            if (!$row) continue;
            $insert->execute([mb_strtolower($row['word']), $row['ipa'], $row['meaning'], $row['example'], $rank++, $deck]);
        }
    }
    $pdo->commit();
    meta('dictionary', $stamp);
    return true;
}

// Words of the published decks teaching `$lang` that start with the query, or
// whose Vietnamese meaning holds it.
function lookupWords(string $query, string $lang = 'en', int $limit = 50): array {
    $q = mb_strtolower(trim($query));
    if ($q === '') return [];
    $decks = [];
    foreach (glob(PUBLIC_DECKS_DIR . '/*.tsv') ?: [] as $file) {
        if (deckLanguage($file) === $lang) $decks[] = basename($file, '.tsv');
    }
    if (!$decks) return [];
    $in = implode(',', array_fill(0, count($decks), '?'));
    $st = db()->prepare("SELECT * FROM words WHERE deck IN ($in) AND word LIKE ? ORDER BY (word = ?) DESC, length(word), rank LIMIT ?");
    $st->execute([...$decks, $q . '%', $q, $limit]);
    $rows = $st->fetchAll();
    if (count($rows) < 5) {  // Vietnamese -> English or Chinese: search the meanings too
        $st = db()->prepare("SELECT * FROM words WHERE deck IN ($in) AND meaning LIKE ? ORDER BY rank LIMIT ?");
        $st->execute([...$decks, '%' . $query . '%', $limit]);
        $rows = array_merge($rows, $st->fetchAll());
    }
    return $rows;
}

// Plain text of every XHTML document in an EPUB, in archive order.
function epubText(string $file, int $maxBytes = 6_000_000): string {
    $zip = new ZipArchive();
    if ($zip->open($file, ZipArchive::RDONLY) !== true) return '';
    $text = '';
    for ($i = 0; $i < $zip->numFiles && strlen($text) < $maxBytes; $i++) {
        $name = (string)$zip->getNameIndex($i);
        if (!preg_match('/\.(x?html?|xml)$/i', $name) || stripos($name, 'META-INF') === 0 || preg_match('/\.(opf|ncx)$/i', $name)) {
            continue;
        }
        $html = (string)$zip->getFromIndex($i);
        $html = preg_replace('#<(script|style)\b.*?</\1>#is', ' ', $html) ?? '';
        $html = preg_replace('#<(br|p|div|h\d|li)\b[^>]*>#i', "\n", $html) ?? '';
        $text .= html_entity_decode(strip_tags($html), ENT_QUOTES | ENT_HTML5, 'UTF-8') . "\n";
    }
    $zip->close();
    return $text;
}

// Inflected forms of very common words that suffix rules cannot undo.
const IRREGULAR = [
    'am' => 'be', 'is' => 'be', 'are' => 'be', 'was' => 'be', 'were' => 'be', 'been' => 'be', 'being' => 'be',
    'has' => 'have', 'had' => 'have', 'having' => 'have', 'does' => 'do', 'did' => 'do', 'done' => 'do',
    'went' => 'go', 'gone' => 'go', 'said' => 'say', 'made' => 'make', 'took' => 'take', 'taken' => 'take',
    'came' => 'come', 'saw' => 'see', 'seen' => 'see', 'knew' => 'know', 'known' => 'know', 'got' => 'get',
    'gotten' => 'get', 'gave' => 'give', 'given' => 'give', 'found' => 'find', 'thought' => 'think',
    'told' => 'tell', 'became' => 'become', 'left' => 'leave', 'felt' => 'feel', 'brought' => 'bring',
    'began' => 'begin', 'begun' => 'begin', 'kept' => 'keep', 'held' => 'hold', 'wrote' => 'write',
    'written' => 'write', 'stood' => 'stand', 'heard' => 'hear', 'meant' => 'mean', 'met' => 'meet',
    'ran' => 'run', 'paid' => 'pay', 'sat' => 'sit', 'spoke' => 'speak', 'spoken' => 'speak', 'lay' => 'lie',
    'led' => 'lead', 'grew' => 'grow', 'grown' => 'grow', 'lost' => 'lose', 'fell' => 'fall', 'fallen' => 'fall',
    'sent' => 'send', 'built' => 'build', 'understood' => 'understand', 'drew' => 'draw', 'drawn' => 'draw',
    'broke' => 'break', 'broken' => 'break', 'spent' => 'spend', 'rose' => 'rise', 'risen' => 'rise',
    'drove' => 'drive', 'driven' => 'drive', 'bought' => 'buy', 'wore' => 'wear', 'worn' => 'wear',
    'chose' => 'choose', 'chosen' => 'choose', 'caught' => 'catch', 'taught' => 'teach', 'sold' => 'sell',
    'fought' => 'fight', 'threw' => 'throw', 'thrown' => 'throw', 'ate' => 'eat', 'eaten' => 'eat',
    'slept' => 'sleep', 'woke' => 'wake', 'woken' => 'wake', 'shook' => 'shake', 'hid' => 'hide',
    'hidden' => 'hide', 'flew' => 'fly', 'flown' => 'fly', 'forgot' => 'forget', 'forgotten' => 'forget',
    'men' => 'man', 'women' => 'woman', 'children' => 'child', 'feet' => 'foot', 'teeth' => 'tooth',
    'better' => 'good', 'best' => 'good', 'worse' => 'bad', 'worst' => 'bad',
    'me' => 'i', 'mine' => 'my', 'him' => 'he', 'his' => 'he', 'hers' => 'her', 'us' => 'we', 'ours' => 'our',
    'them' => 'they', 'theirs' => 'their', 'yours' => 'your', "i'm" => 'i', "don't" => 'do', "can't" => 'can',
];

// Base forms to try for an inflected word, the word itself first.
function wordCandidates(string $w): array {
    $c = [$w];
    if (isset(IRREGULAR[$w])) $c[] = IRREGULAR[$w];
    $len = strlen($w);
    if ($len > 4 && str_ends_with($w, 'ies')) $c[] = substr($w, 0, -3) . 'y';
    if ($len > 3 && str_ends_with($w, 'es')) $c[] = substr($w, 0, -2);
    if ($len > 3 && str_ends_with($w, 's') && !str_ends_with($w, 'ss')) $c[] = substr($w, 0, -1);
    if ($len > 4 && str_ends_with($w, 'ied')) $c[] = substr($w, 0, -3) . 'y';
    if ($len > 3 && str_ends_with($w, 'ed')) {
        $stem = substr($w, 0, -2);
        $c[] = $stem;
        $c[] = $stem . 'e';
        if (strlen($stem) > 2 && $stem[-1] === $stem[-2]) $c[] = substr($stem, 0, -1);
    }
    if ($len > 5 && str_ends_with($w, 'ing')) {
        $stem = substr($w, 0, -3);
        $c[] = $stem;
        $c[] = $stem . 'e';
        if (strlen($stem) > 2 && $stem[-1] === $stem[-2]) $c[] = substr($stem, 0, -1);
    }
    if ($len > 4 && str_ends_with($w, 'ly')) $c[] = substr($w, 0, -2);
    if ($len > 4 && str_ends_with($w, 'er')) $c[] = substr($w, 0, -2);
    if ($len > 5 && str_ends_with($w, 'est')) $c[] = substr($w, 0, -3);
    return array_values(array_unique($c));
}

// The book's less common words that the dictionary knows, most frequent in
// the book first, each with an example sentence from the book. Returns the
// deck's file name, or null when the book yields nothing.
function makeBookDeck(array $book): ?string {
    rebuildDictionary();
    $text = epubText(bookFile($book));
    if ($text === '') return null;
    // Curly apostrophes too, so "haven’t" stays one token (skipped below).
    $text = str_replace(["\u{2019}", "\u{2018}"], "'", $text);
    if (!preg_match_all("/[A-Za-z]+(?:'[A-Za-z]+)*/", $text, $m)) return null;

    // Capitalised tokens are names or sentence starts: a word seen only
    // capitalised counts when it is rare (a sentence start), not when frequent (a name).
    $counts = [];
    $capitalised = [];
    foreach ($m[0] as $token) {
        if (strlen($token) < 3 || str_contains($token, "'")) continue;  // contractions are common words
        $w = strtolower($token);
        if (ctype_upper($token[0])) {
            $capitalised[$w] = ($capitalised[$w] ?? 0) + 1;
        } else {
            $counts[$w] = ($counts[$w] ?? 0) + 1;
        }
    }
    foreach ($capitalised as $w => $n) {
        if (isset($counts[$w])) {
            $counts[$w] += $n;
        } elseif ($n <= 2) {
            $counts[$w] = $n;
        }
    }

    $lookup = db()->prepare('SELECT * FROM words WHERE word = ?');
    $found = [];  // base word -> [row, count, surface]
    foreach ($counts as $w => $n) {
        // Any common reading of the word (its base form included) means the
        // reader knows it: "arms" is "arm", not "weapons".
        $pick = null;
        $common = isset(IRREGULAR[$w]);
        foreach (wordCandidates($w) as $cand) {
            $lookup->execute([$cand]);
            $row = $lookup->fetch();
            if (!$row) continue;
            if ($row['rank'] < BOOK_DECK_SKIP_RANK) {
                $common = true;
                break;
            }
            // Prefer the base form when the word itself is only an inflection.
            if ($pick === null || $cand !== $w) $pick = [$cand, $row];
        }
        if ($common || $pick === null) continue;
        [$cand, $row] = $pick;
        $found[$cand] ??= ['row' => $row, 'count' => 0, 'surface' => $w];
        $found[$cand]['count'] += $n;
    }
    if (!$found) return null;
    uasort($found, fn($a, $b) => [$b['count'], $a['row']['rank']] <=> [$a['count'], $b['row']['rank']]);
    $found = array_slice($found, 0, BOOK_DECK_MAX_WORDS, true);

    // An example sentence from the book for each word.
    $sentences = preg_split('/(?<=[.!?])\s+|\n+/u', $text) ?: [];
    $lines = ["# Words from \"{$book['title']}\" — " . count($found) . ' words, most frequent in the book first'];
    foreach ($found as $word => $f) {
        $example = $f['row']['example'];
        $pattern = '/\b' . preg_quote($f['surface'], '/') . '\b/i';
        foreach ($sentences as $s) {
            $s = trim(preg_replace('/\s+/u', ' ', $s) ?? '');
            $len = mb_strlen($s);
            if ($len >= 25 && $len <= 220 && preg_match($pattern, $s)) {
                $example = $s;
                break;
            }
        }
        $clean = fn(string $v) => str_replace(["\t", "\n", "\r"], ' ', $v);
        $lines[] = implode("\t", [$word, $clean($f['row']['ipa']), $clean($f['row']['meaning']), $clean($example)]);
    }
    if (!is_dir(CUSTOM_DECKS_DIR)) mkdir(CUSTOM_DECKS_DIR, 0750, true);
    $title = mb_strimwidth(preg_replace('/[\x00-\x1F<>:"\/\\\\|?*]/u', '', $book['title']) ?? 'Sach', 0, 70, '', 'UTF-8');
    $name = 'Sách - ' . trim($title);
    file_put_contents(CUSTOM_DECKS_DIR . '/' . $name . '.tsv', implode("\n", $lines) . "\n");
    return $name;
}
