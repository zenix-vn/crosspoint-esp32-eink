<?php
// Shared code for the CrossPoint library: storage paths, database, login,
// EPUB metadata and covers. Lives outside the web root; the pages in
// public/ include it.

declare(strict_types=1);

// Dates on the pages and in the history are Vietnam time (the server runs UTC).
date_default_timezone_set('Asia/Ho_Chi_Minh');

const SITE_ROOT = '/usr/local/lsws/crosspoint.zenix.vn';
const LIB_DATA = SITE_ROOT . '/library-data';
const BOOKS_DIR = LIB_DATA . '/books';
const COVERS_DIR = LIB_DATA . '/covers';  // before covers went public; emptied by moveCoversPublic()
const PUBLIC_COVERS_DIR = SITE_ROOT . '/html/library/covers';
const CUSTOM_DECKS_DIR = LIB_DATA . '/decks';
const DB_PATH = LIB_DATA . '/library.sqlite';
const AUTH_FILE = LIB_DATA . '/auth.json';  // the first release's single login, imported once
const GOOGLE_FILE = LIB_DATA . '/google.json';
const USERS_DIR = LIB_DATA . '/users';
const PUBLIC_DECKS_DIR = SITE_ROOT . '/html/vocab';
const INBOX_DIR = SITE_ROOT . '/inbox-data';
const INBOX_EXT = ['epub', 'txt', 'md', 'xtc', 'xtch', 'tsv'];
const BOOK_EXT = ['epub', 'pdf', 'mobi', 'azw3', 'docx', 'doc', 'txt'];
const PAGE_SIZE = 48;

// --- database -----------------------------------------------------------------

function db(): PDO {
    static $pdo = null;
    if ($pdo) return $pdo;
    $pdo = new PDO('sqlite:' . DB_PATH);
    $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    $pdo->setAttribute(PDO::ATTR_DEFAULT_FETCH_MODE, PDO::FETCH_ASSOC);
    // NORMAL is crash-safe under WAL and skips an fsync per commit.
    $pdo->exec('PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000;');
    $pdo->exec(<<<SQL
        CREATE TABLE IF NOT EXISTS books (
            id INTEGER PRIMARY KEY,
            path TEXT UNIQUE NOT NULL,
            title TEXT NOT NULL,
            author TEXT NOT NULL DEFAULT '',
            language TEXT NOT NULL DEFAULT '',
            description TEXT NOT NULL DEFAULT '',
            category TEXT NOT NULL DEFAULT '',
            subcategory TEXT NOT NULL DEFAULT '',
            format TEXT NOT NULL,
            size INTEGER NOT NULL,
            mtime INTEGER NOT NULL,
            added INTEGER NOT NULL,
            cover INTEGER NOT NULL DEFAULT 0,
            search TEXT NOT NULL DEFAULT '',
            status TEXT NOT NULL DEFAULT '',
            favorite INTEGER NOT NULL DEFAULT 0,
            tags TEXT NOT NULL DEFAULT '',
            sent INTEGER NOT NULL DEFAULT 0,
            seen INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS books_category ON books(category, subcategory);
        CREATE TABLE IF NOT EXISTS words (
            word TEXT PRIMARY KEY,
            ipa TEXT NOT NULL DEFAULT '',
            meaning TEXT NOT NULL,
            example TEXT NOT NULL DEFAULT '',
            rank INTEGER NOT NULL,
            deck TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS devices (
            id TEXT PRIMARY KEY,
            secret TEXT NOT NULL,
            name TEXT NOT NULL DEFAULT '',
            model TEXT NOT NULL DEFAULT '',
            firmware TEXT NOT NULL DEFAULT '',
            battery INTEGER NOT NULL DEFAULT -1,
            ip TEXT NOT NULL DEFAULT '',
            approved INTEGER NOT NULL DEFAULT 0,
            first_seen INTEGER NOT NULL,
            last_seen INTEGER NOT NULL
        );
    SQL);
    migrate($pdo);
    return $pdo;
}

// Schema changes after the first release, in order. PRAGMA user_version
// records how many have run; append new steps, never edit old ones.
function migrate(PDO $pdo): void {
    $steps = [
        'CREATE INDEX IF NOT EXISTS books_added ON books(added)',
        'CREATE INDEX IF NOT EXISTS books_status ON books(status, favorite)',
        "CREATE TABLE users (
            id INTEGER PRIMARY KEY,
            email TEXT UNIQUE COLLATE NOCASE,
            login TEXT UNIQUE COLLATE NOCASE,
            name TEXT NOT NULL DEFAULT '',
            hash TEXT NOT NULL DEFAULT '',
            google_sub TEXT UNIQUE,
            role TEXT NOT NULL DEFAULT 'user',
            link_key TEXT UNIQUE,
            disabled INTEGER NOT NULL DEFAULT 0,
            created INTEGER NOT NULL,
            last_login INTEGER NOT NULL DEFAULT 0
        )",
        'ALTER TABLE devices ADD COLUMN user_id INTEGER',
        "CREATE TABLE events (
            id INTEGER PRIMARY KEY,
            time INTEGER NOT NULL,
            user_id INTEGER,
            device_id TEXT,
            action TEXT NOT NULL,
            detail TEXT NOT NULL DEFAULT '',
            ip TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX events_time ON events(time);
        CREATE INDEX events_user ON events(user_id, time);
        CREATE INDEX events_ip ON events(action, ip, time)",
        'importLegacyAdmin',
        'moveCoversPublic',
        "ALTER TABLE books ADD COLUMN rights TEXT NOT NULL DEFAULT '';
        ALTER TABLE books ADD COLUMN rights_note TEXT NOT NULL DEFAULT '';
        CREATE INDEX books_rights ON books(rights)",
        // Error reports sent by the firmware (api.php?action=report). One row per
        // device and fingerprint; a repeat bumps count and last_seen.
        "CREATE TABLE error_reports (
            id INTEGER PRIMARY KEY,
            device_id TEXT NOT NULL,
            fingerprint TEXT NOT NULL,
            kind TEXT NOT NULL DEFAULT '',
            message TEXT NOT NULL DEFAULT '',
            detail TEXT NOT NULL DEFAULT '',
            firmware TEXT NOT NULL DEFAULT '',
            model TEXT NOT NULL DEFAULT '',
            count INTEGER NOT NULL DEFAULT 1,
            status TEXT NOT NULL DEFAULT 'open',
            note TEXT NOT NULL DEFAULT '',
            first_seen INTEGER NOT NULL,
            last_seen INTEGER NOT NULL,
            UNIQUE(device_id, fingerprint)
        );
        CREATE INDEX error_reports_status ON error_reports(status, last_seen)",
        // Feedback written by users on the website (interface bugs, fixes, ideas).
        "CREATE TABLE feedback (
            id INTEGER PRIMARY KEY,
            user_id INTEGER NOT NULL,
            kind TEXT NOT NULL DEFAULT 'ui',
            title TEXT NOT NULL,
            body TEXT NOT NULL,
            screen TEXT NOT NULL DEFAULT '',
            model TEXT NOT NULL DEFAULT '',
            firmware TEXT NOT NULL DEFAULT '',
            image TEXT NOT NULL DEFAULT '',
            status TEXT NOT NULL DEFAULT 'new',
            reply TEXT NOT NULL DEFAULT '',
            created INTEGER NOT NULL,
            updated INTEGER NOT NULL
        );
        CREATE INDEX feedback_user ON feedback(user_id, created);
        CREATE INDEX feedback_status ON feedback(status, created)",
    ];
    if ((int)$pdo->query('PRAGMA user_version')->fetchColumn() >= count($steps)) return;
    // One request migrates; others wait on the write lock, then see it done.
    $pdo->exec('BEGIN IMMEDIATE');
    try {
        $done = (int)$pdo->query('PRAGMA user_version')->fetchColumn();
        for ($i = $done; $i < count($steps); $i++) {
            // A step is SQL, or the name of a function given the connection.
            if (function_exists($steps[$i])) $steps[$i]($pdo);
            else $pdo->exec($steps[$i]);
            $pdo->exec('PRAGMA user_version = ' . ($i + 1));
        }
        $pdo->exec('COMMIT');
    } catch (Throwable $e) {
        $pdo->exec('ROLLBACK');
        throw $e;
    }
}

