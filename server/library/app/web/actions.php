<?php
// POST actions of the web app. Each handler gets the request context and
// returns the message to show on the next page; it may change $ctx['back']
// to send the browser elsewhere. Register new actions in actions().

declare(strict_types=1);

require_once __DIR__ . '/kernel.php';

function actions(): array {
    return [
        'send'         => 'actSend',
        'status'       => 'actStatus',
        'favorite'     => 'actFavorite',
        'edit'         => 'actEdit',
        'bookdeck'     => 'actBookDeck',
        'senddeck'     => 'actSendDeck',
        'deletedeck'   => 'actDeleteDeck',
        'uploaddeck'   => 'actUploadDeck',
        'unsend'       => 'actUnsend',
        'approve'      => 'actApprove',
        'unapprove'    => 'actApprove',
        'renamedevice' => 'actRenameDevice',
        'removedevice' => 'actRemoveDevice',
        'password'     => 'actPassword',
    ];
}

// Runs the posted action and redirects back (POST/redirect/GET).
function handlePost(): never {
    checkCsrf();
    $action = (string)($_POST['action'] ?? '');
    $id = (int)($_POST['id'] ?? 0);
    // Target reader: a device id, or empty for the shared inbox.
    $to = validDeviceId((string)($_POST['to'] ?? '')) ? (string)$_POST['to'] : '';
    $target = $to !== '' ? deviceById($to) : null;
    if ($to !== '' && !$target) $to = '';
    $ctx = [
        'action' => $action,
        'back' => (string)($_POST['back'] ?? ''),
        'id' => $id,
        'book' => $id ? bookById($id) : null,
        'to' => $to,
        'toLabel' => $target ? deviceLabel($target) : 'hộp thư chung',
        'device' => validDeviceId((string)($_POST['device'] ?? '')) ? deviceById((string)$_POST['device']) : null,
    ];
    $handler = actions()[$action] ?? null;
    $msg = $handler ? (string)$handler($ctx) : '';
    $_SESSION['flash'] = $msg;
    header('Location: index.php' . (str_starts_with($ctx['back'], '?') ? $ctx['back'] : ''));
    exit;
}

function actSend(array &$ctx): string {
    $book = $ctx['book'];
    if (!$book) return '';
    $sent = sendToReader(bookFile($book), $book['title'] . ($book['author'] ? ' - ' . $book['author'] : '') . '.' . $book['format'], $ctx['to']);
    if (!$sent) return 'Không gửi được sách này.';
    db()->prepare('UPDATE books SET sent = ? WHERE id = ?')->execute([time(), $book['id']]);
    return "Đã gửi \"{$book['title']}\" tới {$ctx['toLabel']}. Trên máy: Thêm → Nhận sách.";
}

function actStatus(array &$ctx): string {
    if (!$ctx['book']) return '';
    $status = in_array($_POST['status'] ?? '', ['', 'want', 'reading', 'read'], true) ? $_POST['status'] : '';
    db()->prepare('UPDATE books SET status = ? WHERE id = ?')->execute([$status, $ctx['id']]);
    return '';
}

function actFavorite(array &$ctx): string {
    if ($ctx['book']) db()->prepare('UPDATE books SET favorite = 1 - favorite WHERE id = ?')->execute([$ctx['id']]);
    return '';
}

function actEdit(array &$ctx): string {
    $book = $ctx['book'];
    if (!$book) return '';
    $f = fn($k) => trim(mb_substr((string)($_POST[$k] ?? ''), 0, 300));
    db()->prepare('UPDATE books SET title=?, author=?, category=?, subcategory=?, tags=?, language=?, search=? WHERE id=?')
        ->execute([$f('title') ?: $book['title'], $f('author'), $f('category'), $f('subcategory'), $f('tags'),
                   strtolower(substr($f('language'), 0, 2)),
                   fold($f('title') . ' ' . $f('author') . ' ' . $f('category') . ' ' . $f('subcategory') . ' ' . $f('tags')), $ctx['id']]);
    return 'Đã lưu.';
}

