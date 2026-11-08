<?php
// Grows the collection with freely licensed EPUBs, a few per run, into
// library-data/books where the indexer picks them up:
//   - Project Gutenberg: the most downloaded English books (public domain),
//     into "z.Sách tiếng Anh/<shelf>"
//   - Vietnamese Wikisource: novels, short stories and other prose, exported
//     by ws-export, into "Văn Học/Việt Nam/Wikisource"
// Books whose title is already in the library are skipped. Progress is kept in
// library-data/fetch/state.json. Run from cron:
//   5,25,45 * * * * php /usr/local/lsws/crosspoint.zenix.vn/library-app/fetcher.php

declare(strict_types=1);

require_once __DIR__ . '/lib.php';

if (PHP_SAPI !== 'cli') exit;

const FETCH_DIR = LIB_DATA . '/fetch';
const FETCH_STATE = FETCH_DIR . '/state.json';
const PG_CATALOG = FETCH_DIR . '/pg_catalog.csv.gz';
const PG_TOP = 1000;           // how far down the Gutenberg popularity list to go
const PG_PER_RUN = 30;
const WS_PER_RUN = 10;
const FETCH_DELAY = 3;         // seconds between requests to one site
const FETCH_MAX_BYTES = 60_000_000;
const FETCH_TRIES = 3;
const FETCH_AGENT = 'CrossPointLibrary/1.0 (+https://crosspoint.zenix.vn/library/)';
const WS_CATEGORIES = ['Tiểu thuyết', 'Truyện ngắn Việt Nam', 'Truyện ngắn', 'Văn học Việt Nam',
                       'Tác phẩm chọn lọc', 'Phóng sự'];

// Gutenberg "Category:" shelves -> the English collection's subfolders; first match wins.
const PG_SHELVES = [
    'Children & Young Adult Reading' => 'Children (Thiếu nhi)',
    'Science-Fiction & Fantasy' => 'Science Fiction & Fantasy (Viễn tưởng)',
    'Crime, Thrillers and Mystery' => 'Mystery & Thriller (Trinh thám)',
    'Poetry' => 'Poetry (Thơ)',
    'Plays/Films/Dramas' => 'Drama (Kịch)',
    'Biographies' => 'Biography & Autobiography (Tiểu sử, Hồi ký)',
    'Philosophy & Ethics' => 'Philosophy & Religion (Triết học, Tôn giáo)',
    'Religion/Spirituality' => 'Philosophy & Religion (Triết học, Tôn giáo)',
    'Short Stories' => 'Short Stories (Truyện ngắn)',
    'Novels' => 'Novel (Tiểu thuyết)',
    'Historical Novels' => 'Novel (Tiểu thuyết)',
    'Adventure' => 'Novel (Tiểu thuyết)',
    'Romance' => 'Novel (Tiểu thuyết)',
    'Humour' => 'Novel (Tiểu thuyết)',
    'History - ' => 'History (Lịch sử)',
    'Science - ' => 'Science (Khoa học)',
    'Essays, Letters & Speeches' => 'Essays (Tiểu luận)',
];
const PG_DEFAULT_SHELF = 'Classics (Kinh điển)';

$lock = fopen(LIB_DATA . '/fetcher.lock', 'c');
if (!$lock || !flock($lock, LOCK_EX | LOCK_NB)) exit;  // a previous run is still going
if (!is_dir(FETCH_DIR)) mkdir(FETCH_DIR, 0750, true);

$state = is_file(FETCH_STATE) ? (json_decode((string)file_get_contents(FETCH_STATE), true) ?: []) : [];
$state += ['done' => [], 'pg_next' => 1];

// Normalized titles already in the library, so the same book is not fetched twice.
$have = [];
foreach (db()->query('SELECT title FROM books') as $row) $have[titleKey($row['title'])] = true;

$got = fetchGutenberg($state, $have) + fetchWikisource($state, $have);
file_put_contents(FETCH_STATE, json_encode($state, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT));
echo date('c') . " fetched: $got\n";

// --- sources --------------------------------------------------------------------