function meta(string $key, ?string $value = null): ?string {
    if ($value !== null) {
        db()->prepare('INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)')->execute([$key, $value]);
        return $value;
    }
    $st = db()->prepare('SELECT value FROM meta WHERE key = ?');
    $st->execute([$key]);
    $v = $st->fetchColumn();
    return $v === false ? null : (string)$v;
}

// Lower case without diacritics, so "tieng viet" finds "Tiếng Việt".
function fold(string $s): string {
    static $t = null;
    $t ??= Transliterator::create('Any-Latin; Latin-ASCII; Lower()');
    $s = str_replace(['đ', 'Đ'], 'd', $s);
    return $t ? (string)$t->transliterate($s) : mb_strtolower($s);
}

// "1.Ứng Dụng - Khoa Học" -> "Ứng Dụng - Khoa Học": drops sort prefixes.
function cleanFolder(string $name): string {
    return trim(preg_replace('/^(\d+|z)\.\s*/u', '', $name) ?? $name);
}

// --- accounts ---------------------------------------------------------------------
// Anyone can register (email + password, or Google) and gets no approval step;
// the admin (role "admin", the original single login) sees what they do in
// the events log. A user's data: devices, a link key, uploaded files.

const ROLE_ADMIN = 'admin';

function userById(int $id): ?array {
    $st = db()->prepare('SELECT * FROM users WHERE id = ?');
    $st->execute([$id]);
    return $st->fetch() ?: null;
}

// By email, or by the admin's login name.
function userByLogin(string $login): ?array {
    $st = db()->prepare('SELECT * FROM users WHERE email = ? OR login = ? LIMIT 1');
    $st->execute([trim($login), trim($login)]);
    return $st->fetch() ?: null;
}

function userByGoogle(string $sub): ?array {
    $st = db()->prepare('SELECT * FROM users WHERE google_sub = ?');
    $st->execute([$sub]);
    return $st->fetch() ?: null;
}

function isAdmin(?array $u): bool {
    return $u !== null && $u['role'] === ROLE_ADMIN;
}

function userLabel(array $u): string {
    return $u['name'] !== '' ? $u['name'] : ($u['email'] ?? $u['login'] ?? ('#' . $u['id']));
}

// The user whose password matches, or null. Disabled accounts never match.
function checkPassword(string $login, string $password): ?array {
    $u = userByLogin($login);
    if (!$u || $u['disabled'] || $u['hash'] === '' || !password_verify($password, $u['hash'])) return null;
    return $u;
}

function createUser(string $email, string $name, string $password, string $googleSub = ''): int {
    db()->prepare('INSERT INTO users(email, name, hash, google_sub, created) VALUES (?, ?, ?, ?, ?)')
        ->execute([mb_strtolower(trim($email)), trim(mb_substr($name, 0, 60)),
                   $password !== '' ? password_hash($password, PASSWORD_DEFAULT) : '', $googleSub !== '' ? $googleSub : null, time()]);
    return (int)db()->lastInsertId();
}

function setPassword(int $uid, string $password): void {
    db()->prepare('UPDATE users SET hash = ? WHERE id = ?')->execute([password_hash($password, PASSWORD_DEFAULT), $uid]);
}

// The admin login, for setup.php: creates or updates the admin named $user.
function setLogin(string $user, string $password): void {
    $u = userByLogin($user);
    if ($u) {
        setPassword((int)$u['id'], $password);
        return;
    }
    db()->prepare("INSERT INTO users(login, name, hash, role, created) VALUES (?, ?, ?, 'admin', ?)")
        ->execute([$user, $user, password_hash($password, PASSWORD_DEFAULT), time()]);
}

