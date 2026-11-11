<?php
// CrossPoint library: anyone can register (email or Google), link their
// readers with a key they make themselves, upload books and word lists, and
// send them to their readers. The admin also curates the shared collection
// (which regular users see only when it is opened to them) and sees the
// history of what everyone does.

declare(strict_types=1);

require_once __DIR__ . '/../../library-app/vocab.php';

const WELCOME = 'Chào mừng! Bấm "Tạo khoá liên kết" rồi nhập khoá trên máy đọc sách (Thêm → Nhận sách).';

$page = (string)($_GET['p'] ?? '');
startSession();
$flash = $_SESSION['flash'] ?? '';
unset($_SESSION['flash']);

function flashTo(string $msg, string $url = 'index.php'): never {
    $_SESSION['flash'] = $msg;
    header('Location: ' . $url);
    exit;
}

// --- signing in -------------------------------------------------------------------
if ($page === 'logout') {
    $_SESSION = [];
    session_destroy();
    header('Location: index.php');
    exit;
}
if ($page === 'google') googleFlow();

$me = currentUser();
if (!$me) {
    authPages($page);
    exit;
}

// Google sign-in: ?p=google&start=1 goes to Google (with &link=1 from the
// account page, to attach Google to the signed-in account); Google returns to
// ?p=google with a code.
function googleFlow(): never {
    if (!googleConfig()) flashTo('Đăng nhập bằng Google chưa được bật.');
    if (isset($_GET['start'])) {
        $_SESSION['gstate'] = bin2hex(random_bytes(16));
        $_SESSION['glink'] = !empty($_GET['link']) && loggedIn();
        header('Location: ' . googleAuthUrl($_SESSION['gstate']));
        exit;
    }
    $state = (string)($_SESSION['gstate'] ?? '');
    $link = !empty($_SESSION['glink']);
    unset($_SESSION['gstate'], $_SESSION['glink']);
    if ($state === '' || !hash_equals($state, (string)($_GET['state'] ?? '')) || empty($_GET['code'])) {
        flashTo('Đăng nhập bằng Google không thành công, hãy thử lại.');
    }
    $id = googleIdentity((string)$_GET['code']);
    if (!$id) flashTo('Google không xác nhận được tài khoản (cần email đã xác minh).');
    $bySub = userByGoogle($id['sub']);
    $me = currentUser();
    if ($link && $me) {
        if ($bySub && (int)$bySub['id'] !== (int)$me['id']) flashTo('Tài khoản Google này đã gắn với một tài khoản khác.', 'index.php?p=profile');
        db()->prepare('UPDATE users SET google_sub = ? WHERE id = ?')->execute([$id['sub'], $me['id']]);
        logEvent('google_link', $id['email'], (int)$me['id']);
        flashTo('Đã liên kết với Google (' . $id['email'] . ').', 'index.php?p=profile');
    }
    $u = $bySub;
    $new = false;
    if (!$u) {
        // Registering needs no email check, so an address already taken by a
        // password account is not joined to Google here: whoever registered
        // it might not own it.
        if (userByLogin($id['email'])) {
            flashTo('Email ' . $id['email'] . ' đã có tài khoản. Hãy đăng nhập bằng mật khẩu rồi bấm "Liên kết Google" ở trang Tài khoản.');
        }
        if (!registrationOpen()) flashTo('Hiện chưa mở đăng ký tài khoản mới.');
        $uid = createUser($id['email'], $id['name'], '', $id['sub']);
        logEvent('register', 'Google: ' . $id['email'], $uid);
        $u = userById($uid);
        $new = true;
    }
    if ($u['disabled']) flashTo('Tài khoản này đã bị khoá.');
    signIn($u, 'login_google');
    flashTo($new ? WELCOME : '', $new ? 'index.php?p=device' : 'index.php');
}

// Sign-in and registration forms, for visitors.
function authPages(string $page): void {
    $error = '';
    $old = fn(string $k) => h((string)($_POST[$k] ?? ''));
    if ($page === 'register' && $_SERVER['REQUEST_METHOD'] === 'POST') {
        $email = mb_strtolower(trim((string)($_POST['email'] ?? '')));
        $name = trim((string)($_POST['name'] ?? ''));
        $password = (string)($_POST['password'] ?? '');
        if (!registrationOpen()) $error = 'Hiện chưa mở đăng ký tài khoản mới.';
        elseif ((string)($_POST['website'] ?? '') !== '') $error = 'Không đăng ký được.';  // filled only by bots
        elseif (recentFromIp('register', 3600) >= 5) $error = 'Có quá nhiều lượt đăng ký từ mạng này, hãy thử lại sau.';
        elseif ($name === '') $error = 'Hãy nhập tên của bạn.';
        elseif (!filter_var($email, FILTER_VALIDATE_EMAIL) || strlen($email) > 120) $error = 'Email không hợp lệ.';
        elseif (userByLogin($email)) $error = 'Email này đã có tài khoản. Hãy đăng nhập.';
        elseif (mb_strlen($password) < 8) $error = 'Mật khẩu cần ít nhất 8 ký tự.';
        elseif ($password !== (string)($_POST['confirm'] ?? '')) $error = 'Hai lần nhập mật khẩu không khớp.';
        else {
            $uid = createUser($email, $name, $password);
            logEvent('register', $email, $uid);
            signIn(userById($uid), 'login');
            flashTo(WELCOME, 'index.php?p=device');
        }
    } elseif ($page !== 'register' && $_SERVER['REQUEST_METHOD'] === 'POST') {
        $login = trim((string)($_POST['user'] ?? ''));
        usleep(400000);  // slows down password guessing
        if (recentFromIp('login_fail', 900) >= 10) {
            $error = 'Sai quá nhiều lần. Hãy thử lại sau 15 phút.';
        } elseif ($u = checkPassword($login, (string)($_POST['password'] ?? ''))) {
            signIn($u, 'login');
            $here = (string)($_SERVER['REQUEST_URI'] ?? 'index.php');
            header('Location: ' . (str_contains($here, 'p=register') ? 'index.php' : $here));
            exit;
        } else {
            logEvent('login_fail', mb_substr($login, 0, 80));
            $error = 'Sai email hoặc mật khẩu.';
        }
    }
    $google = googleConfig() !== null;
    $open = registrationOpen();
    $title = $page === 'register' ? 'Đăng ký' : 'Đăng nhập';
    layout($title, '', function () use ($page, $error, $old, $google, $open) { ?>
        <div class="login card">
          <h1>Thư viện CrossPoint</h1>
          <p class="hint">Gửi sách và bộ từ vựng tới máy đọc sách X4 Pro của bạn qua Wi-Fi.</p>
          <?php if ($error): ?><p class="error"><?= h($error) ?></p><?php endif ?>
          <?php if ($page === 'register'): ?>
            <?php if (!$open): ?><p>Hiện chưa mở đăng ký tài khoản mới.</p><?php else: ?>
            <form method="post" action="?p=register">
              <label>Tên của bạn <input name="name" value="<?= $old('name') ?>" autocomplete="name" maxlength="60" required autofocus></label>
              <label>Email <input name="email" type="email" value="<?= $old('email') ?>" autocomplete="email" required></label>
              <label>Mật khẩu (ít nhất 8 ký tự) <input name="password" type="password" autocomplete="new-password" minlength="8" required></label>
              <label>Nhập lại mật khẩu <input name="confirm" type="password" autocomplete="new-password" minlength="8" required></label>
              <label class="hp" aria-hidden="true">Website <input name="website" tabindex="-1" autocomplete="off"></label>
              <button class="primary wide">Tạo tài khoản</button>
            </form>
            <?php endif ?>
          <?php else: ?>
            <form method="post">
              <label>Email <input name="user" value="<?= $old('user') ?>" autocomplete="username" required autofocus></label>
              <label>Mật khẩu <input name="password" type="password" autocomplete="current-password" required></label>
              <button class="primary wide">Đăng nhập</button>
            </form>
          <?php endif ?>
          <?php if ($google && ($open || $page !== 'register')): ?>
            <div class="or"><span>hoặc</span></div>
            <a class="button wide google" href="?p=google&amp;start=1"><?= googleMark() ?> Tiếp tục với Google</a>
          <?php endif ?>
          <p class="switch">
            <?php if ($page === 'register'): ?>Đã có tài khoản? <a href="index.php">Đăng nhập</a>
            <?php elseif ($open): ?>Chưa có tài khoản? <a href="?p=register">Đăng ký miễn phí</a><?php endif ?>
          </p>
        </div>
    <?php });
}

