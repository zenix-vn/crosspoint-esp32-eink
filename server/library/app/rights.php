<?php
// Reviews the copyright of books not reviewed yet (rights = ''). A book may be
// shared only when it is a Project Gutenberg or Wikisource edition, or when
// every author died in 1975 or earlier (Vietnam protects a work for 50 years
// after the author's death) and the book is in the author's own language: a
// translation belongs to its translator as well. Everything else, a book
// without a known author included, is flagged 'copyright' and hidden from all
// but the admin. Authors' death dates and languages come from Wikidata and are
// cached in library-data/fetch/authors.json. Run from cron:
//   50 * * * * php /usr/local/lsws/crosspoint.zenix.vn/library-app/rights.php

declare(strict_types=1);

require_once __DIR__ . '/lib.php';

if (PHP_SAPI !== 'cli') exit;

const LAST_FREE_DEATH = 1975;   // died this year or earlier: out of copyright in Vietnam
const FIRST_FREE_DEATH = 1800;  // earlier: a namesake, or a text read in translation
// Occupations (P106) of people whose books these are: writer, novelist, poet,
// journalist, essayist, playwright, historian, translator, author, short story
// writer, children's writer, philosopher, diarist, literary critic, screenwriter.
const WD_WRITERS = ['Q36180', 'Q6625963', 'Q49757', 'Q1930187', 'Q11774202', 'Q214917', 'Q201788', 'Q333634',
                    'Q482980', 'Q15949613', 'Q4853732', 'Q4964182', 'Q18939491', 'Q4263842', 'Q28389'];
const AUTHOR_CACHE = LIB_DATA . '/fetch/authors.json';
const WD_API = 'https://www.wikidata.org/w/api.php?';
const WD_AGENT = 'CrossPointLibrary/1.0 (+https://crosspoint.zenix.vn/library/)';
const NO_AUTHOR = ['', 'khong ro', 'unknown', 'nhieu tac gia', 'khuyet danh', 'vo danh', 'various', 'anonymous', 'tap the tac gia'];
// Wikidata languages -> the codes books carry.
const WD_LANGS = ['Q1860' => 'en', 'Q9199' => 'vi', 'Q150' => 'fr', 'Q188' => 'de', 'Q7737' => 'ru', 'Q5287' => 'ja',
                  'Q7850' => 'zh', 'Q727694' => 'zh', 'Q1321' => 'es', 'Q652' => 'it', 'Q5146' => 'pt'];

$lock = fopen(LIB_DATA . '/rights.lock', 'c');
if (!$lock || !flock($lock, LOCK_EX | LOCK_NB)) exit;

$cache = is_file(AUTHOR_CACHE) ? (json_decode((string)file_get_contents(AUTHOR_CACHE), true) ?: []) : [];
$pdo = db();
$set = $pdo->prepare('UPDATE books SET rights = ?, rights_note = ? WHERE id = ?');
// --dry N: print the verdict for N random unreviewed books, change nothing.
$dry = ($argv[1] ?? '') === '--dry' ? max(1, (int)($argv[2] ?? 20)) : 0;
$books = $pdo->query("SELECT id, path, title, author, language, format FROM books WHERE rights = ''"
                     . ($dry ? " ORDER BY random() LIMIT $dry" : ''))->fetchAll();
$count = ['free' => 0, 'copyright' => 0];
foreach ($books as $i => $b) {
    [$rights, $note] = review($b, $cache);
    if ($dry) echo str_pad($rights, 10) . "{$b['title']} | {$b['author']} | $note\n";
    else $set->execute([$rights, $note, $b['id']]);
    $count[$rights]++;
    if ($i % 50 === 49) file_put_contents(AUTHOR_CACHE, json_encode($cache, JSON_UNESCAPED_UNICODE));
}
file_put_contents(AUTHOR_CACHE, json_encode($cache, JSON_UNESCAPED_UNICODE));
echo date('c') . " reviewed " . count($books) . ": {$count['free']} free, {$count['copyright']} copyright\n";