// The single login of the first release (auth.json) becomes the admin, and
// the readers it approved become the admin's devices.
function importLegacyAdmin(PDO $pdo): void {
    $raw = @file_get_contents(AUTH_FILE);
    $auth = $raw ? json_decode($raw, true) : null;
    if (!is_array($auth) || !isset($auth['user'], $auth['hash'])) return;
    $pdo->prepare("INSERT OR IGNORE INTO users(login, name, hash, role, created) VALUES (?, ?, ?, 'admin', ?)")
        ->execute([$auth['user'], $auth['user'], $auth['hash'], time()]);
    $st = $pdo->prepare('SELECT id FROM users WHERE login = ?');
    $st->execute([$auth['user']]);
    $pdo->prepare('UPDATE devices SET user_id = ? WHERE approved = 1 AND user_id IS NULL')->execute([(int)$st->fetchColumn()]);
}

// Site-wide switches on the admin page.
function flag(string $key, bool $default): bool {
    $v = meta($key);
    return $v === null ? $default : $v === '1';
}

function registrationOpen(): bool { return flag('registration_open', true); }

// Whether regular users may browse, download and send the shared collection.
function libraryOpen(): bool { return flag('library_open', true); }

function canSeeLibrary(?array $u): bool {
    return $u !== null && (isAdmin($u) || libraryOpen());
}

// PHP locks the session file for as long as it is open, so one user's parallel
// requests (a page of cover images) queue behind each other and can occupy
// every PHP worker the server shares with other sites. Requests that only read
// the session open it read-only; pages release it once the POST work is done.
function startSession(bool $readOnly = false): void {
    static $started = false;
    if ($started || session_status() === PHP_SESSION_ACTIVE) return;
    $started = true;
    session_name('cplib');
    session_set_cookie_params(['lifetime' => 60 * 60 * 24 * 90, 'path' => '/library/', 'secure' => true,
                               'httponly' => true, 'samesite' => 'Lax']);
    ini_set('session.gc_maxlifetime', (string)(60 * 60 * 24 * 90));
    session_start($readOnly ? ['read_and_close' => true] : []);
}

// Saves the session and drops its lock; $_SESSION stays readable.
function releaseSession(): void {
    csrfToken();
    if (session_status() === PHP_SESSION_ACTIVE) session_write_close();
}

function currentUser(): ?array {
    static $user = false;
    if ($user !== false) return $user;
    startSession();
    // Sessions from the single-login release carry the login name.
    if (empty($_SESSION['uid']) && !empty($_SESSION['user'])) {
        $_SESSION['uid'] = (int)(userByLogin((string)$_SESSION['user'])['id'] ?? 0);
        unset($_SESSION['user']);
    }
    $u = !empty($_SESSION['uid']) ? userById((int)$_SESSION['uid']) : null;
    if ($u && $u['disabled']) $u = null;
    return $user = $u;
}

function loggedIn(): bool {
    return currentUser() !== null;
}

function signIn(array $u, string $how): void {
    startSession();
    session_regenerate_id(true);
    $_SESSION['uid'] = (int)$u['id'];
    db()->prepare('UPDATE users SET last_login = ? WHERE id = ?')->execute([time(), $u['id']]);
    logEvent($how, '', (int)$u['id']);
}

// Basic credentials, for the e-reader's OPDS client and downloads.
function basicAuthUser(): ?array {
    $user = $_SERVER['PHP_AUTH_USER'] ?? null;
    $pass = $_SERVER['PHP_AUTH_PW'] ?? null;
    if ($user === null) {
        $header = $_SERVER['HTTP_AUTHORIZATION'] ?? $_SERVER['REDIRECT_HTTP_AUTHORIZATION'] ?? '';
        if (stripos($header, 'basic ') === 0) {
            [$user, $pass] = array_pad(explode(':', (string)base64_decode(substr($header, 6)), 2), 2, '');
        }
    }
    return $user !== null ? checkPassword((string)$user, (string)$pass) : null;
}

// The signed-in user (browser session or Basic credentials), or a 401.
function requireBasicAuth(): array {
    startSession(true);
    $u = currentUser() ?? basicAuthUser();
    if ($u) return $u;
    header('WWW-Authenticate: Basic realm="CrossPoint Library", charset="UTF-8"');
    http_response_code(401);
    echo 'Login required';
    exit;
}

// Like requireBasicAuth(), for the shared collection.
function requireLibraryAccess(): array {
    $u = requireBasicAuth();
    if (canSeeLibrary($u)) return $u;
    http_response_code(403);
    exit('The library is not open to this account');
}

function csrfToken(): string {
    static $token = null;
    if ($token !== null) return $token;
    startSession();
    $_SESSION['csrf'] ??= bin2hex(random_bytes(16));
    return $token = $_SESSION['csrf'];
}

function checkCsrf(): void {
    startSession();
    if (!hash_equals($_SESSION['csrf'] ?? '', (string)($_POST['csrf'] ?? ''))) {
        http_response_code(400);
        exit('Bad request');
    }
}

// --- events -----------------------------------------------------------------------
// The history the admin page shows: who signed up, linked which reader, sent
// or received what. Nothing needs approving; this is the record instead.

const EVENT_LABEL = [
    'register' => 'Đăng ký', 'login' => 'Đăng nhập', 'login_google' => 'Đăng nhập Google', 'login_fail' => 'Đăng nhập sai',
    'google_link' => 'Liên kết Google', 'google_unlink' => 'Bỏ liên kết Google', 'password' => 'Đổi mật khẩu', 'profile' => 'Đổi tên',
    'key_create' => 'Tạo khoá', 'key_revoke' => 'Thu hồi khoá', 'bad_key' => 'Khoá sai',
    'device_link' => 'Liên kết máy', 'device_unlink' => 'Gỡ máy', 'device_rename' => 'Đổi tên máy',
    'send' => 'Gửi tới máy', 'unsend' => 'Huỷ gửi', 'receive' => 'Máy đã nhận',
    'upload' => 'Tải lên', 'delete_file' => 'Xoá tệp', 'deck_create' => 'Tạo bộ từ',
    'user_disable' => 'Khoá tài khoản', 'user_enable' => 'Mở khoá tài khoản', 'user_delete' => 'Xoá tài khoản', 'setting' => 'Đổi cài đặt', 'feedback' => 'Góp ý',
];