function googleMark(): string {
    return '<svg class="g" viewBox="0 0 24 24" aria-hidden="true"><path fill="#4285F4" d="M23.5 12.3c0-.8-.1-1.6-.2-2.3H12v4.5h6.5a5.6 5.6 0 0 1-2.4 3.6v3h3.9c2.2-2.1 3.5-5.1 3.5-8.8Z"/>' .
           '<path fill="#34A853" d="M12 24c3.2 0 6-1.1 8-2.9l-3.9-3c-1.1.7-2.5 1.2-4.1 1.2-3.1 0-5.8-2.1-6.7-5H1.3v3.1A12 12 0 0 0 12 24Z"/>' .
           '<path fill="#FBBC05" d="M5.3 14.3a7.2 7.2 0 0 1 0-4.6V6.6h-4a12 12 0 0 0 0 10.8l4-3.1Z"/>' .
           '<path fill="#EA4335" d="M12 4.8c1.8 0 3.3.6 4.6 1.8l3.4-3.4A12 12 0 0 0 1.3 6.6l4 3.1c.9-2.9 3.6-4.9 6.7-4.9Z"/></svg>';
}

// --- actions (POST) --------------------------------------------------------------------
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    checkCsrf();
    $action = (string)($_POST['action'] ?? '');
    $back = (string)($_POST['back'] ?? 'index.php');
    $uid = (int)$me['id'];
    $admin = isAdmin($me);
    $id = (int)($_POST['id'] ?? 0);
    $book = $id && canSeeLibrary($me) ? bookById($id, $me) : null;
    // Target reader: one of the user's own, or (admin) empty for the shared inbox.
    $toRaw = (string)($_POST['to'] ?? '');
    $target = $toRaw !== '' ? ownDevice($me, $toRaw) : null;
    $to = $target['id'] ?? '';
    $toOk = $target !== null || ($toRaw === '' && $admin);
    $toLabel = $target ? deviceLabel($target) : 'hộp thư chung';
    $device = ownDevice($me, (string)($_POST['device'] ?? ''), true);
    $send = function (string $file, string $name, string $what) use ($toOk, $to, $uid): ?string {
        $sent = $toOk ? sendToReader($file, $name, $to) : null;
        if ($sent) logEvent('send', $what, $uid, $to !== '' ? $to : null);
        return $sent;
    };
    $notSent = $toOk ? 'Không gửi được (hộp thư của máy đã đầy ' . DEVICE_INBOX_MAX . ' tệp, hoặc tệp không hợp lệ).'
                     : 'Không gửi được: hãy chọn một máy trong tài khoản của bạn.';
    $msg = '';
    switch ($action) {
        case 'send':
            if ($book) {
                $sent = $send(bookFile($book), $book['title'] . ($book['author'] ? ' - ' . $book['author'] : '') . '.' . $book['format'],
                              'Sách: ' . $book['title']);
                if ($sent) db()->prepare('UPDATE books SET sent = ? WHERE id = ?')->execute([time(), $id]);
                $msg = $sent ? "Đã gửi \"{$book['title']}\" tới $toLabel. Trên máy: Thêm → Nhận sách." : $notSent;
            }
            break;
        case 'status':
            if ($book && $admin) {
                $status = in_array($_POST['status'] ?? '', ['', 'want', 'reading', 'read'], true) ? $_POST['status'] : '';
                db()->prepare('UPDATE books SET status = ? WHERE id = ?')->execute([$status, $id]);
            }
            break;
        case 'favorite':
            if ($book && $admin) db()->prepare('UPDATE books SET favorite = 1 - favorite WHERE id = ?')->execute([$id]);
            break;
        case 'edit':
            if ($book && $admin) {
                $f = fn($k) => trim(mb_substr((string)($_POST[$k] ?? ''), 0, 300));
                db()->prepare('UPDATE books SET title=?, author=?, category=?, subcategory=?, tags=?, language=?, search=? WHERE id=?')
                    ->execute([$f('title') ?: $book['title'], $f('author'), $f('category'), $f('subcategory'), $f('tags'),
                               strtolower(substr($f('language'), 0, 2)),
                               fold($f('title') . ' ' . $f('author') . ' ' . $f('category') . ' ' . $f('subcategory') . ' ' . $f('tags')), $id]);
                $msg = 'Đã lưu.';
            }
            break;
        case 'rights':
            if ($book && $admin && isset(RIGHTS_LABEL[$_POST['rights'] ?? ''])) {
                $rights = (string)$_POST['rights'];
                db()->prepare('UPDATE books SET rights = ?, rights_note = ? WHERE id = ?')
                    ->execute([$rights, 'Admin đặt ' . date('d/m/Y'), $id]);
                $msg = 'Đã đánh dấu "' . RIGHTS_LABEL[$rights] . '".';
            }
            break;
        case 'deletebook':
            if ($book && $admin) {
                @unlink(bookFile($book));
                @unlink(coverFile($id));
                db()->prepare('DELETE FROM books WHERE id = ?')->execute([$id]);
                logEvent('delete_file', 'Sách: ' . $book['title'], $uid);
                $msg = "Đã xoá \"{$book['title']}\" khỏi thư viện.";
                $back = '?p=books&f=copyright';
            }
            break;
        case 'bookdeck':
            if ($book && $admin && $book['format'] === 'epub') {
                set_time_limit(180);
                $deck = makeBookDeck($book);
                $msg = $deck ? "Đã tạo bộ từ \"$deck\"." : 'Không tìm được từ nào trong sách này.';
                if ($deck) {
                    logEvent('deck_create', $deck, $uid);
                    $back = '?p=deck&d=' . rawurlencode($deck);
                }
            }
            break;
        case 'senddeck':
            $deck = deckByName((string)($_POST['deck'] ?? ''), $me);
            $sent = $deck ? $send($deck['file'], $deck['name'] . '.tsv', 'Bộ từ: ' . $deck['name']) : null;
            $msg = $sent ? "Đã gửi bộ từ \"{$deck['name']}\" tới $toLabel. Trên máy: Thêm → Nhận sách, rồi Cài đặt → Từ vựng → Chọn bộ từ." : $notSent;
            break;
        case 'deletedeck':
            $deck = deckByName((string)($_POST['deck'] ?? ''), $me);
            if ($deck && ($deck['own'] || ($deck['custom'] && $admin))) {
                unlink($deck['file']);
                logEvent('delete_file', $deck['name'] . '.tsv', $uid);
                $msg = "Đã xoá \"{$deck['name']}\".";
                $back = '?p=vocab';
            }
            break;
        case 'uploaddeck':
            $file = $_FILES['deck'] ?? null;
            if (!$file || $file['error'] !== UPLOAD_ERR_OK || !preg_match('/\.tsv$/i', $file['name']) || $file['size'] >= 5 << 20) {
                $msg = 'Cần một file .tsv (từ, phiên âm, nghĩa, ví dụ), tối đa 5 MB.';
            } elseif ($admin) {
                if (!is_dir(CUSTOM_DECKS_DIR)) mkdir(CUSTOM_DECKS_DIR, 0750, true);
                $name = inboxName(basename($file['name']));
                move_uploaded_file($file['tmp_name'], CUSTOM_DECKS_DIR . '/' . $name);
                logEvent('upload', $name, $uid);
                $msg = "Đã thêm bộ từ \"$name\".";
            } else {
                $stored = storeUserFile($uid, $file['tmp_name'], $file['name'], (int)$file['size']);
                if ($stored[0] !== '!') logEvent('upload', $stored, $uid);
                $msg = $stored[0] === '!' ? substr($stored, 1) : "Đã thêm bộ từ \"$stored\".";
            }
            break;
        case 'upload':
            $files = $_FILES['files'] ?? null;
            $saved = [];
            $errors = [];
            foreach (is_array($files['name'] ?? null) ? $files['name'] : [] as $i => $original) {
                if ($files['error'][$i] !== UPLOAD_ERR_OK) {
                    $errors[] = $original . ': ' . ($files['error'][$i] === UPLOAD_ERR_INI_SIZE ? 'tệp quá lớn' : 'tải lên lỗi');
                    continue;
                }
                $stored = storeUserFile($uid, $files['tmp_name'][$i], (string)$original, (int)$files['size'][$i]);
                if ($stored[0] === '!') $errors[] = substr($stored, 1);
                else { $saved[] = $stored; logEvent('upload', $stored, $uid); }
            }
            if (!$saved && !$errors) $errors[] = 'Chưa chọn tệp nào (hoặc tổng dung lượng quá lớn cho một lần tải).';
            $msg = trim(($saved ? 'Đã tải lên ' . count($saved) . ' tệp. ' : '') . implode(' · ', $errors));
            break;
        case 'sendfile':
            $f = userFile($uid, (string)($_POST['file'] ?? ''));
            $sent = $f ? $send($f['file'], $f['name'], 'Tệp: ' . $f['name']) : null;
            $msg = $sent ? "Đã gửi \"{$f['name']}\" tới $toLabel. Trên máy: Thêm → Nhận sách." : $notSent;
            break;
        case 'deletefile':
            $f = userFile($uid, (string)($_POST['file'] ?? ''));
            if ($f) {
                unlink($f['file']);
                logEvent('delete_file', $f['name'], $uid);
                $msg = "Đã xoá \"{$f['name']}\".";
            }
            break;
        case 'unsend':
            $name = basename((string)($_POST['file'] ?? ''));
            $from = $toRaw !== '' ? ownDevice($me, $toRaw, true) : null;
            $dir = $from ? deviceInboxDir($from['id']) : ($toRaw === '' && $admin ? INBOX_DIR : '');
            if ($dir !== '' && $name !== '' && $name[0] !== '.' && is_file($dir . '/' . $name)) {
                unlink($dir . '/' . $name);
                logEvent('unsend', $name, $uid, $from['id'] ?? null);
            }
            break;
        case 'newkey':
            $key = newLinkKey($uid);
            logEvent('key_create', '', $uid);
            $msg = 'Khoá liên kết mới: ' . formatLinkKey($key) . '. Nhập khoá này trên máy đọc sách.';
            break;
        case 'revokekey':
            db()->prepare('UPDATE users SET link_key = NULL WHERE id = ?')->execute([$uid]);
            logEvent('key_revoke', '', $uid);
            $msg = 'Đã thu hồi khoá. Các máy đã liên kết vẫn nhận sách bình thường.';
            break;
        case 'renamedevice':
            if ($device) {
                renameDevice($device['id'], (string)($_POST['name'] ?? ''));
                logEvent('device_rename', (string)($_POST['name'] ?? ''), $uid, $device['id']);
            }
            break;
        case 'unlinkdevice':
            if ($device) {
                unlinkDevice($device['id']);
                logEvent('device_unlink', deviceLabel($device), $uid, $device['id']);
                $msg = 'Đã gỡ ' . deviceLabel($device) . ' khỏi tài khoản. Nếu máy còn giữ khoá hiện tại, máy sẽ tự liên kết lại; hãy tạo khoá mới nếu không muốn vậy.';
            }
            break;
        case 'removedevice':
            if ($device && $admin) {
                removeDevice($device['id']);
                logEvent('device_unlink', 'Xoá ' . deviceLabel($device), $uid, $device['id']);
                $msg = 'Đã xoá ' . deviceLabel($device) . '. Máy sẽ đăng ký lại khi kết nối.';
            }
            break;
        case 'profile':
            $name = trim(mb_substr((string)($_POST['name'] ?? ''), 0, 60));
            if ($name !== '') {
                db()->prepare('UPDATE users SET name = ? WHERE id = ?')->execute([$name, $uid]);
                logEvent('profile', $name, $uid);
                $msg = 'Đã lưu.';
            }
            break;
        case 'password':
            $new = (string)($_POST['new'] ?? '');
            if ($me['hash'] !== '' && !password_verify((string)($_POST['current'] ?? ''), $me['hash'])) {
                usleep(400000);
                $msg = 'Mật khẩu hiện tại không đúng.';
            } elseif (mb_strlen($new) < 8) {
                $msg = 'Mật khẩu mới cần ít nhất 8 ký tự.';
            } elseif ($new !== (string)($_POST['confirm'] ?? '')) {
                $msg = 'Hai lần nhập mật khẩu mới không khớp.';
            } else {
                setPassword($uid, $new);
                logEvent('password', '', $uid);
                $msg = 'Đã lưu mật khẩu mới.';
            }
            break;
        case 'unlinkgoogle':
            if ($me['hash'] !== '' && $me['google_sub'] !== null) {
                db()->prepare('UPDATE users SET google_sub = NULL WHERE id = ?')->execute([$uid]);
                logEvent('google_unlink', '', $uid);
                $msg = 'Đã bỏ liên kết Google.';
            }
            break;
        case 'deleteself':
            if (!$admin && mb_strtolower(trim((string)($_POST['confirm_email'] ?? ''))) === mb_strtolower((string)$me['email'])) {
                logEvent('user_delete', $me['email'] . ' (tự xoá)', $uid);
                deleteUser($uid);
                $_SESSION = [];
                session_regenerate_id(true);
                flashTo('Tài khoản của bạn đã được xoá.');
            }
            $msg = 'Email xác nhận không khớp, tài khoản chưa bị xoá.';
            break;
        case 'feedback':
            $model = '';
            $fw = '';
            $dev = ownDevice($me, (string)($_POST['device'] ?? ''));
            if ($dev) { $model = (string)$dev['model']; $fw = (string)$dev['firmware']; }
            elseif (isset($_POST['model_text'])) { $model = (string)$_POST['model_text']; }
            $res = saveFeedback($uid, (string)($_POST['kind'] ?? 'ui'), (string)($_POST['title'] ?? ''), (string)($_POST['body'] ?? ''),
                                (string)($_POST['screen'] ?? ''), $model, $fw, $_FILES['image'] ?? null);
            if (is_string($res)) { $msg = substr($res, 1); break; }
            logEvent('feedback', (string)$_POST['title'], $uid);
            $msg = 'Cảm ơn bạn! Góp ý đã được gửi, quản trị viên sẽ xem và trả lời tại trang này.';
            break;
        case 'feedbackdelete':
            $fb = $id ? feedbackById($id) : null;
            // A user withdraws their own report while it is new; the admin deletes any.
            if ($fb && ($admin || ((int)$fb['user_id'] === $uid && $fb['status'] === 'new'))) {
                deleteFeedback($id);
                $msg = 'Đã xoá góp ý.';
            }
            break;
        case 'feedbackupdate':
            if ($admin && $id && isset(FEEDBACK_STATUS[(string)($_POST['status'] ?? '')])) {
                db()->prepare('UPDATE feedback SET status = ?, reply = ?, updated = ? WHERE id = ?')
                    ->execute([(string)$_POST['status'], mb_substr(trim((string)($_POST['reply'] ?? '')), 0, 2000), time(), $id]);
                $msg = 'Đã cập nhật góp ý.';
            }
            break;
        case 'reportstatus':
            if ($admin && $id) {
                $status = in_array($_POST['status'] ?? '', ['open', 'fixed', 'ignored'], true) ? (string)$_POST['status'] : 'open';
                db()->prepare('UPDATE error_reports SET status = ?, note = ? WHERE id = ?')
                    ->execute([$status, mb_substr(trim((string)($_POST['note'] ?? '')), 0, 300), $id]);
                $msg = 'Đã cập nhật báo lỗi.';
            }
            break;
        case 'deletereport':
            if ($admin) {
                if ($id > 0) {
                    db()->prepare('DELETE FROM error_reports WHERE id = ?')->execute([$id]);
                    $msg = 'Đã xoá báo lỗi.';
                } else {
                    $msg = 'Đã xoá ' . db()->exec("DELETE FROM error_reports WHERE status != 'open'") . ' báo lỗi đã xử lý.';
                }
            }
            break;
        case 'setflag':
            $key = (string)($_POST['key'] ?? '');
            if ($admin && in_array($key, ['registration_open', 'library_open'], true)) {
                $on = ($_POST['value'] ?? '') === '1';
                meta($key, $on ? '1' : '0');
                logEvent('setting', $key . ' = ' . ($on ? 'bật' : 'tắt'), $uid);
            }
            break;
        case 'userdisable':
        case 'userenable':
        case 'userdelete':
            $u = $admin ? userById((int)($_POST['user'] ?? 0)) : null;
            if ($u && !isAdmin($u)) {
                if ($action === 'userdelete') {
                    deleteUser((int)$u['id']);
                    $msg = 'Đã xoá tài khoản ' . userLabel($u) . '.';
                } else {
                    db()->prepare('UPDATE users SET disabled = ? WHERE id = ?')->execute([$action === 'userdisable' ? 1 : 0, $u['id']]);
                }
                logEvent($action === 'userdelete' ? 'user_delete' : ($action === 'userdisable' ? 'user_disable' : 'user_enable'),
                         (string)($u['email'] ?? $u['login']), $uid);
            }
            break;
    }
    $_SESSION['flash'] = $msg;
    header('Location: index.php' . (str_starts_with($back, '?') ? $back : ''));
    exit;
}