function fetchGutenberg(array &$state, array &$have): int {
    if (!refreshCatalog()) return 0;
    $catalog = loadCatalog();
    if (!$catalog) return 0;  // an unreadable catalog must not mark every book skipped
    $got = $tried = 0;
    $fetch = function (string $id) use (&$state, &$have, &$got, &$tried, $catalog): void {
        $key = "pg:$id";
        $book = $catalog[$id] ?? null;
        if (!$book) { $state['done'][$key] = 'skip'; return; }
        if (isset($have[titleKey($book['title'])])) { $state['done'][$key] = 'dup'; return; }
        $tried++;
        sleep(FETCH_DELAY);
        $dir = BOOKS_DIR . '/z.Sách tiếng Anh/' . shelfOf($book['shelves']);
        // The edition with images, or the plain one when that is too big (some run to 80+ MB).
        $ok = saveEpub("https://www.gutenberg.org/cache/epub/$id/pg$id-images-3.epub", $dir,
                       $book['title'], $book['author'])
           || saveEpub("https://www.gutenberg.org/cache/epub/$id/pg$id.epub", $dir, $book['title'], $book['author']);
        if ($ok) {
            $state['done'][$key] = 'ok';
            $have[titleKey($book['title'])] = true;
            $got++;
        } else {
            failed($state, $key);  // retried at the start of later runs
        }
    };
    foreach ($state['done'] as $key => $s) {
        if ($tried >= PG_PER_RUN) return $got;
        if (str_starts_with($key, 'pg:') && str_starts_with($s, 'fail:') && retryable($state, $key)) {
            $fetch(substr($key, 3));
        }
    }
    while ($tried < PG_PER_RUN && $state['pg_next'] <= PG_TOP) {
        $feed = httpGet('https://www.gutenberg.org/ebooks/search.opds/?query=l.en&sort_order=downloads&start_index='
                        . $state['pg_next']);
        if ($feed === null || !preg_match_all('~<id>https?://www\.gutenberg\.org/ebooks/(\d+)\.opds</id>~', $feed, $m)) {
            return $got;  // try again next run
        }
        foreach (array_unique($m[1]) as $id) {
            if (isset($state['done']["pg:$id"])) continue;
            if ($tried >= PG_PER_RUN) return $got;  // the rest of this page next run
            $fetch($id);
        }
        $state['pg_next'] += 25;
        sleep(FETCH_DELAY);
    }
    return $got;
}

function fetchWikisource(array &$state, array &$have): int {
    $titles = [];
    foreach (WS_CATEGORIES as $cat) {
        foreach (categoryMembers($cat) as $t) $titles[$t] = true;
    }
    $got = $tried = 0;
    foreach (array_keys($titles) as $title) {
        $key = "ws:$title";
        if (!retryable($state, $key)) continue;
        if ($tried >= WS_PER_RUN) break;
        if (isset($have[titleKey($title)])) { $state['done'][$key] = 'dup'; continue; }
        $tried++;
        sleep(FETCH_DELAY);
        $url = 'https://ws-export.wmcloud.org/?' . http_build_query(
            ['format' => 'epub-3', 'lang' => 'vi', 'page' => str_replace(' ', '_', $title)]);
        // The author comes from the EPUB's metadata when the indexer reads it.
        if (saveEpub($url, BOOKS_DIR . '/Văn Học/Việt Nam/Wikisource', $title, '')) {
            $state['done'][$key] = 'ok';
            $have[titleKey($title)] = true;
            $got++;
        } else {
            failed($state, $key);
        }
    }
    return $got;
}

// Top-level works (no subpages) of a Wikisource category.
function categoryMembers(string $cat): array {
    $titles = [];
    $cont = [];
    do {
        $q = ['action' => 'query', 'list' => 'categorymembers', 'cmtitle' => "Thể loại:$cat", 'cmnamespace' => 0,
              'cmlimit' => 500, 'format' => 'json'] + $cont;
        $data = json_decode((string)httpGet('https://vi.wikisource.org/w/api.php?' . http_build_query($q)), true);
        if (!is_array($data)) break;
        foreach ($data['query']['categorymembers'] ?? [] as $m) {
            if (!str_contains($m['title'], '/')) $titles[] = $m['title'];
        }
        $cont = $data['continue'] ?? [];
    } while ($cont);
    return $titles;
}

// --- Gutenberg catalog ----------------------------------------------------------

// The catalog CSV (titles, authors, shelves), refreshed weekly.
function refreshCatalog(): bool {
    if (is_file(PG_CATALOG) && filemtime(PG_CATALOG) > time() - 7 * 86400) return true;
    $data = httpGet('https://www.gutenberg.org/cache/epub/feeds/pg_catalog.csv.gz');
    if ($data === null || strlen($data) < 1_000_000) return is_file(PG_CATALOG);
    file_put_contents(PG_CATALOG . '.part', $data);
    return rename(PG_CATALOG . '.part', PG_CATALOG);
}