function actBookDeck(array &$ctx): string {
    $book = $ctx['book'];
    if (!$book || $book['format'] !== 'epub') return '';
    set_time_limit(180);
    $deck = makeBookDeck($book);
    if (!$deck) return 'Không tìm được từ nào trong sách này.';
    $ctx['back'] = '?p=deck&d=' . rawurlencode($deck);
    return "Đã tạo bộ từ \"$deck\".";
}

function actSendDeck(array &$ctx): string {
    $deck = deckByName((string)($_POST['deck'] ?? ''));
    $sent = $deck ? sendToReader($deck['file'], $deck['name'] . '.tsv', $ctx['to']) : null;
    return $sent ? "Đã gửi bộ từ \"{$deck['name']}\" tới {$ctx['toLabel']}. Trên máy: Thêm → Nhận sách, rồi Cài đặt → Từ vựng → Chọn bộ từ." : 'Không gửi được.';
}

function actDeleteDeck(array &$ctx): string {
    $deck = deckByName((string)($_POST['deck'] ?? ''));
    if (!$deck || !$deck['custom']) return '';
    unlink($deck['file']);
    $ctx['back'] = '?p=vocab';
    return "Đã xoá \"{$deck['name']}\".";
}

function actUploadDeck(array &$ctx): string {
    $file = $_FILES['deck'] ?? null;
    if (!$file || $file['error'] !== UPLOAD_ERR_OK || !preg_match('/\.tsv$/i', $file['name']) || $file['size'] >= 5 << 20) {
        return 'Cần một file .tsv (từ, phiên âm, nghĩa, ví dụ), tối đa 5 MB.';
    }
    if (!is_dir(CUSTOM_DECKS_DIR)) mkdir(CUSTOM_DECKS_DIR, 0750, true);
    $name = inboxName(basename($file['name']));
    move_uploaded_file($file['tmp_name'], CUSTOM_DECKS_DIR . '/' . $name);
    return "Đã thêm bộ từ \"$name\".";
}

function actUnsend(array &$ctx): string {
    $name = basename((string)($_POST['file'] ?? ''));
    $dir = $ctx['to'] !== '' ? deviceInboxDir($ctx['to']) : INBOX_DIR;
    if ($name !== '' && $name[0] !== '.' && is_file($dir . '/' . $name)) unlink($dir . '/' . $name);
    return '';
}

function actApprove(array &$ctx): string {
    $device = $ctx['device'];
    if (!$device) return '';
    $approve = $ctx['action'] === 'approve';
    setDeviceApproved($device['id'], $approve);
    return $approve ? 'Đã duyệt ' . deviceLabel($device) . '. Trên máy: Thêm → Nhận sách.' : 'Đã tạm ngưng ' . deviceLabel($device) . '.';
}

function actRenameDevice(array &$ctx): string {
    if ($ctx['device']) renameDevice($ctx['device']['id'], (string)($_POST['name'] ?? ''));
    return '';
}

function actRemoveDevice(array &$ctx): string {
    $device = $ctx['device'];
    if (!$device) return '';
    removeDevice($device['id']);
    return 'Đã xoá ' . deviceLabel($device) . '. Máy sẽ đăng ký lại khi kết nối, chờ duyệt.';
}

function actPassword(array &$ctx): string {
    $auth = authData();
    $current = (string)($_POST['current'] ?? '');
    $new = (string)($_POST['new'] ?? '');
    $ctx['back'] = '?p=account';
    if (!checkPassword((string)($auth['user'] ?? ''), $current)) {
        usleep(400000);
        return 'Mật khẩu hiện tại không đúng.';
    }
    if (mb_strlen($new) < 8) return 'Mật khẩu mới cần ít nhất 8 ký tự.';
    if ($new !== (string)($_POST['confirm'] ?? '')) return 'Hai lần nhập mật khẩu mới không khớp.';
    setLogin((string)$auth['user'], $new);
    return 'Đã đổi mật khẩu. Nhớ cập nhật mật khẩu OPDS trên máy đọc sách.';
}