// [rights, note] for one book.
function review(array $b, array &$cache): array {
    $source = freeEdition(BOOKS_DIR . '/' . $b['path'], $b['format']);
    if ($source) return ['free', "Ấn bản $source"];
    $names = authorNames($b['author']);
    if (!$names) return ['copyright', 'Không rõ tác giả'];
    // EPUB language tags are often wrong (Vietnamese translations tagged "en"), so
    // only the English shelf counts as English.
    $lang = str_contains(fold($b['path']), 'tieng anh') ? (substr(strtolower($b['language']), 0, 2) ?: 'en') : 'vi';
    $deaths = [];
    foreach ($names as $name) {
        $a = $cache[$name] ??= lookupAuthor($name) ?? false;
        if (!$a) return ['copyright', "Không xác định được tác giả \"$name\""];
        if (!$a['death']) return ['copyright', "{$a['label']} chưa mất (hoặc không rõ năm mất)"];
        if ($a['death'] > LAST_FREE_DEATH) {
            return ['copyright', "{$a['label']} mất năm {$a['death']}, còn bản quyền đến hết năm " . ($a['death'] + 50)];
        }
        // A namesake is the usual mistake: an old king or scholar sharing a modern
        // writer's name. Modern editions of pre-1800 texts are translations anyway.
        if (!$a['writer'] || $a['death'] < FIRST_FREE_DEATH) {
            return ['copyright', "Tên trùng với {$a['label']} (mất năm {$a['death']}), chưa chắc là cùng người"];
        }
        if (!in_array($lang, $a['langs'], true)) {
            return ['copyright', "{$a['label']} mất năm {$a['death']} nhưng đây là bản dịch; bản dịch thuộc bản quyền người dịch"];
        }
        $deaths[] = "{$a['label']} mất năm {$a['death']}";
    }
    return ['free', implode('; ', $deaths) . ' (Wikidata)'];
}

// "Project Gutenberg" or "Wikisource" when the EPUB says it is their edition.
function freeEdition(string $file, string $format): string {
    if ($format !== 'epub') return '';
    $zip = new ZipArchive();
    if ($zip->open($file) !== true) return '';
    $found = '';
    for ($i = 0; $i < $zip->numFiles && $found === ''; $i++) {
        if (!str_ends_with(strtolower((string)$zip->getNameIndex($i)), '.opf')) continue;
        $opf = (string)$zip->getFromIndex($i);
        $meta = substr($opf, 0, (int)(strpos($opf, '</metadata>') ?: strlen($opf)));
        if (preg_match('~<dc:(?:identifier|source)[^>]*>[^<]*gutenberg\.org~i', $meta)) $found = 'Project Gutenberg';
        elseif (preg_match('~<dc:(?:identifier|source)[^>]*>[^<]*wikisource\.org~i', $meta)) $found = 'Wikisource';
    }
    $zip->close();
    return $found;
}

// "Snicket, Lemony" -> ["Lemony Snicket"]; "A, B và C" -> ["A", "B", "C"]; [] when unknown.
function authorNames(string $author): array {
    $author = trim(preg_replace('/\s*\(.*?\)|\s*\[.*?\]/u', '', $author) ?? $author);
    if (in_array(fold($author), NO_AUTHOR, true) || str_starts_with(fold($author), 'nxb')) return [];
    $parts = array_values(array_filter(array_map('trim',
        preg_split('/\s*(?:[;&]|,|\s+và\s+|\s+and\s+)\s*/u', $author) ?: [])));
    if (count($parts) === 2 && !str_contains($parts[0], ' ') && str_contains($author, ',')) {
        $parts = ["$parts[1] $parts[0]"];  // "Last, First"
    }
    return array_values(array_filter($parts, fn($p) => !in_array(fold($p), NO_AUTHOR, true)));
}