// English texts by id: title, first author as "First Last", shelves.
function loadCatalog(): array {
    $books = [];
    $f = gzopen(PG_CATALOG, 'r');
    if (!$f) return [];
    $head = fgetcsv($f, null, ',', '"', '');
    while (($row = fgetcsv($f, null, ',', '"', '')) !== false) {
        $r = @array_combine($head, $row);
        if (!$r || $r['Type'] !== 'Text' || $r['Language'] !== 'en') continue;
        $title = trim(strtok(str_replace("\r", '', $r['Title']), "\n"));
        $author = trim(explode(';', $r['Authors'])[0]);
        $author = preg_replace('/,\s*(?:BCE?\s*)?\d.*$/u', '', $author) ?? $author;  // drop the dates
        if (preg_match('/^([^,]+),\s*([^,]+)/u', $author, $m)) $author = "$m[2] $m[1]";  // drops "Sir" and the like
        $author = preg_replace('/\s*\[.*?\]|\s*\(.*?\)/u', '', $author) ?? $author;
        $books[$r['Text#']] = ['title' => $title, 'author' => trim($author), 'shelves' => $r['Bookshelves']];
    }
    gzclose($f);
    return $books;
}

function shelfOf(string $shelves): string {
    $cats = [];
    foreach (explode('; ', $shelves) as $s) {
        if (str_starts_with($s, 'Category: ')) $cats[] = substr($s, 10);
    }
    foreach (PG_SHELVES as $prefix => $folder) {
        foreach ($cats as $c) {
            if (str_starts_with($c, $prefix)) return $folder;
        }
    }
    return PG_DEFAULT_SHELF;
}

// --- helpers --------------------------------------------------------------------

// "Frankenstein; Or, The Modern Prometheus" and "Frankenstein" match.
function titleKey(string $title): string {
    $main = preg_split('/[;:(\[]/u', $title)[0];
    return preg_replace('/[^a-z0-9]+/', '', fold($main)) ?? '';
}

function retryable(array $state, string $key): bool {
    $s = $state['done'][$key] ?? '';
    return $s === '' || (str_starts_with($s, 'fail:') && (int)substr($s, 5) < FETCH_TRIES);
}

function failed(array &$state, string $key): void {
    $n = (int)substr($state['done'][$key] ?? 'fail:0', 5) + 1;
    $state['done'][$key] = "fail:$n";
}

function httpGet(string $url): ?string {
    $ch = curl_init($url);
    curl_setopt_array($ch, [
        CURLOPT_RETURNTRANSFER => true, CURLOPT_FOLLOWLOCATION => true, CURLOPT_MAXREDIRS => 5,
        CURLOPT_CONNECTTIMEOUT => 20, CURLOPT_TIMEOUT => 300, CURLOPT_USERAGENT => FETCH_AGENT,
        CURLOPT_PROTOCOLS => CURLPROTO_HTTPS, CURLOPT_REDIR_PROTOCOLS => CURLPROTO_HTTPS,
        CURLOPT_MAXFILESIZE => FETCH_MAX_BYTES,
    ]);
    $body = curl_exec($ch);
    $code = curl_getinfo($ch, CURLINFO_RESPONSE_CODE);
    if (!is_string($body) || $code !== 200) {
        echo date('c') . " $code $url\n";
        return null;
    }
    return $body;
}

// Downloads an EPUB to "<dir>/<Title> - <Author>.epub" if it is a real EPUB.
function saveEpub(string $url, string $dir, string $title, string $author): bool {
    $data = httpGet($url);
    if ($data === null || substr($data, 0, 2) !== 'PK') return false;
    if (!is_dir($dir)) mkdir($dir, 0750, true);
    $name = safeName($author !== '' ? "$title - $author" : $title);
    $dest = "$dir/$name.epub";
    for ($i = 2; is_file($dest); $i++) $dest = "$dir/$name ($i).epub";
    $part = "$dest.part";  // not a book extension, so the indexer ignores it
    file_put_contents($part, $data);
    $zip = new ZipArchive();
    $ok = $zip->open($part) === true && $zip->getFromName('mimetype') === 'application/epub+zip';
    if ($ok) $zip->close();
    if (!$ok || !rename($part, $dest)) {
        @unlink($part);
        echo date('c') . " not an EPUB: $url\n";
        return false;
    }
    echo date('c') . " saved " . substr($dest, strlen(BOOKS_DIR) + 1) . "\n";
    return true;
}

function safeName(string $s): string {
    $s = preg_replace('/[\/\\\\:*?"<>|\x00-\x1f]+/u', ' ', $s) ?? $s;
    $s = trim(preg_replace('/\s+/u', ' ', $s) ?? $s, ' .');
    return mb_substr($s, 0, 150) ?: 'Untitled';
}
