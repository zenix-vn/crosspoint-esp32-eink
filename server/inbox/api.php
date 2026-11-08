<?php
// CrossPoint book inbox: books and word lists wait here until a reader collects them.
//
//   GET  ?action=hello                 -> {"linked":bool,"approved":bool}   (registers the device)
//   GET  ?action=list                  -> {"books":[{"id","name","size","uploaded"}]}
//                                         or {"books":[],"pending":true,"linked":false,"bad_key":bool}
//                                         for a device not linked to an account yet
//   GET  ?action=get&id=<id>           -> the file
//   POST action=report, kind, message, detail -> {"ok":true}   (a reader reports a crash or error)
//   POST action=delete&id=<id>         -> {"ok":true}
//   POST action=upload, files[]        -> {"ok":true,"saved":[...],"rejected":[...]}   (shared key only)
//
// Who is calling:
// - a reader sends its factory MAC and its own secret (X-Device-Id and
//   X-Device-Secret headers, or device_id / device_secret fields). The first
//   call registers it. Sent together with an account's link key (X-Inbox-Key
//   or "key", made on the library's Devices page and typed on the reader), it
//   joins that account at once and gets its own inbox. The admin's readers
//   also get the shared inbox.
// - anyone with the shared inbox key (inbox-key.txt) reaches the shared
//   inbox, which the upload page fills.
// Files live outside the web root, so these are the only ways to reach them.

declare(strict_types=1);

require_once '/usr/local/lsws/crosspoint.zenix.vn/library-app/lib.php';

const KEY_FILE = '/usr/local/lsws/crosspoint.zenix.vn/inbox-key.txt';
const MAX_BYTES = 200 * 1024 * 1024;
const MAX_FILES = 200;

