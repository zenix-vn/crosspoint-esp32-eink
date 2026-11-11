<?php
// JSON API for the Windows reader, with the same HTTP Basic credentials as OPDS.
//
//   GET  api.php?a=me                          the account and its readers
//   POST api.php?a=send        to, file, name  a book into one of the account's readers
//   GET  api.php?a=lookup&q=<word>             dictionary entries for a word as written in a book
//   GET  api.php?a=words                       the account's saved words
//   POST api.php?a=saveword    word, ipa, meaning, example
//   POST api.php?a=deleteword  word
//   POST api.php?a=senddeck    to              the saved words to a reader, as a .tsv deck
//
// Saved words live in the account's own deck "Từ đã lưu.tsv", so the website's
// Vocabulary page lists them with the user's other word lists.

declare(strict_types=1);

require_once __DIR__ . '/../../library-app/vocab.php';

const SAVED_DECK = 'Từ đã lưu';
const SAVED_MAX_WORDS = 5000;

function reply(array $body, int $code = 200): never {
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    echo json_encode($body, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

function fail(string $message, int $code = 400): never {
    reply(['error' => $message], $code);
}

function requirePost(): void {
    if ($_SERVER['REQUEST_METHOD'] !== 'POST') fail('POST required', 405);
}

// --- saved words ---------------------------------------------------------------------

function savedDeckFile(array $u): string {
    return userDir((int)$u['id'], 'decks') . '/' . SAVED_DECK . '.tsv';
}

// Lowercase word => ['word','ipa','meaning','example'], in the order saved.
function readSaved(array $u): array {
    $file = savedDeckFile($u);
    $rows = [];
    if (!is_file($file)) return $rows;
    foreach (new SplFileObject($file) as $line) {
        $row = is_string($line) ? parseDeckLine($line) : null;
        if ($row) $rows[mb_strtolower($row['word'])] = $row;
    }
    return $rows;
}

function cell(string $s): string {
    return trim(preg_replace('/[\t\r\n]+/u', ' ', $s) ?? '');
}

// Rewrites the deck under a lock; the reader and the website read the same file.
function writeSaved(array $u, array $rows): void {
    $file = savedDeckFile($u);
    $dir = dirname($file);
    if (!is_dir($dir)) mkdir($dir, 0750, true);
    $text = "# lang: en\n# Từ lưu khi đọc sách trên CrossPoint Reader cho Windows\n";
    foreach ($rows as $r) {
        $text .= cell($r['word']) . "\t" . cell($r['ipa']) . "\t" . cell($r['meaning']) . "\t" . cell($r['example']) . "\n";
    }
    $fh = fopen($file . '.lock', 'c');
    flock($fh, LOCK_EX);
    file_put_contents($file . '.part', $text);
    rename($file . '.part', $file);
    chmod($file, 0640);
    flock($fh, LOCK_UN);
    fclose($fh);
}

// --- dictionary ----------------------------------------------------------------------

function englishDecks(): array {
    $decks = [];
    foreach (glob(PUBLIC_DECKS_DIR . '/*.tsv') ?: [] as $file) {
        if (deckLanguage($file) === 'en') $decks[] = basename($file, '.tsv');
    }
    return $decks;
}

// Entries for a word as it appears in a sentence: the word itself, then its
// base forms ("running" -> "run"), then words that start with it.
function lookup(string $query): array {
    $w = mb_strtolower(trim($query, " \t\n\r\0\x0B.,;:!?\"'“”‘’()[]{}—–-"));
    $w = str_replace(['’', '‘'], "'", $w);
    if ($w === '' || mb_strlen($w) > 60) return [];
    $decks = englishDecks();
    if (!$decks) return [];
    $cands = str_contains($w, ' ') ? [$w] : wordCandidates($w);
    $inDecks = implode(',', array_fill(0, count($decks), '?'));
    $inCands = implode(',', array_fill(0, count($cands), '?'));
    $st = db()->prepare("SELECT word, ipa, meaning, example, deck, rank FROM words
                         WHERE deck IN ($inDecks) AND word IN ($inCands) ORDER BY rank LIMIT 6");
    $st->execute([...$decks, ...$cands]);
    $rows = $st->fetchAll();
    // The word as written first, then base forms by how common they are.
    usort($rows, fn($a, $b) => [$a['word'] !== $w, (int)$a['rank']] <=> [$b['word'] !== $w, (int)$b['rank']]);
    if (!$rows) $rows = array_slice(lookupWords($w, 'en', 6), 0, 6);
    $seen = [];
    $out = [];
    foreach ($rows as $r) {
        $key = $r['word'] . '|' . $r['meaning'];
        if (isset($seen[$key])) continue;
        $seen[$key] = true;
        $out[] = ['word' => $r['word'], 'ipa' => $r['ipa'], 'meaning' => $r['meaning'], 'example' => $r['example'], 'deck' => $r['deck']];
    }
    return $out;
}

// --- requests ------------------------------------------------------------------------

$user = requireBasicAuth();
releaseSession();
$action = (string)($_GET['a'] ?? '');

switch ($action) {
    case 'me':
        $devices = [];
        foreach (devicesOf((int)$user['id']) as $d) {
            $devices[] = [
                'id' => $d['id'], 'name' => deviceLabel($d), 'model' => (string)$d['model'],
                'firmware' => (string)$d['firmware'], 'battery' => (int)$d['battery'],
                'lastSeen' => (int)$d['last_seen'], 'waiting' => count(inboxWaiting($d['id'])),
            ];
        }
        reply(['name' => userLabel($user), 'admin' => isAdmin($user), 'devices' => $devices]);

    case 'send':
        requirePost();
        $to = (string)($_POST['to'] ?? '');
        $device = ownDevice($user, $to);
        if (!$device) fail('Không tìm thấy máy đọc này trong tài khoản.', 404);
        $file = $_FILES['file'] ?? null;
        if (!$file || $file['error'] !== UPLOAD_ERR_OK) fail('Không nhận được tệp.');
        if ($file['size'] > USER_FILE_MAX) fail('Tệp lớn hơn ' . humanSize(USER_FILE_MAX) . '.');
        $name = inboxName((string)($_POST['name'] ?? $file['name']));
        if ($name === '' || !in_array(strtolower(pathinfo($name, PATHINFO_EXTENSION)), INBOX_EXT, true)) {
            fail('Máy đọc không mở được loại tệp này.');
        }
        $sent = sendToReader($file['tmp_name'], $name, $to);
        if (!$sent) fail('Hộp thư của máy đọc đã đầy, hãy nhận sách trên máy trước.', 409);
        logEvent('send', $sent . ' → ' . deviceLabel($device), (int)$user['id'], $to);
        reply(['ok' => true, 'name' => $sent, 'device' => deviceLabel($device)]);

    case 'lookup':
        $saved = readSaved($user);
        $entries = lookup((string)($_GET['q'] ?? ''));
        foreach ($entries as &$e) $e['saved'] = isset($saved[mb_strtolower($e['word'])]);
        unset($e);
        reply(['query' => (string)($_GET['q'] ?? ''), 'entries' => $entries]);

    case 'words':
        reply(['deck' => SAVED_DECK, 'words' => array_values(readSaved($user))]);

    case 'saveword':
        requirePost();
        $word = cell((string)($_POST['word'] ?? ''));
        $meaning = cell((string)($_POST['meaning'] ?? ''));
        if ($word === '' || $meaning === '' || mb_strlen($word) > 60) fail('Thiếu từ hoặc nghĩa.');
        $rows = readSaved($user);
        if (count($rows) >= SAVED_MAX_WORDS && !isset($rows[mb_strtolower($word)])) fail('Bộ từ đã đủ ' . SAVED_MAX_WORDS . ' từ.');
        unset($rows[mb_strtolower($word)]);  // saved again: moves to the end, newest
        $rows[mb_strtolower($word)] = [
            'word' => $word, 'ipa' => cell((string)($_POST['ipa'] ?? '')), 'meaning' => mb_substr($meaning, 0, 400),
            'example' => mb_substr(cell((string)($_POST['example'] ?? '')), 0, 300),
        ];
        writeSaved($user, $rows);
        reply(['ok' => true, 'count' => count($rows)]);

    case 'deleteword':
        requirePost();
        $rows = readSaved($user);
        unset($rows[mb_strtolower(cell((string)($_POST['word'] ?? '')))]);
        writeSaved($user, $rows);
        reply(['ok' => true, 'count' => count($rows)]);

    case 'senddeck':
        requirePost();
        $to = (string)($_POST['to'] ?? '');
        $device = ownDevice($user, $to);
        if (!$device) fail('Không tìm thấy máy đọc này trong tài khoản.', 404);
        $file = savedDeckFile($user);
        if (!is_file($file)) fail('Chưa có từ nào được lưu.');
        $sent = sendToReader($file, SAVED_DECK . '.tsv', $to);
        if (!$sent) fail('Hộp thư của máy đọc đã đầy, hãy nhận sách trên máy trước.', 409);
        logEvent('send', $sent . ' → ' . deviceLabel($device), (int)$user['id'], $to);
        reply(['ok' => true, 'device' => deviceLabel($device)]);
}

fail('unknown action', 404);