// Everything below only reads the session: let the user's other requests run.
releaseSession();

// A user's own file, downloaded.
if ($page === 'getfile') {
    $f = userFile((int)$me['id'], (string)($_GET['f'] ?? ''));
    if (!$f) { http_response_code(404); exit('Not found'); }
    sendFile($f['file'], $f['name'], $f['kind'] === 'deck' ? 'text/tab-separated-values; charset=utf-8' : 'application/octet-stream');
}
if ($page === '') $page = canSeeLibrary($me) ? 'books' : 'mine';

// --- shared pieces ----------------------------------------------------------------------

function layout(string $title, string $active, callable $body): void {
    global $flash, $me;
    $nav = [];
    if ($me) {
        if (canSeeLibrary($me)) $nav['books'] = ['index.php?p=books', 'Sách'];
        $nav['mine'] = ['?p=mine', 'Tủ của tôi'];
        $nav['vocab'] = ['?p=vocab', 'Từ vựng'];
        $nav['device'] = ['?p=device', 'Thiết bị'];
        $nav['feedback'] = ['?p=feedback', 'Góp ý'];
        if (isAdmin($me)) $nav['admin'] = ['?p=admin', 'Quản trị'];
    }
    ?><!doctype html>
<html lang="vi"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex, nofollow"><title><?= h($title) ?> · Thư viện CrossPoint</title><link rel="stylesheet" href="style.css?v=4"></head>
<body>
<?php if ($active !== ''): ?>
<header class="top">
  <a class="brand" href="index.php">📚 Thư viện</a>
  <nav>
    <?php foreach ($nav as $id => [$href, $label]): ?>
      <a href="<?= h($href) ?>" class="<?= $active === $id ? 'on' : '' ?>"><?= h($label) ?></a>
    <?php endforeach ?>
    <a href="?p=profile" class="me <?= $active === 'profile' ? 'on' : '' ?>" title="Hồ sơ của <?= h(userLabel($me)) ?>"><?= avatar($me, 'sm') ?><span><?= h(userLabel($me)) ?></span></a>
    <a href="?p=logout" class="quiet">Thoát</a>
  </nav>
</header>
<?php endif ?>
<main>
<?php if (!empty($flash)): ?><div class="flash"><?= h($flash) ?></div><?php endif ?>
<?php $body() ?>
</main>
<script>
document.querySelectorAll('form[data-confirm]').forEach(function (f) {
  f.addEventListener('submit', function (e) { if (!confirm(f.dataset.confirm)) e.preventDefault(); });
});
</script>
</body></html>
<?php
}