function reply(int $code, array $body): never {
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    echo json_encode($body, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

function inboxKey(): string {
    $key = @file_get_contents(KEY_FILE);
    return $key === false ? '' : trim($key);
}

function param(string $header, string $field): string {
    $value = $_SERVER['HTTP_' . $header] ?? '';
    if ($value !== '') return (string)$value;
    return (string)($_POST[$field] ?? $_GET[$field] ?? '');
}

$action = (string)($_POST['action'] ?? $_GET['action'] ?? 'list');

// --- who is calling -----------------------------------------------------------------
const BAD_KEYS_PER_HOUR = 20;

$expectedKey = inboxKey();
$key = param('X_INBOX_KEY', 'key');
$byKey = $expectedKey !== '' && hash_equals($expectedKey, $key);
$device = null;
$deviceId = param('X_DEVICE_ID', 'device_id');
if ($deviceId !== '') {
    $device = touchDevice($deviceId, param('X_DEVICE_SECRET', 'device_secret'), [
        'name' => $_SERVER['HTTP_X_DEVICE_NAME'] ?? '',
        'model' => $_SERVER['HTTP_X_DEVICE_MODEL'] ?? '',
        'firmware' => $_SERVER['HTTP_X_DEVICE_FIRMWARE'] ?? '',
        'battery' => $_SERVER['HTTP_X_DEVICE_BATTERY'] ?? '',
    ]);
    // A known id with another secret: someone else, or a reader whose NVS was
    // erased. Removing the device on the website lets it register afresh.
    if ($device === null && !$byKey) reply(403, ['error' => 'device refused']);
}

$owner = $device !== null && $device['user_id'] !== null ? userById((int)$device['user_id']) : null;
$badKey = false;
if ($device !== null && $key !== '' && !$byKey) {
    $keyUser = userByLinkKey($key);
    if ($keyUser && (int)($owner['id'] ?? 0) !== (int)$keyUser['id']) {
        // The key proves the account, the secret the reader: link without approval.
        // Moving to another account drops what the old one had queued.
        if ($owner !== null) {
            unlinkDevice($device['id']);
            logEvent('device_unlink', deviceLabel($device) . ' → tài khoản khác', (int)$owner['id'], $device['id']);
        }
        linkDevice($device['id'], (int)$keyUser['id']);
        logEvent('device_link', deviceLabel($device) . ' (' . ($device['model'] ?: '?') . ')', (int)$keyUser['id'], $device['id']);
        $device = deviceById($device['id']);
        $owner = $keyUser;
    } elseif (!$keyUser && ($owner === null || $owner['disabled'])) {
        // A linked reader keeps working with an old key; only unlinked ones are told.
        if (recentFromIp('bad_key', 3600) >= BAD_KEYS_PER_HOUR) reply(429, ['error' => 'too many wrong keys']);
        logEvent('bad_key', $deviceId, null, $device['id']);
        $badKey = true;
    }
}
$linked = $owner !== null && !$owner['disabled'];

if ($action === 'hello') {
    if ($device === null) reply(400, ['error' => 'no device']);
    reply(200, ['linked' => $linked, 'approved' => $linked]);
}
// A reader reports a crash or error; it needs only its own identity, not an account.
if ($action === 'report') {
    if ($device === null) reply(400, ['error' => 'no device']);
    if ($_SERVER['REQUEST_METHOD'] !== 'POST') reply(405, ['error' => 'POST required']);
    $ok = saveErrorReport($device, (string)($_POST['kind'] ?? 'error'), (string)($_POST['message'] ?? ''),
        (string)($_POST['detail'] ?? ''), (string)($device['firmware'] ?? ''), (string)($device['model'] ?? ''));
    reply($ok ? 200 : 429, ['ok' => $ok]);
}
if (!$byKey && !$linked) {
    // "pending" keeps 2.0.7 readers on their waiting screen.
    if ($device !== null && $action === 'list') reply(200, ['books' => [], 'pending' => true, 'linked' => false, 'bad_key' => $badKey]);
    reply(403, ['error' => $device !== null ? 'device not linked' : 'bad key']);
}

// The inboxes this caller reads: "d:" its own, "s:" the shared one (the
// admin's readers and the shared key only).
$sources = [];
if ($linked) $sources['d'] = deviceInboxDir($device['id']);
if ($byKey || isAdmin($owner)) $sources['s'] = INBOX_DIR;

// "d:Name.epub" -> its path; a bare id (older readers) means the shared inbox.
function idPath(string $id, array $sources): ?string {
    [$prefix, $name] = str_contains($id, ':') ? explode(':', $id, 2) : ['s', $id];
    if (!isset($sources[$prefix]) || $name === '' || $name !== basename($name) || $name[0] === '.') return null;
    $path = $sources[$prefix] . '/' . $name;
    return is_file($path) ? $path : null;
}

if ($action === 'list') {
    $books = [];
    foreach ($sources as $prefix => $dir) {
        foreach ((is_dir($dir) ? scandir($dir) : []) ?: [] as $file) {
            if ($file[0] === '.' || str_ends_with($file, '.part')) continue;
            $path = $dir . '/' . $file;
            if (!is_file($path)) continue;
            $books[] = ['id' => $prefix . ':' . $file, 'name' => $file, 'size' => filesize($path), 'uploaded' => filemtime($path)];
        }
    }
    usort($books, fn($a, $b) => $a['uploaded'] <=> $b['uploaded']);
    reply(200, ['books' => $books]);
}

if ($action === 'get') {
    $path = idPath((string)($_GET['id'] ?? ''), $sources);
    if ($path === null) reply(404, ['error' => 'not found']);
    header('Content-Type: application/octet-stream');
    header('Content-Length: ' . filesize($path));
    header('Cache-Control: no-store');
    readfile($path);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') reply(405, ['error' => 'POST required']);

if ($action === 'delete') {
    $path = idPath((string)($_POST['id'] ?? ''), $sources);
    if ($path === null) reply(404, ['error' => 'not found']);
    // The reader deletes a file once it is safely on the card: that is the receipt.
    if ($linked) logEvent('receive', basename($path), (int)$owner['id'], $device['id']);
    reply(unlink($path) ? 200 : 500, ['ok' => !file_exists($path)]);
}

if ($action === 'upload') {
    if (!$byKey) reply(403, ['error' => 'bad key']);
    $files = $_FILES['files'] ?? null;
    if (!$files || !is_array($files['name'])) reply(400, ['error' => 'no files']);
    if (!is_dir(INBOX_DIR)) mkdir(INBOX_DIR, 0750, true);
    $existing = count(array_filter(scandir(INBOX_DIR) ?: [], fn($f) => $f[0] !== '.'));
    $saved = [];
    $rejected = [];
    foreach ($files['name'] as $i => $original) {
        $name = inboxName((string)$original);
        $ext = strtolower(pathinfo($name, PATHINFO_EXTENSION));
        if ($files['error'][$i] !== UPLOAD_ERR_OK) { $rejected[] = [$original, 'upload error ' . $files['error'][$i]]; continue; }
        if ($name === '' || !in_array($ext, INBOX_EXT, true)) { $rejected[] = [$original, 'unsupported type']; continue; }
        if ($files['size'][$i] > MAX_BYTES) { $rejected[] = [$original, 'too large']; continue; }
        if ($existing >= MAX_FILES) { $rejected[] = [$original, 'inbox full']; continue; }
        // Never overwrite a waiting book: "Book.epub" becomes "Book (2).epub".
        $target = $name;
        $base = pathinfo($name, PATHINFO_FILENAME);
        for ($n = 2; file_exists(INBOX_DIR . '/' . $target); $n++) $target = "$base ($n).$ext";
        $dest = INBOX_DIR . '/' . $target;
        if (!move_uploaded_file($files['tmp_name'][$i], $dest . '.part') || !rename($dest . '.part', $dest)) {
            $rejected[] = [$original, 'could not store'];
            continue;
        }
        chmod($dest, 0640);
        $saved[] = $target;
        $existing++;
    }
    reply(200, ['ok' => true, 'saved' => $saved, 'rejected' => $rejected]);
}

reply(400, ['error' => 'unknown action']);