const REPORT_KINDS = ['crash' => 'Treo / khởi động lại', 'error' => 'Lỗi', 'user' => 'Người dùng báo'];
const REPORTS_PER_DAY = 20;

// Stores one firmware error report. Returns false when the device has sent too many today.
function saveErrorReport(array $device, string $kind, string $message, string $detail, string $firmware, string $model): bool {
    $pdo = db();
    $kind = isset(REPORT_KINDS[$kind]) ? $kind : 'error';
    $message = mb_substr(trim($message), 0, 200);
    $detail = mb_substr($detail, 0, 16384);
    $now = time();
    $st = $pdo->prepare('SELECT COUNT(*) FROM error_reports WHERE device_id = ? AND last_seen > ?');
    $st->execute([$device['id'], $now - 86400]);
    if ((int)$st->fetchColumn() >= REPORTS_PER_DAY) return false;
    // Same device, kind, message and firmware is one problem seen again.
    $fingerprint = sha1($kind . '|' . $message . '|' . $firmware);
    $pdo->prepare("INSERT INTO error_reports(device_id, fingerprint, kind, message, detail, firmware, model, first_seen, last_seen)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                   ON CONFLICT(device_id, fingerprint) DO UPDATE SET count = count + 1, last_seen = excluded.last_seen,
                       detail = excluded.detail, status = CASE WHEN status = 'fixed' THEN 'open' ELSE status END")
        ->execute([$device['id'], $fingerprint, $kind, $message, $detail, mb_substr($firmware, 0, 40), mb_substr($model, 0, 40), $now, $now]);
    return true;
}

// --- user feedback (?p=feedback): interface bugs, feature fixes, ideas ------------------------
const FEEDBACK_KINDS = ['ui' => 'Lỗi giao diện', 'bug' => 'Lỗi tính năng', 'fix' => 'Cần sửa tính năng', 'idea' => 'Đề xuất tính năng'];
const FEEDBACK_STATUS = ['new' => 'Mới', 'seen' => 'Đã xem', 'working' => 'Đang xử lý', 'done' => 'Đã xong', 'rejected' => 'Không thực hiện'];
const FEEDBACK_PER_DAY = 10;
const FEEDBACK_IMAGE_MAX = 3 * 1024 * 1024;
const FEEDBACK_IMAGE_TYPES = [IMAGETYPE_JPEG => 'jpg', IMAGETYPE_PNG => 'png', IMAGETYPE_WEBP => 'webp', IMAGETYPE_GIF => 'gif'];

function feedbackDir(): string {
    return LIB_DATA . '/feedback';
}

function feedbackById(int $id): ?array {
    $st = db()->prepare('SELECT f.*, COALESCE(NULLIF(u.email, \'\'), u.login) AS who FROM feedback f LEFT JOIN users u ON u.id = f.user_id WHERE f.id = ?');
    $st->execute([$id]);
    return $st->fetch() ?: null;
}

// Stores one report; returns its id, or a message prefixed with "!" when refused.
// $image is a $_FILES entry (or null).
function saveFeedback(int $uid, string $kind, string $title, string $body, string $screen, string $model, string $firmware, ?array $image): int|string {
    $pdo = db();
    $title = mb_substr(trim($title), 0, 120);
    $body = mb_substr(trim($body), 0, 4000);
    if ($title === '' || mb_strlen($body) < 10) return '!Hãy nhập tiêu đề và mô tả (ít nhất 10 ký tự).';
    $st = $pdo->prepare('SELECT COUNT(*) FROM feedback WHERE user_id = ? AND created > ?');
    $st->execute([$uid, time() - 86400]);
    if ((int)$st->fetchColumn() >= FEEDBACK_PER_DAY) return '!Hôm nay bạn đã gửi quá nhiều góp ý, hãy thử lại vào ngày mai.';

    $ext = '';
    $tmp = '';
    if ($image && ($image['error'] ?? UPLOAD_ERR_NO_FILE) !== UPLOAD_ERR_NO_FILE) {
        if ($image['error'] !== UPLOAD_ERR_OK || $image['size'] > FEEDBACK_IMAGE_MAX) return '!Ảnh quá lớn (tối đa 3 MB) hoặc tải lên lỗi.';
        $info = @getimagesize($image['tmp_name']);
        $ext = $info ? (FEEDBACK_IMAGE_TYPES[$info[2]] ?? '') : '';
        if ($ext === '') return '!Chỉ nhận ảnh JPG, PNG, WebP hoặc GIF.';
        $tmp = $image['tmp_name'];
    }
    $now = time();
    $pdo->prepare('INSERT INTO feedback(user_id, kind, title, body, screen, model, firmware, image, created, updated) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)')
        ->execute([$uid, isset(FEEDBACK_KINDS[$kind]) ? $kind : 'ui', $title, $body, mb_substr(trim($screen), 0, 80),
                   mb_substr(trim($model), 0, 40), mb_substr(trim($firmware), 0, 40), $ext, $now, $now]);
    $id = (int)$pdo->lastInsertId();
    if ($tmp !== '') {
        if (!is_dir(feedbackDir())) mkdir(feedbackDir(), 0750, true);
        if (!move_uploaded_file($tmp, feedbackDir() . "/$id.$ext")) $pdo->prepare("UPDATE feedback SET image = '' WHERE id = ?")->execute([$id]);
    }
    return $id;
}

function deleteFeedback(int $id): void {
    $f = feedbackById($id);
    if (!$f) return;
    if ($f['image'] !== '') @unlink(feedbackDir() . '/' . $id . '.' . $f['image']);
    db()->prepare('DELETE FROM feedback WHERE id = ?')->execute([$id]);
}

function logEvent(string $action, string $detail = '', ?int $uid = null, ?string $deviceId = null): void {
    db()->prepare('INSERT INTO events(time, user_id, device_id, action, detail, ip) VALUES (?, ?, ?, ?, ?, ?)')
        ->execute([time(), $uid, $deviceId, $action, mb_substr($detail, 0, 300), (string)($_SERVER['REMOTE_ADDR'] ?? '')]);
}

// How often $action came from this IP in the last $seconds (for throttling).
function recentFromIp(string $action, int $seconds): int {
    $st = db()->prepare('SELECT COUNT(*) FROM events WHERE action = ? AND ip = ? AND time > ?');
    $st->execute([$action, (string)($_SERVER['REMOTE_ADDR'] ?? ''), time() - $seconds]);
    return (int)$st->fetchColumn();
}

// --- Google sign-in -------------------------------------------------------------------
// OpenID Connect code flow. Off until library-data/google.json holds
// {"client_id": ..., "client_secret": ...} of a Google Cloud OAuth client whose
// redirect URI is googleRedirectUri().

function googleConfig(): ?array {
    $raw = @file_get_contents(GOOGLE_FILE);
    $c = $raw ? json_decode($raw, true) : null;
    return is_array($c) && !empty($c['client_id']) && !empty($c['client_secret']) ? $c : null;
}

function googleRedirectUri(): string {
    return 'https://' . ($_SERVER['HTTP_HOST'] ?? 'crosspoint.zenix.vn') . '/library/index.php?p=google';
}

function googleAuthUrl(string $state): string {
    return 'https://accounts.google.com/o/oauth2/v2/auth?' . http_build_query([
        'client_id' => googleConfig()['client_id'], 'redirect_uri' => googleRedirectUri(), 'response_type' => 'code',
        'scope' => 'openid email profile', 'state' => $state, 'prompt' => 'select_account',
    ]);
}

// Trades the code for the user's verified identity: ['sub','email','name'], or null.
// The ID token comes straight from Google over TLS, so its claims are checked
// but its signature need not be (OpenID Connect Core 3.1.3.7).
function googleIdentity(string $code): ?array {
    $c = googleConfig();
    if (!$c) return null;
    $ch = curl_init('https://oauth2.googleapis.com/token');
    curl_setopt_array($ch, [CURLOPT_POST => true, CURLOPT_RETURNTRANSFER => true, CURLOPT_TIMEOUT => 15,
        CURLOPT_POSTFIELDS => http_build_query(['code' => $code, 'client_id' => $c['client_id'], 'client_secret' => $c['client_secret'],
                                                'redirect_uri' => googleRedirectUri(), 'grant_type' => 'authorization_code'])]);
    $body = curl_exec($ch);
    $ok = curl_getinfo($ch, CURLINFO_RESPONSE_CODE) === 200;
    curl_close($ch);
    $token = $ok && is_string($body) ? json_decode($body, true) : null;
    $parts = explode('.', (string)($token['id_token'] ?? ''));
    if (count($parts) !== 3) return null;
    $claims = json_decode((string)base64_decode(strtr($parts[1], '-_', '+/')), true);
    if (!is_array($claims) || ($claims['aud'] ?? '') !== $c['client_id'] || (int)($claims['exp'] ?? 0) < time() ||
        !in_array($claims['iss'] ?? '', ['accounts.google.com', 'https://accounts.google.com'], true) ||
        empty($claims['sub']) || empty($claims['email']) || ($claims['email_verified'] ?? false) !== true) {
        return null;
    }
    return ['sub' => (string)$claims['sub'], 'email' => mb_strtolower((string)$claims['email']), 'name' => (string)($claims['name'] ?? '')];
}

// --- books ------------------------------------------------------------------------

// Books still under copyright (rights = 'copyright', set by rights.php or by
// the admin) are hidden from everyone but the admin, who deletes them or
// clears the flag. '' is not reviewed yet, 'free' may be shared.
const RIGHTS_LABEL = ['' => 'Chưa rà soát', 'free' => 'Được chia sẻ', 'copyright' => 'Bản quyền'];

function visibleBooks(?array $viewer): string {
    return isAdmin($viewer) ? '1' : "rights <> 'copyright'";
}

function bookById(int $id, ?array $viewer = null): ?array {
    $st = db()->prepare('SELECT * FROM books WHERE id = ? AND ' . visibleBooks($viewer));
    $st->execute([$id]);
    $b = $st->fetch();
    return $b ?: null;
}

function bookFile(array $book): string {
    return BOOKS_DIR . '/' . $book['path'];
}

// Covers are static files the web server sends without PHP: a page of books
// asked PHP for dozens of covers at once and filled the PHP workers the server
// shares with other sites. The name carries a keyed hash, so a cover can only
// be found through a page that links it (the folder is not listable).
function coverName(int $id): string {
    static $key = null;
    $key ??= meta('cover_key') ?? meta('cover_key', bin2hex(random_bytes(16)));
    return $id . '-' . substr(hash_hmac('sha256', (string)$id, $key), 0, 20) . '.jpg';
}

function coverFile(int $id): string {
    return PUBLIC_COVERS_DIR . '/' . coverName($id);
}

// Relative to the pages in /library/.
function coverUrl(int $id): string {
    return 'covers/' . coverName($id);
}

function moveCoversPublic(PDO $pdo): void {
    if (!is_dir(PUBLIC_COVERS_DIR)) mkdir(PUBLIC_COVERS_DIR, 0755, true);
    foreach (glob(COVERS_DIR . '/*.jpg') ?: [] as $old) {
        $id = (int)basename($old, '.jpg');
        if ($id > 0) @rename($old, coverFile($id));
    }
}

// Metadata from the file name: "Title - Author.epub", minus "1." prefixes.
function nameMeta(string $path): array {
    $base = pathinfo($path, PATHINFO_FILENAME);
    $base = trim(preg_replace('/^\d+\.\s*/u', '', $base) ?? $base);
    $parts = preg_split('/\s+[-–]\s+/u', $base);
    if (count($parts) >= 2) {
        $author = trim(array_pop($parts));
        return ['title' => trim(implode(' - ', $parts)), 'author' => $author];
    }
    return ['title' => $base, 'author' => ''];
}

// Title, author, language, description and cover bytes of an EPUB.
function epubMeta(string $file): array {
    $out = ['title' => '', 'author' => '', 'language' => '', 'description' => '', 'cover' => null];
    $zip = new ZipArchive();
    if ($zip->open($file, ZipArchive::RDONLY) !== true) return $out;
    try {
        $container = $zip->getFromName('META-INF/container.xml');
        if ($container === false || !preg_match('/full-path="([^"]+)"/', $container, $m)) return $out;
        $opfPath = $m[1];
        $opf = $zip->getFromName($opfPath);
        if ($opf === false) return $out;
        $dom = new DOMDocument();
        if (!@$dom->loadXML($opf, LIBXML_NONET | LIBXML_NOERROR | LIBXML_NOWARNING)) return $out;
        $xp = new DOMXPath($dom);
        $text = fn(string $q) => trim((string)($xp->query($q)->item(0)?->textContent ?? ''));
        $out['title'] = $text('//*[local-name()="title"]');
        $out['author'] = $text('//*[local-name()="creator"]');
        $out['language'] = strtolower(substr($text('//*[local-name()="language"]'), 0, 2));
        $out['description'] = trim(html_entity_decode(strip_tags($text('//*[local-name()="description"]')),
                                                      ENT_QUOTES | ENT_HTML5));

        // Cover: EPUB 3 cover-image, EPUB 2 <meta name="cover">, or an image named like a cover.
        $href = null;
        foreach ($xp->query('//*[local-name()="item"]') as $item) {
            if (str_contains(' ' . $item->getAttribute('properties') . ' ', ' cover-image ')) {
                $href = $item->getAttribute('href');
                break;
            }
        }
        if ($href === null) {
            $coverId = $xp->query('//*[local-name()="meta"][@name="cover"]')->item(0)?->getAttribute('content');
            foreach ($xp->query('//*[local-name()="item"]') as $item) {
                $isImage = str_starts_with($item->getAttribute('media-type'), 'image/');
                if ($coverId && $item->getAttribute('id') === $coverId && $isImage) {
                    $href = $item->getAttribute('href');
                    break;
                }
                if ($href === null && $isImage && stripos($item->getAttribute('id') . $item->getAttribute('href'), 'cover') !== false) {
                    $href = $item->getAttribute('href');
                }
            }
        }
        if ($href !== null) {
            $dir = dirname($opfPath);
            $coverPath = ($dir === '.' ? '' : $dir . '/') . rawurldecode($href);
            $coverPath = normalizeZipPath($coverPath);
            $bytes = $zip->getFromName($coverPath);
            if ($bytes !== false) $out['cover'] = $bytes;
        }
    } finally {
        $zip->close();
    }
    return $out;
}

function normalizeZipPath(string $path): string {
    $parts = [];
    foreach (explode('/', $path) as $p) {
        if ($p === '' || $p === '.') continue;
        if ($p === '..') { array_pop($parts); continue; }
        $parts[] = $p;
    }
    return implode('/', $parts);
}

// A 300 px high JPEG thumbnail; false when the image cannot be read.
function writeThumb(string $bytes, string $dest): bool {
    $img = @imagecreatefromstring($bytes);
    if (!$img) return false;
    $w = imagesx($img);
    $h = imagesy($img);
    $th = 300;
    $tw = max(1, (int)round($w * $th / max(1, $h)));
    $thumb = imagecreatetruecolor($tw, $th);
    imagecopyresampled($thumb, $img, 0, 0, 0, 0, $tw, $th, $w, $h);
    $ok = imagejpeg($thumb, $dest, 82);
    imagedestroy($img);
    imagedestroy($thumb);
    return $ok;
}

// --- devices ----------------------------------------------------------------------
// Readers running this firmware register themselves (factory MAC + a secret
// they keep in NVS) on their first call to the inbox API. A reader joins an
// account when it presents that account's link key (typed on the reader, see
// server/inbox/api.php); books and word lists sent to it then wait in its
// own inbox.

const DEVICE_INBOX_DIR = INBOX_DIR . '/devices';
const DEVICE_INBOX_MAX = 200;
// No 0/O or 1/I/L: the key is typed on the reader's on-screen keyboard.
const LINK_KEY_ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';

function devicesOf(int $uid): array {
    $st = db()->prepare('SELECT * FROM devices WHERE user_id = ? ORDER BY last_seen DESC');
    $st->execute([$uid]);
    return $st->fetchAll();
}

// Every reader, with its owner's email (admin page).
function allDevices(): array {
    return db()->query('SELECT d.*, u.email AS owner_email, u.login AS owner_login FROM devices d
                        LEFT JOIN users u ON u.id = d.user_id ORDER BY d.last_seen DESC')->fetchAll();
}

function deviceById(string $id): ?array {
    $st = db()->prepare('SELECT * FROM devices WHERE id = ?');
    $st->execute([$id]);
    return $st->fetch() ?: null;
}

function validDeviceId(string $id): bool {
    return (bool)preg_match('/^[0-9a-f]{12}$/', $id);
}

function deviceInboxDir(string $id): string {
    return DEVICE_INBOX_DIR . '/' . $id;
}

// Records a call from a reader. Returns the device row, or null when the id or
// secret is malformed or the secret differs from the one first registered.
function touchDevice(string $id, string $secret, array $info): ?array {
    if (!validDeviceId($id) || !preg_match('/^[0-9a-f]{32}$/', $secret)) return null;
    $hash = hash('sha256', $secret);
    $now = time();
    $device = deviceById($id);
    if ($device && !hash_equals($device['secret'], $hash)) return null;
    // Only what the request says; a call without the info headers keeps the old values.
    $given = fn(string $k) => trim(mb_substr(rawurldecode((string)($info[$k] ?? '')), 0, 40));
    $battery = (string)($info['battery'] ?? '') === '' ? -1 : max(-1, min(100, (int)$info['battery']));
    $name = $given('name');
    // A name given on the website wins over the device's own.
    if ($device && $device['name'] !== '' && $device['user_id'] !== null) $name = $device['name'];
    $fields = [$name ?: ($device['name'] ?? ''), $given('model') ?: ($device['model'] ?? ''),
               $given('firmware') ?: ($device['firmware'] ?? ''), $battery >= 0 ? $battery : (int)($device['battery'] ?? -1),
               (string)($_SERVER['REMOTE_ADDR'] ?? ''), $now];
    if (!$device) {
        db()->prepare('INSERT INTO devices(name, model, firmware, battery, ip, last_seen, id, secret, first_seen) VALUES (?,?,?,?,?,?,?,?,?)')
            ->execute(array_merge($fields, [$id, $hash, $now]));
    } else {
        db()->prepare('UPDATE devices SET name=?, model=?, firmware=?, battery=?, ip=?, last_seen=? WHERE id=?')
            ->execute(array_merge($fields, [$id]));
    }
    return deviceById($id);
}

// "ab cd-2345" -> "ABCD2345": what a key typed on the reader is compared as.
function normalizeLinkKey(string $key): string {
    return preg_replace('/[^A-Z0-9]/', '', strtoupper($key)) ?? '';
}

function formatLinkKey(string $key): string {
    return substr($key, 0, 4) . '-' . substr($key, 4);
}

function userByLinkKey(string $key): ?array {
    $key = normalizeLinkKey($key);
    if (strlen($key) !== 8) return null;
    $st = db()->prepare('SELECT * FROM users WHERE link_key = ? AND disabled = 0');
    $st->execute([$key]);
    return $st->fetch() ?: null;
}

// A fresh key for the account; the old one stops linking new readers (readers
// already linked stay linked).
function newLinkKey(int $uid): string {
    do {
        $key = '';
        for ($i = 0; $i < 8; $i++) $key .= LINK_KEY_ALPHABET[random_int(0, strlen(LINK_KEY_ALPHABET) - 1)];
    } while (userByLinkKey($key));
    db()->prepare('UPDATE users SET link_key = ? WHERE id = ?')->execute([$key, $uid]);
    return $key;
}

function linkDevice(string $id, int $uid): void {
    db()->prepare('UPDATE devices SET user_id = ?, approved = 1 WHERE id = ?')->execute([$uid, $id]);
}

// Detaches a reader from its account and drops what was waiting for it.
// It joins an account again when it next presents a valid key.
function unlinkDevice(string $id): void {
    if (!validDeviceId($id)) return;
    foreach (glob(deviceInboxDir($id) . '/*') ?: [] as $f) @unlink($f);
    @rmdir(deviceInboxDir($id));
    db()->prepare('UPDATE devices SET user_id = NULL, approved = 0 WHERE id = ?')->execute([$id]);
}

// Forgets a device entirely; it registers afresh, even with a new secret.
function removeDevice(string $id): void {
    unlinkDevice($id);
    db()->prepare('DELETE FROM devices WHERE id = ?')->execute([$id]);
}

function renameDevice(string $id, string $name): void {
    db()->prepare('UPDATE devices SET name = ? WHERE id = ?')->execute([trim(mb_substr($name, 0, 40)), $id]);
}

function deviceLabel(array $d): string {
    return $d['name'] !== '' ? $d['name'] : 'Máy ' . strtoupper(substr($d['id'], -6));
}

// Labels for a user's readers; two of the same name (both "CrossPoint", the
// firmware default) get the end of their id added.
function deviceLabels(array $devices): array {
    $counts = array_count_values(array_map('deviceLabel', $devices));
    $out = [];
    foreach ($devices as $d) {
        $label = deviceLabel($d);
        $out[$d['id']] = $counts[$label] > 1 ? $label . ' · ' . strtoupper(substr($d['id'], -4)) : $label;
    }
    return $out;
}

// The device a user may send to or manage: their own; the admin manages any.
function ownDevice(?array $u, string $id, bool $manage = false): ?array {
    if (!$u || !validDeviceId($id)) return null;
    $d = deviceById($id);
    if (!$d) return null;
    if ((int)($d['user_id'] ?? 0) === (int)$u['id'] || ($manage && isAdmin($u))) return $d;
    return null;
}

// Deletes an account with its files; its readers are detached.
function deleteUser(int $uid): void {
    foreach (devicesOf($uid) as $d) unlinkDevice($d['id']);
    foreach (userFiles($uid) as $f) @unlink($f['file']);
    foreach (['books', 'decks', ''] as $sub) @rmdir(userDir($uid, $sub));
    db()->prepare('DELETE FROM users WHERE id = ?')->execute([$uid]);
}

// --- a user's own files -------------------------------------------------------------
// Books and word lists a user uploads to send to their readers; .tsv files are
// also listed among their word lists.

const USER_QUOTA = 500 << 20;
const USER_FILE_MAX = 100 << 20;

function userDir(int $uid, string $sub): string {
    return USERS_DIR . '/' . $uid . ($sub !== '' ? '/' . $sub : '');
}

// [['name','kind' => 'book'|'deck','file','size','mtime'], ...], newest first.
function userFiles(int $uid): array {
    $out = [];
    foreach (['book' => 'books', 'deck' => 'decks'] as $kind => $sub) {
        foreach (glob(userDir($uid, $sub) . '/*') ?: [] as $file) {
            if (!is_file($file) || str_ends_with($file, '.part')) continue;
            $out[] = ['name' => basename($file), 'kind' => $kind, 'file' => $file, 'size' => filesize($file), 'mtime' => filemtime($file)];
        }
    }
    usort($out, fn($a, $b) => $b['mtime'] <=> $a['mtime']);
    return $out;
}

function userFile(int $uid, string $name): ?array {
    foreach (userFiles($uid) as $f) {
        if ($f['name'] === $name) return $f;
    }
    return null;
}

function userUsage(int $uid): int {
    return array_sum(array_column(userFiles($uid), 'size'));
}

// Stores an uploaded file for the user. Returns the stored name, or an error
// message prefixed with "!".
function storeUserFile(int $uid, string $tmp, string $original, int $size): string {
    $name = inboxName($original);
    $ext = strtolower(pathinfo($name, PATHINFO_EXTENSION));
    if ($name === '' || !in_array($ext, INBOX_EXT, true)) return '!' . $original . ': X4 Pro không đọc được loại tệp này';
    if ($size > USER_FILE_MAX) return '!' . $original . ': lớn hơn ' . humanSize(USER_FILE_MAX);
    if (userUsage($uid) + $size > USER_QUOTA) return '!' . $original . ': hết dung lượng (' . humanSize(USER_QUOTA) . ')';
    $dir = userDir($uid, $ext === 'tsv' ? 'decks' : 'books');
    if (!is_dir($dir)) mkdir($dir, 0750, true);
    $target = $name;
    $base = pathinfo($name, PATHINFO_FILENAME);
    for ($n = 2; file_exists($dir . '/' . $target); $n++) $target = "$base ($n).$ext";
    if (!move_uploaded_file($tmp, $dir . '/' . $target)) return '!' . $original . ': không lưu được';
    chmod($dir . '/' . $target, 0640);
    return $target;
}

// --- sending to the reader -------------------------------------------------------

// The inbox's naming rules: one safe path component, never overwriting.
function inboxName(string $name): string {
    $name = basename(str_replace('\\', '/', $name));
    $name = preg_replace('/[\x00-\x1F\x7F<>:"\/\\\\|?*]/u', '_', $name) ?? '';
    $name = ltrim(trim($name), '.');
    if (strlen($name) > 120) {
        $ext = pathinfo($name, PATHINFO_EXTENSION);
        $name = mb_strcut(pathinfo($name, PATHINFO_FILENAME), 0, 110, 'UTF-8') . ($ext !== '' ? '.' . $ext : '');
    }
    return $name;
}

// Copies a file into a device's inbox (or the shared, key-opened inbox when
// $deviceId is empty); the reader collects it under More > Receive books
// (.tsv word lists go straight to /Vocab).
function sendToReader(string $file, string $name, string $deviceId = ''): ?string {
    $name = inboxName($name);
    $ext = strtolower(pathinfo($name, PATHINFO_EXTENSION));
    if ($name === '' || !in_array($ext, INBOX_EXT, true) || !is_file($file)) return null;
    $dir = $deviceId !== '' && validDeviceId($deviceId) ? deviceInboxDir($deviceId) : INBOX_DIR;
    if (!is_dir($dir)) mkdir($dir, 0750, true);
    if (count(inboxWaiting($deviceId)) >= DEVICE_INBOX_MAX) return null;
    $target = $name;
    $base = pathinfo($name, PATHINFO_FILENAME);
    if ($ext !== 'tsv') {  // a word list replaces its older copy on the reader anyway
        for ($n = 2; file_exists($dir . '/' . $target); $n++) $target = "$base ($n).$ext";
    }
    $dest = $dir . '/' . $target;
    if (!copy($file, $dest . '.part') || !rename($dest . '.part', $dest)) return null;
    chmod($dest, 0640);
    return $target;
}

// Files waiting in a device's inbox, or the shared one.
function inboxWaiting(string $deviceId = ''): array {
    $dir = $deviceId !== '' ? deviceInboxDir($deviceId) : INBOX_DIR;
    $out = [];
    foreach ((is_dir($dir) ? scandir($dir) : []) ?: [] as $f) {
        if ($f[0] === '.' || str_ends_with($f, '.part') || !is_file($dir . '/' . $f)) continue;
        $out[] = $f;
    }
    return $out;
}

// --- output helpers ---------------------------------------------------------------

function h(?string $s): string {
    return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function humanSize(int $bytes): string {
    if ($bytes >= 1 << 20) return number_format($bytes / (1 << 20), 1, ',', '.') . ' MB';
    return number_format(max(1, intdiv($bytes, 1024)), 0, ',', '.') . ' KB';
}

function sendFile(string $file, string $name, string $type = 'application/octet-stream'): never {
    if (!is_file($file)) {
        http_response_code(404);
        exit('Not found');
    }
    $size = filesize($file);
    header('Content-Type: ' . $type);
    header("Content-Disposition: attachment; filename*=UTF-8''" . rawurlencode($name));
    header('Cache-Control: private, max-age=0');
    header('Accept-Ranges: bytes');
    // One byte range (the web PDF reader asks for the pages it shows, not the whole scan).
    if (preg_match('/^bytes=(\d*)-(\d*)$/', $_SERVER['HTTP_RANGE'] ?? '', $m) && ($m[1] !== '' || $m[2] !== '')) {
        $start = $m[1] === '' ? max(0, $size - (int)$m[2]) : (int)$m[1];
        $end = $m[1] === '' || $m[2] === '' ? $size - 1 : min((int)$m[2], $size - 1);
        if ($start > $end || $start >= $size) {
            http_response_code(416);
            header("Content-Range: bytes */$size");
            exit;
        }
        http_response_code(206);
        header("Content-Range: bytes $start-$end/$size");
        header('Content-Length: ' . ($end - $start + 1));
        $fh = fopen($file, 'rb');
        fseek($fh, $start);
        for ($left = $end - $start + 1; $left > 0 && !feof($fh); $left -= 65536) {
            echo fread($fh, min(65536, $left));
        }
        fclose($fh);
        exit;
    }
    header('Content-Length: ' . $size);
    readfile($file);
    exit;
}