function postButton(string $action, array $fields, string $label, string $class = '', string $confirm = ''): string {
    $html = '<form method="post" class="inline"' . ($confirm !== '' ? ' data-confirm="' . h($confirm) . '"' : '') . '>' .
            '<input type="hidden" name="csrf" value="' . h(csrfToken()) . '">' .
            '<input type="hidden" name="action" value="' . h($action) . '">' .
            '<input type="hidden" name="back" value="' . h('?' . ($_SERVER['QUERY_STRING'] ?? '')) . '">';
    foreach ($fields as $k => $v) $html .= '<input type="hidden" name="' . h($k) . '" value="' . h((string)$v) . '">';
    return $html . '<button class="' . h($class) . '">' . h($label) . '</button></form>';
}

// "Send" for a book or word list: one button per linked reader of the user
// when there are one or two, a picker beyond that. Without a reader: a link to
// set one up (the admin falls back to the shared inbox).
function sendControls(string $action, array $fields, string $class = 'primary'): string {
    global $me;
    $targets = devicesOf((int)$me['id']);
    if (!$targets) {
        if (isAdmin($me)) return postButton($action, $fields + ['to' => ''], 'Gửi tới hộp thư chung', $class);
        return '<a class="button ' . h($class) . '" href="?p=device">Liên kết máy để gửi</a>';
    }
    $labels = deviceLabels($targets);
    if (count($targets) <= 2) {
        return implode(' ', array_map(fn($d) => postButton($action, $fields + ['to' => $d['id']], 'Gửi tới ' . $labels[$d['id']], $class), $targets));
    }
    $html = '<form method="post" class="inline"><input type="hidden" name="csrf" value="' . h(csrfToken()) . '">' .
            '<input type="hidden" name="action" value="' . h($action) . '">' .
            '<input type="hidden" name="back" value="' . h('?' . ($_SERVER['QUERY_STRING'] ?? '')) . '">';
    foreach ($fields as $k => $v) $html .= '<input type="hidden" name="' . h($k) . '" value="' . h((string)$v) . '">';
    $html .= '<select name="to" class="inline-select">';
    foreach ($targets as $d) $html .= '<option value="' . h($d['id']) . '">' . h($labels[$d['id']]) . '</option>';
    return $html . '</select> <button class="' . h($class) . '">Gửi</button></form>';
}

function query(array $change): string {
    $q = array_merge($_GET, $change);
    $q = array_filter($q, fn($v) => $v !== '' && $v !== null);
    return '?' . http_build_query($q);
}

function ago(int $t): string {
    if ($t <= 0) return 'chưa';
    $d = time() - $t;
    if ($d < 120) return 'vừa xong';
    if ($d < 3600) return intdiv($d, 60) . ' phút trước';
    if ($d < 86400) return intdiv($d, 3600) . ' giờ trước';
    if ($d < 86400 * 30) return intdiv($d, 86400) . ' ngày trước';
    return date('d/m/Y', $t);
}