// The person on Wikidata whose name matches: label, death year (0 = alive or unknown), writing languages.
function lookupAuthor(string $name): ?array {
    $key = nameKey($name);
    foreach (['vi', 'en'] as $lang) {
        $found = wd(['action' => 'wbsearchentities', 'search' => $name, 'language' => $lang, 'uselang' => $lang,
                     'type' => 'item', 'limit' => 7]);
        $ids = array_column($found['search'] ?? [], 'id');
        if (!$ids) continue;
        $data = wd(['action' => 'wbgetentities', 'ids' => implode('|', $ids), 'props' => 'claims|labels|aliases',
                    'languages' => 'vi|en|mul']);
        foreach ($ids as $id) {
            $e = $data['entities'][$id] ?? null;
            if (!$e || !in_array('Q5', claimIds($e, 'P31'), true)) continue;  // a person
            $names = array_column($e['labels'] ?? [], 'value');
            foreach ($e['aliases'] ?? [] as $list) $names = array_merge($names, array_column($list, 'value'));
            if (!in_array($key, array_map('nameKey', $names), true)) continue;
            $death = 0;
            foreach ($e['claims']['P570'] ?? [] as $c) {
                $t = $c['mainsnak']['datavalue']['value']['time'] ?? '';
                if (preg_match('/^[+-](\d+)-/', $t, $m)) $death = max($death, (int)$m[1]);
            }
            $langs = [];
            foreach (array_merge(claimIds($e, 'P1412'), claimIds($e, 'P103'), claimIds($e, 'P6886')) as $q) {
                if (isset(WD_LANGS[$q])) $langs[] = WD_LANGS[$q];
            }
            if (!in_array('vi', $langs, true) && vietnamese(claimIds($e, 'P27'))) $langs[] = 'vi';
            $label = $e['labels']['vi']['value'] ?? $e['labels']['en']['value'] ?? $name;
            return ['id' => $id, 'label' => $label, 'death' => $death, 'langs' => array_values(array_unique($langs)),
                    'writer' => (bool)array_intersect(claimIds($e, 'P106'), WD_WRITERS)];
        }
    }
    return null;
}

// True when a citizenship is Vietnam or one of its earlier states.
function vietnamese(array $countries): bool {
    static $known = [];
    $ask = array_diff($countries, array_keys($known));
    if ($ask) {
        $data = wd(['action' => 'wbgetentities', 'ids' => implode('|', array_slice($ask, 0, 50)), 'props' => 'labels',
                    'languages' => 'en']);
        foreach ($ask as $q) {
            $label = $data['entities'][$q]['labels']['en']['value'] ?? '';
            $known[$q] = (bool)preg_match('/Vietnam|Viet Nam|Indochina|Annam|Tonkin|Cochinchina|Nguyễn|Đại Việt|Dai Viet/iu', $label);
        }
    }
    foreach ($countries as $q) if (!empty($known[$q])) return true;
    return false;
}

function claimIds(array $entity, string $prop): array {
    $ids = [];
    foreach ($entity['claims'][$prop] ?? [] as $c) {
        $id = $c['mainsnak']['datavalue']['value']['id'] ?? null;
        if ($id) $ids[] = $id;
    }
    return $ids;
}

// Lowercase letters with their accents kept: "Tô Như" is not "Tố Như".
function nameKey(string $name): string {
    $name = Normalizer::normalize($name, Normalizer::FORM_C) ?: $name;
    return preg_replace('/[^\p{L}\p{N}]+/u', '', mb_strtolower($name)) ?? '';
}

function wd(array $q): array {
    usleep(300_000);  // stay well inside Wikidata's limits
    $ch = curl_init(WD_API . http_build_query($q + ['format' => 'json']));
    curl_setopt_array($ch, [CURLOPT_RETURNTRANSFER => true, CURLOPT_TIMEOUT => 60, CURLOPT_USERAGENT => WD_AGENT]);
    for ($try = 0; $try < 3; $try++) {
        $body = curl_exec($ch);
        if (is_string($body) && curl_getinfo($ch, CURLINFO_RESPONSE_CODE) === 200) return json_decode($body, true) ?: [];
        sleep(5);
    }
    throw new RuntimeException('Wikidata unreachable');  // stop the run; the rest is reviewed next time
}