function events(string $where, array $args, int $limit, int $offset = 0): array {
    $st = db()->prepare("SELECT e.*, COALESCE(NULLIF(u.email, ''), u.login) AS who, d.name AS device_name FROM events e
                         LEFT JOIN users u ON u.id = e.user_id LEFT JOIN devices d ON d.id = e.device_id
                         " . ($where !== '' ? "WHERE $where" : '') . " ORDER BY e.time DESC, e.id DESC LIMIT $limit OFFSET $offset");
    $st->execute($args);
    return $st->fetchAll();
}

const STATUS_LABEL = ['want' => 'Muốn đọc', 'reading' => 'Đang đọc', 'read' => 'Đã đọc'];

function coverHtml(array $b): string {
    if ($b['cover']) return '<img loading="lazy" src="' . h(coverUrl((int)$b['id'])) . '" alt="">';
    $initials = mb_strtoupper(mb_substr($b['title'], 0, 1));
    return '<div class="nocover"><span>' . h($initials) . '</span><small>' . h(strtoupper($b['format'])) . '</small></div>';
}

require_once __DIR__ . '/../../library-app/pages/ui.php';

// --- pages ---------------------------------------------------------------------------------

if ($page === 'book') {
    $book = canSeeLibrary($me) ? bookById((int)($_GET['id'] ?? 0), $me) : null;
    if (!$book) { http_response_code(404); layout('Không tìm thấy', 'books', fn() => print('<p>Không tìm thấy sách.</p>')); exit; }
    layout($book['title'], 'books', function () use ($book, $me) {
        $sendable = in_array($book['format'], INBOX_EXT, true);
        $admin = isAdmin($me);
        ?>
        <p class="crumbs"><a href="index.php">Sách</a> › <a href="<?= h(query(['p' => null, 'id' => null, 'c' => $book['category']])) ?>"><?= h($book['category']) ?></a>
          <?php if ($book['subcategory']): ?> › <a href="<?= h(query(['p' => null, 'id' => null, 'c' => $book['category'], 's' => $book['subcategory']])) ?>"><?= h($book['subcategory']) ?></a><?php endif ?></p>
        <article class="detail">
          <div class="cover big"><?= coverHtml($book) ?></div>
          <div class="info">
            <?php if ($admin && $book['rights'] === 'copyright'): ?><div class="rights-alert">
              <strong>BẢN QUYỀN</strong>
              <p>Sách này có thể còn bản quyền nên đang được ẩn với mọi người trừ admin<?= $book['rights_note'] !== '' ? ': ' . h($book['rights_note']) : '' ?>.</p>
              <?= postButton('deletebook', ['id' => $book['id']], 'Xoá khỏi thư viện', 'danger', 'Xoá hẳn tệp sách này khỏi server?') ?>
              <?= postButton('rights', ['id' => $book['id'], 'rights' => 'free', 'back' => '?p=book&id=' . $book['id']], 'Được phép chia sẻ, hiện lại') ?>
            </div><?php endif ?>
            <h1><?= h($book['title']) ?> <?= $book['favorite'] ? '★' : '' ?></h1>
            <p class="author"><?= h($book['author']) ?></p>
            <p class="facts"><?= h(strtoupper($book['format'])) ?> · <?= h(humanSize((int)$book['size'])) ?>
              <?= $book['language'] ? ' · ' . h($book['language'] === 'vi' ? 'Tiếng Việt' : ($book['language'] === 'en' ? 'Tiếng Anh' : $book['language'])) : '' ?>
              <?= $book['sent'] ? ' · đã gửi tới máy ' . date('d/m/Y', (int)$book['sent']) : '' ?></p>
            <div class="actions">
              <?php if ($sendable): ?><?= sendControls('send', ['id' => $book['id']]) ?><?php endif ?>
              <?php if (in_array($book['format'], ['epub', 'pdf'], true)): ?><a class="button primary" href="read.php?id=<?= (int)$book['id'] ?>">📖 Đọc trên web</a><?php endif ?>
              <a class="button" href="file.php?id=<?= (int)$book['id'] ?>">Tải xuống</a>
              <?php if ($admin): ?><?= postButton('favorite', ['id' => $book['id']], $book['favorite'] ? '★ Bỏ yêu thích' : '☆ Yêu thích') ?><?php endif ?>
            </div>
            <?php if (!$sendable): ?><p class="hint">X4 Pro không đọc được định dạng <?= h(strtoupper($book['format'])) ?>; hãy chuyển sang EPUB (ví dụ bằng Calibre) để gửi tới máy.</p><?php endif ?>
            <?php if ($admin): ?><div class="status">
              <?php foreach (['' => 'Chưa đánh dấu'] + STATUS_LABEL as $k => $label): ?>
                <?= postButton('status', ['id' => $book['id'], 'status' => $k], $label, $book['status'] === $k ? 'chip on' : 'chip') ?>
              <?php endforeach ?>
            </div><?php endif ?>
            <?php if ($book['description']): ?><p class="desc"><?= nl2br(h($book['description'])) ?></p><?php endif ?>
            <?php if ($book['format'] === 'epub' && $book['language'] !== 'vi'): ?>
              <section class="card vocabbox">
                <h2>Học từ vựng với cuốn sách này</h2>
                <p>Tìm các từ ít gặp trong sách (bỏ qua 1.500 từ thông dụng nhất), kèm phiên âm, nghĩa tiếng Việt và câu ví dụ lấy từ chính cuốn sách. Học trước trên X4 Pro rồi đọc sẽ dễ hơn.</p>
                <?php $existing = deckByName('Sách - ' . trim(mb_strimwidth(preg_replace('/[\x00-\x1F<>:"\/\\\\|?*]/u', '', $book['title']) ?? '', 0, 70, '', 'UTF-8'))); ?>
                <?php if ($existing): ?><p><a href="?p=deck&d=<?= h(rawurlencode($existing['key'])) ?>">Xem bộ từ "<?= h($existing['name']) ?>" (<?= $existing['words'] ?> từ)</a></p>
                <?php elseif (!$admin): ?><p class="hint">Chưa có bộ từ cho cuốn này.</p><?php endif ?>
                <?php if ($admin): ?><?= postButton('bookdeck', ['id' => $book['id']], $existing ? 'Tạo lại bộ từ' : 'Tạo bộ từ vựng', 'primary') ?><?php endif ?>
              </section>
            <?php endif ?>
            <?php if ($admin): ?><details class="card edit"><summary>Sửa thông tin và phân loại</summary>
              <form method="post">
                <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>"><input type="hidden" name="action" value="edit">
                <input type="hidden" name="id" value="<?= (int)$book['id'] ?>"><input type="hidden" name="back" value="?p=book&id=<?= (int)$book['id'] ?>">
                <label>Tên sách <input name="title" value="<?= h($book['title']) ?>"></label>
                <label>Tác giả <input name="author" value="<?= h($book['author']) ?>"></label>
                <label>Thể loại <input name="category" value="<?= h($book['category']) ?>" list="cats"></label>
                <label>Nhóm <input name="subcategory" value="<?= h($book['subcategory']) ?>"></label>
                <label>Nhãn (cách nhau bởi dấu phẩy) <input name="tags" value="<?= h($book['tags']) ?>"></label>
                <label>Ngôn ngữ (vi, en…) <input name="language" value="<?= h($book['language']) ?>" maxlength="2"></label>
                <datalist id="cats"><?php foreach (db()->query('SELECT DISTINCT category FROM books ORDER BY category') as $r): ?><option value="<?= h($r['category']) ?>"><?php endforeach ?></datalist>
                <button class="primary">Lưu</button>
              </form>
            </details>
            <p class="path">Tệp: <?= h($book['path']) ?></p>
            <?php if ($book['rights'] !== 'copyright'): ?><p class="path">Bản quyền: <?= h(RIGHTS_LABEL[$book['rights']] ?? '') ?><?= $book['rights_note'] !== '' ? ' (' . h($book['rights_note']) . ')' : '' ?>
              <?= postButton('rights', ['id' => $book['id'], 'rights' => 'copyright', 'back' => '?p=book&id=' . $book['id']], 'Đánh dấu còn bản quyền', 'small') ?></p><?php endif ?>
            <?php endif ?>
          </div>
        </article>
    <?php });
    exit;
}

if ($page === 'vocab') {
    // The language studied is remembered in a cookie, like the reader's setting.
    $lang = (string)($_GET['lang'] ?? $_COOKIE['vocab_lang'] ?? 'en');
    if (!isset(VOCAB_LANGS[$lang])) $lang = 'en';
    if (($_COOKIE['vocab_lang'] ?? '') !== $lang) {
        setcookie('vocab_lang', $lang, ['expires' => time() + 365 * 86400, 'path' => '/library/', 'secure' => true,
                                        'httponly' => true, 'samesite' => 'Lax']);
    }
    layout('Từ vựng', 'vocab', function () use ($me, $lang) {
        $q = trim((string)($_GET['q'] ?? ''));
        $decks = array_values(array_filter(listDecks($me), fn($d) => $d['lang'] === $lang));
        ?>
        <div class="filters">
          <?php foreach (VOCAB_LANGS as $k => $label): ?>
            <a class="chip <?= $lang === $k ? 'on' : '' ?>" href="?p=vocab&lang=<?= h($k) ?>"><?= h($label) ?></a>
          <?php endforeach ?>
        </div>
        <?php if ($lang === 'zh'): ?><p class="hint">Bộ HSK 3.0: chữ Hán, pinyin, nghĩa tiếng Việt và âm Hán Việt. Trên máy (bản 2.0.10 trở lên): Cài đặt → Từ vựng → Ngôn ngữ học → Tiếng Trung. Phông chữ Hán có sẵn trong firmware.</p><?php endif ?>
        <section class="card">
          <form class="search" method="get"><input type="hidden" name="p" value="vocab"><input type="hidden" name="lang" value="<?= h($lang) ?>">
            <input name="q" value="<?= h($q) ?>" placeholder="<?= $lang === 'zh' ? 'Tra từ: chữ Hán hoặc nghĩa tiếng Việt' : 'Tra từ: tiếng Anh hoặc nghĩa tiếng Việt' ?>"><button>Tra</button></form>
          <?php if ($q !== ''): $rows = lookupWords($q, $lang); ?>
            <?php if (!$rows): ?><p>Không có từ nào khớp.</p><?php endif ?>
            <table class="words"><?php foreach ($rows as $w): ?>
              <tr><td><b><?= h($w['word']) ?></b><br><span class="ipa"><?= h($w['ipa']) ?></span></td>
                  <td><?= h($w['meaning']) ?><?php if ($w['example']): ?><br><i><?= h($w['example']) ?></i><?php endif ?><br><small><?= h($w['deck']) ?></small></td></tr>
            <?php endforeach ?></table>
          <?php endif ?>
        </section>
        <h2>Bộ từ</h2>
        <div class="decks">
          <?php foreach ($decks as $d): ?>
            <div class="card deck">
              <a href="?p=deck&d=<?= h(rawurlencode($d['key'])) ?>"><b><?= h($d['name']) ?></b></a>
              <small><?= number_format($d['words'], 0, ',', '.') ?> từ<?= $d['own'] ? ' · của tôi' : ($d['custom'] ? ' · từ sách' : '') ?></small>
              <div class="actions">
                <?= sendControls('senddeck', ['deck' => $d['key']], 'primary small') ?>
                <a class="button small" href="deck.php?d=<?= h(rawurlencode($d['key'])) ?>">Tải .tsv</a>
              </div>
            </div>
          <?php endforeach ?>
        </div>
        <section class="card">
          <h2>Thêm bộ từ</h2>
          <p>Mỗi dòng: <code>từ⇥phiên âm⇥nghĩa⇥ví dụ</code> (phiên âm và ví dụ không bắt buộc).<?= isAdmin($me) ? ' Hoặc mở một cuốn sách tiếng Anh và bấm <b>Tạo bộ từ vựng</b>.' : ' Bộ từ bạn tải lên chỉ mình bạn thấy.' ?></p>
          <form method="post" enctype="multipart/form-data">
            <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>"><input type="hidden" name="action" value="uploaddeck"><input type="hidden" name="back" value="?p=vocab">
            <input type="file" name="deck" accept=".tsv" required> <button>Tải lên</button>
          </form>
        </section>
    <?php });
    exit;
}

if ($page === 'deck') {
    $deck = deckByName((string)($_GET['d'] ?? ''), $me);
    if (!$deck) { http_response_code(404); layout('Không tìm thấy', 'vocab', fn() => print('<p>Không tìm thấy bộ từ.</p>')); exit; }
    layout($deck['name'], 'vocab', function () use ($deck, $me) {
        $q = mb_strtolower(trim((string)($_GET['q'] ?? '')));
        $pageNo = max(1, (int)($_GET['n'] ?? 1));
        $rows = [];
        $total = 0;
        foreach (new SplFileObject($deck['file']) as $line) {
            $row = is_string($line) ? parseDeckLine($line) : null;
            if (!$row) continue;
            if ($q !== '' && !str_contains(mb_strtolower($row['word'] . ' ' . $row['meaning']), $q)) continue;
            $total++;
            if ($total > ($pageNo - 1) * 100 && $total <= $pageNo * 100) $rows[] = $row;
        }
        $pages = max(1, (int)ceil($total / 100));
        ?>
        <p class="crumbs"><a href="?p=vocab">Từ vựng</a> › <?= h($deck['name']) ?></p>
        <h1><?= h($deck['name']) ?> <small><?= number_format($deck['words'], 0, ',', '.') ?> từ</small></h1>
        <div class="actions">
          <?= sendControls('senddeck', ['deck' => $deck['key']]) ?>
          <a class="button" href="deck.php?d=<?= h(rawurlencode($deck['key'])) ?>">Tải .tsv</a>
          <?php if ($deck['own'] || ($deck['custom'] && isAdmin($me))): ?><?= postButton('deletedeck', ['deck' => $deck['key']], 'Xoá bộ từ', 'danger', 'Xoá bộ từ này?') ?><?php endif ?>
        </div>
        <form class="search" method="get"><input type="hidden" name="p" value="deck"><input type="hidden" name="d" value="<?= h($deck['key']) ?>">
          <input name="q" value="<?= h($q) ?>" placeholder="Lọc trong bộ từ"><button>Lọc</button></form>
        <table class="words"><?php foreach ($rows as $w): ?>
          <tr><td><b><?= h($w['word']) ?></b><br><span class="ipa"><?= h($w['ipa']) ?></span></td>
              <td><?= h($w['meaning']) ?><?php if ($w['example']): ?><br><i><?= h($w['example']) ?></i><?php endif ?></td></tr>
        <?php endforeach ?></table>
        <?php if ($pages > 1): ?><nav class="pager">
          <?php if ($pageNo > 1): ?><a href="<?= h(query(['n' => $pageNo - 1])) ?>">‹ Trước</a><?php endif ?>
          <span><?= $pageNo ?> / <?= $pages ?></span>
          <?php if ($pageNo < $pages): ?><a href="<?= h(query(['n' => $pageNo + 1])) ?>">Sau ›</a><?php endif ?>
        </nav><?php endif ?>
    <?php });
    exit;
}

if ($page === 'device') {
    layout('Thiết bị', 'device', function () use ($me) {
        $admin = isAdmin($me);
        $mine = devicesOf((int)$me['id']);
        $key = $me['link_key'];
        ?>
        <h1>Thiết bị</h1>
        <section class="card">
          <h2>Khoá liên kết</h2>
          <?php if ($key): ?>
            <p class="linkkey"><?= h(formatLinkKey($key)) ?></p>
            <div class="actions">
              <?= postButton('newkey', [], 'Tạo khoá mới', 'small', 'Khoá cũ sẽ không liên kết được máy mới nữa (máy đã liên kết vẫn dùng bình thường). Tạo khoá mới?') ?>
              <?= postButton('revokekey', [], 'Thu hồi khoá', 'small', 'Thu hồi khoá này?') ?>
            </div>
          <?php else: ?>
            <p>Tạo khoá rồi nhập vào máy đọc sách để máy vào tài khoản của bạn — không cần chờ duyệt.</p>
            <div class="actions"><?= postButton('newkey', [], 'Tạo khoá liên kết', 'primary') ?></div>
          <?php endif ?>
          <ol>
            <li>Trên máy: <b>Thêm → Nhận sách</b>, chọn Wi-Fi.</li>
            <li>Máy báo <b>Chưa liên kết tài khoản</b>: bấm <b>OK</b> và nhập khoá (không cần dấu gạch, chữ hoa hay thường đều được).</li>
            <li>Máy hiện trong danh sách dưới đây. Từ đó, bấm <b>Gửi tới …</b> ở sách, bộ từ hay tệp của bạn, rồi trên máy vào <b>Thêm → Nhận sách</b>.</li>
          </ol>
          <p class="hint">Máy chạy firmware cũ (2.0.7): mở <b>Truyền file</b> trên máy, vào <b>Cài đặt</b> trên trình duyệt và dán khoá vào ô <b>Khoá hộp thư sách</b>, rồi vào <b>Nhận sách</b>. Nên cập nhật firmware mới ở <b>Cài đặt → Cập nhật OTA</b>.</p>
        </section>

        <?php if (!$mine): ?><section class="card"><p>Chưa có máy nào trong tài khoản của bạn.</p></section><?php endif ?>
        <?php foreach ($mine as $d): $waiting = inboxWaiting($d['id']); ?>
          <section class="card device">
            <div class="device-head">
              <h2><?= h(deviceLabel($d)) ?></h2>
              <div class="actions">
                <?= postButton('unlinkdevice', ['device' => $d['id']], 'Gỡ khỏi tài khoản', 'danger small', 'Gỡ máy này khỏi tài khoản? Các tệp đang chờ máy nhận sẽ bị huỷ.') ?>
              </div>
            </div>
            <p class="facts">
              Mã máy <code><?= h(strtoupper(implode(':', str_split($d['id'], 2)))) ?></code> ·
              <?= h($d['model'] ?: 'không rõ') ?> · firmware <?= h($d['firmware'] ?: '?') ?>
              <?= (int)$d['battery'] >= 0 ? ' · pin ' . (int)$d['battery'] . '%' : '' ?> ·
              kết nối <?= h(ago((int)$d['last_seen'])) ?>
            </p>
            <form method="post" class="rename">
              <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>"><input type="hidden" name="action" value="renamedevice">
              <input type="hidden" name="device" value="<?= h($d['id']) ?>"><input type="hidden" name="back" value="?p=device">
              <input name="name" value="<?= h($d['name']) ?>" placeholder="Tên máy" maxlength="40"> <button class="small">Đổi tên</button>
            </form>
            <h3>Đang chờ máy nhận (<?= count($waiting) ?>)</h3>
            <?php if (!$waiting): ?><p class="hint">Không có gì. Bấm <b>Gửi tới <?= h(deviceLabel($d)) ?></b> ở một cuốn sách, bộ từ hoặc tệp của bạn.</p><?php endif ?>
            <ul class="waiting"><?php foreach ($waiting as $f): ?>
              <li><?= h($f) ?> <?= postButton('unsend', ['file' => $f, 'to' => $d['id']], 'Huỷ', 'small') ?></li>
            <?php endforeach ?></ul>
          </section>
        <?php endforeach ?>

        <?php if ($admin): $all = array_filter(allDevices(), fn($d) => (int)($d['user_id'] ?? 0) !== (int)$me['id']); ?>
          <section class="card">
            <h2>Máy của người khác (<?= count($all) ?>)</h2>
            <?php if (!$all): ?><p class="hint">Chưa có.</p><?php else: ?>
            <div class="scroll"><table class="log">
              <tr><th>Máy</th><th>Chủ</th><th>Firmware</th><th>Kết nối</th><th>Đang chờ</th><th></th></tr>
              <?php foreach ($all as $d): ?>
                <tr><td><?= h(deviceLabel($d)) ?><br><small><?= h(strtoupper($d['id'])) ?> · <?= h($d['model']) ?></small></td>
                    <td><?= $d['user_id'] !== null ? '<a href="?p=admin&amp;u=' . (int)$d['user_id'] . '">' . h($d['owner_email'] ?: $d['owner_login']) . '</a>' : '<i>chưa liên kết</i>' ?></td>
                    <td><?= h($d['firmware']) ?></td><td><?= h(ago((int)$d['last_seen'])) ?></td>
                    <td><?= $d['user_id'] !== null ? count(inboxWaiting($d['id'])) : '' ?></td>
                    <td><?php if ($d['user_id'] !== null): ?><?= postButton('unlinkdevice', ['device' => $d['id']], 'Gỡ', 'small', 'Gỡ máy này khỏi tài khoản của chủ máy?') ?><?php endif ?>
                        <?= postButton('removedevice', ['device' => $d['id']], 'Xoá', 'danger small', 'Xoá hẳn máy này?') ?></td></tr>
              <?php endforeach ?>
            </table></div><?php endif ?>
          </section>
          <?php $shared = inboxWaiting(); if ($shared): ?>
            <section class="card">
              <h2>Hộp thư chung (<?= count($shared) ?>)</h2>
              <p class="hint">Chỉ máy của quản trị và máy dùng khoá hộp thư chung nhận các tệp này.</p>
              <ul class="waiting"><?php foreach ($shared as $f): ?>
                <li><?= h($f) ?> <?= postButton('unsend', ['file' => $f, 'to' => ''], 'Huỷ', 'small') ?></li>
              <?php endforeach ?></ul>
            </section>
          <?php endif ?>
        <?php endif ?>

        <?php if (canSeeLibrary($me)): $base = 'https://' . ($_SERVER['HTTP_HOST'] ?? 'crosspoint.zenix.vn') . '/library/opds.php'; ?>
          <section class="card">
            <h2>Duyệt cả thư viện ngay trên máy (OPDS)</h2>
            <ol>
              <li>Trên máy: <b>Cài đặt → Mạng → Máy chủ OPDS</b> → <b>Thêm máy chủ</b>.</li>
              <li>Địa chỉ: <code><?= h($base) ?></code></li>
              <li>Tên đăng nhập: <code><?= h($me['email'] ?: $me['login']) ?></code> · Mật khẩu: mật khẩu của tài khoản này<?= $me['hash'] === '' ? ' (bạn đăng nhập bằng Google: hãy đặt mật khẩu ở trang <a href="?p=profile">Hồ sơ</a>)' : '' ?>.</li>
            </ol>
          </section>
        <?php endif ?>
    <?php });
    exit;
}

if ($page === 'mine') {
    layout('Tủ của tôi', 'mine', function () use ($me) {
        $uid = (int)$me['id'];
        $files = userFiles($uid);
        $used = userUsage($uid);
        $maxUpload = min(USER_FILE_MAX, (int)ini_parse_quantity((string)ini_get('upload_max_filesize')));
        ?>
        <h1>Tủ của tôi</h1>
        <p class="hint">Sách (EPUB, TXT, MD, XTC) và bộ từ vựng (.tsv) của riêng bạn, để gửi tới máy đọc sách. Đã dùng <?= h(humanSize($used)) ?> / <?= h(humanSize(USER_QUOTA)) ?>.</p>
        <section class="card">
          <h2>Tải lên</h2>
          <form method="post" enctype="multipart/form-data" class="upload">
            <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>"><input type="hidden" name="action" value="upload"><input type="hidden" name="back" value="?p=mine">
            <input type="file" name="files[]" multiple accept=".epub,.txt,.md,.xtc,.xtch,.tsv" required> <button class="primary">Tải lên</button>
          </form>
          <p class="hint">Mỗi tệp tối đa <?= h(humanSize($maxUpload)) ?>. Sách PDF, MOBI, AZW3 hãy chuyển sang EPUB trước (ví dụ bằng Calibre).</p>
        </section>
        <?php if (!$files): ?><section class="card"><p>Chưa có tệp nào.</p></section><?php else: ?>
        <section class="card">
          <div class="scroll"><table class="log files">
            <tr><th>Tệp</th><th>Loại</th><th>Dung lượng</th><th>Thêm lúc</th><th></th></tr>
            <?php foreach ($files as $f): ?>
              <tr><td><?= h($f['name']) ?></td><td><?= $f['kind'] === 'deck' ? 'Bộ từ' : 'Sách' ?></td>
                  <td><?= h(humanSize((int)$f['size'])) ?></td><td><?= date('d/m/Y', (int)$f['mtime']) ?></td>
                  <td class="actions"><?= sendControls('sendfile', ['file' => $f['name']], 'primary small') ?>
                    <?php if (str_ends_with(strtolower($f['name']), '.epub')): ?><a class="button small" href="read.php?f=<?= h(rawurlencode($f['name'])) ?>">Đọc</a><?php endif ?>
                    <a class="button small" href="?p=getfile&amp;f=<?= h(rawurlencode($f['name'])) ?>">Tải xuống</a>
                    <?= postButton('deletefile', ['file' => $f['name']], 'Xoá', 'danger small', 'Xoá tệp này?') ?></td></tr>
            <?php endforeach ?>
          </table></div>
        </section>
        <?php endif ?>
    <?php });
    exit;
}

if ($page === 'feedback') require __DIR__ . '/../../library-app/pages/feedback.php';
if ($page === 'feedbackimg') {
    // A screenshot: its author and the admin may see it.
    $fb = feedbackById((int)($_GET['id'] ?? 0));
    if (!$fb || $fb['image'] === '' || (!isAdmin($me) && (int)$fb['user_id'] !== (int)$me['id'])) { http_response_code(404); exit('Not found'); }
    $file = feedbackDir() . '/' . (int)$fb['id'] . '.' . $fb['image'];
    if (!is_file($file)) { http_response_code(404); exit('Not found'); }
    header('Content-Type: ' . (['jpg' => 'image/jpeg', 'png' => 'image/png', 'webp' => 'image/webp', 'gif' => 'image/gif'][$fb['image']] ?? 'application/octet-stream'));
    header('Content-Length: ' . filesize($file));
    header('X-Content-Type-Options: nosniff');
    header('Cache-Control: private, max-age=300');
    readfile($file);
    exit;
}
if ($page === 'account') $page = 'profile';  // the page's name before the redesign
if ($page === 'profile') require __DIR__ . '/../../library-app/pages/profile.php';
if ($page === 'admin') {
    if (!isAdmin($me)) { http_response_code(403); layout('Không có quyền', '', fn() => print('<div class="card"><p>Chỉ quản trị viên xem được trang này. <a href="index.php">Về trang chính</a></p></div>')); exit; }
    require __DIR__ . '/../../library-app/pages/admin.php';
}

// --- book list ---------------------------------------------------------------------------
if ($page !== 'books' || !canSeeLibrary($me)) {
    http_response_code(404);
    layout('Không tìm thấy', 'books', fn() => print('<p class="card">Không có trang này. <a href="index.php">Về trang chính</a></p>'));
    exit;
}
layout('Sách', 'books', function () use ($me) {
    $c = (string)($_GET['c'] ?? '');
    $s = (string)($_GET['s'] ?? '');
    $q = trim((string)($_GET['q'] ?? ''));
    $f = (string)($_GET['f'] ?? '');
    $sort = (string)($_GET['sort'] ?? 'title');
    $n = max(1, (int)($_GET['n'] ?? 1));

    $admin = isAdmin($me);
    $visible = visibleBooks($me);
    $where = [$visible];
    $args = [];
    if ($c !== '') { $where[] = 'category = ?'; $args[] = $c; }
    if ($s !== '') { $where[] = 'subcategory = ?'; $args[] = $s; }
    if ($q !== '') { foreach (preg_split('/\s+/', fold($q)) as $word) { $where[] = 'search LIKE ?'; $args[] = "%$word%"; } }
    if ($f === 'fav') $where[] = 'favorite = 1';
    elseif (isset(STATUS_LABEL[$f])) { $where[] = 'status = ?'; $args[] = $f; }
    elseif ($f === 'en') { $where[] = "language = 'en'"; }
    elseif ($f === 'epub') { $where[] = "format = 'epub'"; }
    elseif ($f === 'copyright' && $admin) { $where[] = "rights = 'copyright'"; }
    $sql = ' WHERE ' . implode(' AND ', $where);
    $order = ['title' => 'title COLLATE NOCASE', 'author' => 'author COLLATE NOCASE, title', 'new' => 'added DESC, title'][$sort] ?? 'title';

    $count = db()->prepare("SELECT COUNT(*) FROM books$sql");
    $count->execute($args);
    $total = (int)$count->fetchColumn();
    $st = db()->prepare("SELECT * FROM books$sql ORDER BY $order LIMIT " . PAGE_SIZE . ' OFFSET ' . (($n - 1) * PAGE_SIZE));
    $st->execute($args);
    $books = $st->fetchAll();
    $pages = max(1, (int)ceil($total / PAGE_SIZE));

    $cats = db()->query("SELECT category, COUNT(*) AS n FROM books WHERE $visible GROUP BY category ORDER BY category")->fetchAll();
    $subs = [];
    if ($c !== '') {
        $st2 = db()->prepare("SELECT subcategory, COUNT(*) AS n FROM books WHERE $visible AND category = ? AND subcategory <> '' GROUP BY subcategory ORDER BY subcategory");
        $st2->execute([$c]);
        $subs = $st2->fetchAll();
    }
    $all = (int)db()->query("SELECT COUNT(*) FROM books WHERE $visible")->fetchColumn();
    $flagged = $admin ? (int)db()->query("SELECT COUNT(*) FROM books WHERE rights = 'copyright'")->fetchColumn() : 0;
    ?>
    <div class="library">
      <aside>
        <form class="search" method="get"><input name="q" value="<?= h($q) ?>" placeholder="Tìm tên sách, tác giả…"><button>Tìm</button></form>
        <a class="cat <?= $c === '' ? 'on' : '' ?>" href="<?= h(query(['c' => null, 's' => null, 'n' => null])) ?>">Tất cả <span><?= $all ?></span></a>
        <?php foreach ($cats as $cat): ?>
          <a class="cat <?= $c === $cat['category'] ? 'on' : '' ?>" href="<?= h(query(['c' => $cat['category'], 's' => null, 'n' => null])) ?>"><?= h($cat['category'] ?: 'Chưa phân loại') ?> <span><?= $cat['n'] ?></span></a>
          <?php if ($c === $cat['category']): foreach ($subs as $sub): ?>
            <a class="cat sub <?= $s === $sub['subcategory'] ? 'on' : '' ?>" href="<?= h(query(['s' => $sub['subcategory'], 'n' => null])) ?>"><?= h($sub['subcategory']) ?> <span><?= $sub['n'] ?></span></a>
          <?php endforeach; endif ?>
        <?php endforeach ?>
      </aside>
      <section>
        <div class="filters">
          <?php foreach (($admin ? ['' => 'Tất cả', 'fav' => '★ Yêu thích', 'want' => 'Muốn đọc', 'reading' => 'Đang đọc', 'read' => 'Đã đọc']
                                 : ['' => 'Tất cả']) + ['en' => 'Tiếng Anh', 'epub' => 'Đọc được trên X4'] as $k => $label): ?>
            <a class="chip <?= $f === $k ? 'on' : '' ?>" href="<?= h(query(['f' => $k, 'n' => null])) ?>"><?= h($label) ?></a>
          <?php endforeach ?>
          <?php if ($flagged): ?><a class="chip rights <?= $f === 'copyright' ? 'on' : '' ?>" href="<?= h(query(['f' => 'copyright', 'n' => null])) ?>">Bản quyền <?= number_format($flagged, 0, ',', '.') ?></a><?php endif ?>
          <span class="grow"></span>
          <?php foreach (['title' => 'Tên', 'author' => 'Tác giả', 'new' => 'Mới thêm'] as $k => $label): ?>
            <a class="chip <?= $sort === $k ? 'on' : '' ?>" href="<?= h(query(['sort' => $k, 'n' => null])) ?>"><?= h($label) ?></a>
          <?php endforeach ?>
        </div>
        <p class="count"><?= number_format($total, 0, ',', '.') ?> cuốn<?= $all === 0 ? ' — thư viện đang được tải lên và lập chỉ mục, hãy quay lại sau ít phút.' : '' ?></p>
        <div class="grid">
          <?php foreach ($books as $b): ?>
            <a class="book" href="?p=book&id=<?= (int)$b['id'] ?>">
              <div class="cover"><?= coverHtml($b) ?><?php if ($b['status']): ?><em><?= h(STATUS_LABEL[$b['status']] ?? '') ?></em><?php endif ?>
                <?php if ($b['rights'] === 'copyright'): ?><strong class="rights-tag">BẢN QUYỀN</strong><?php endif ?></div>
              <b><?= h($b['title']) ?></b><small><?= h($b['author']) ?></small>
            </a>
          <?php endforeach ?>
        </div>
        <?php if ($pages > 1): ?><nav class="pager">
          <?php if ($n > 1): ?><a href="<?= h(query(['n' => $n - 1])) ?>">‹ Trước</a><?php endif ?>
          <span><?= $n ?> / <?= $pages ?></span>
          <?php if ($n < $pages): ?><a href="<?= h(query(['n' => $n + 1])) ?>">Sau ›</a><?php endif ?>
        </nav><?php endif ?>
      </section>
    </div>
<?php });
