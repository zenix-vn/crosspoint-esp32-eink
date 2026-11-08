<?php
// The web app's frame: the module registry, the page layout and the small
// HTML helpers every page shares. public/index.php routes requests here.
//
// Adding a section: put pages/<id>.php next to the others (it returns
// ['title' => ..., 'body' => callable]) and register it in modules(). Its
// POST actions go in actions.php.

declare(strict_types=1);

require_once __DIR__ . '/../vocab.php';

const ASSET_VERSION = '3';

// Every page of the app. 'nav' pages appear in the sidebar, in this order;
// 'parent' marks which sidebar entry a sub-page lights up.
function modules(): array {
    return [
        'home'    => ['label' => 'Tổng quan', 'icon' => 'home', 'nav' => true],
        'books'   => ['label' => 'Sách', 'icon' => 'books', 'nav' => true],
        'book'    => ['parent' => 'books'],
        'vocab'   => ['label' => 'Từ vựng', 'icon' => 'cards', 'nav' => true],
        'deck'    => ['parent' => 'vocab'],
        'device'  => ['label' => 'Thiết bị', 'icon' => 'device', 'nav' => true],
        'account' => ['label' => 'Tài khoản', 'icon' => 'user', 'nav' => true],
    ];
}

function pageFile(string $page): ?string {
    if (!isset(modules()[$page])) return null;
    $file = __DIR__ . '/pages/' . $page . '.php';
    return is_file($file) ? $file : null;
}

// --- icons ------------------------------------------------------------------------------

function icon(string $name, string $class = 'ic'): string {
    static $paths = [
        'home'     => '<path d="M3 11 12 4l9 7"/><path d="M5 10v10h5v-6h4v6h5V10"/>',
        'books'    => '<path d="M4 19V5a2 2 0 0 1 2-2h4v16H6a2 2 0 0 0-2 2Z"/><path d="M10 3h4v16h-4"/><path d="m15 4 3.8-.9 3 14.6-3.8.9Z"/>',
        'cards'    => '<rect x="3" y="6" width="14" height="14" rx="2"/><path d="M7 3h12a2 2 0 0 1 2 2v12"/><path d="M7 11h6M7 15h4"/>',
        'device'   => '<rect x="5" y="2" width="14" height="20" rx="2.5"/><path d="M9 6h6M9 10h6M9 14h3"/>',
        'user'     => '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>',
        'search'   => '<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>',
        'send'     => '<path d="M21 3 10 14"/><path d="m21 3-7 18-4-7-7-4Z"/>',
        'download' => '<path d="M12 3v12"/><path d="m7 10 5 5 5-5"/><path d="M4 21h16"/>',
        'upload'   => '<path d="M12 21V9"/><path d="m7 14 5-5 5 5"/><path d="M4 3h16"/>',
        'star'     => '<path d="m12 3 2.7 5.6 6.1.8-4.5 4.2 1.1 6.1L12 16.8l-5.4 2.9 1.1-6.1-4.5-4.2 6.1-.8Z"/>',
        'logout'   => '<path d="M15 4h4v16h-4"/><path d="M10 17l5-5-5-5"/><path d="M15 12H3"/>',
        'globe'    => '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>',
        'grid'     => '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>',
        'list'     => '<path d="M8 6h13M8 12h13M8 18h13"/><circle cx="4" cy="6" r="1"/><circle cx="4" cy="12" r="1"/><circle cx="4" cy="18" r="1"/>',
        'folder'   => '<path d="M3 6a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/>',
        'clock'    => '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
        'check'    => '<path d="m5 12 5 5L20 7"/>',
        'alert'    => '<path d="M12 3 2 20h20Z"/><path d="M12 10v4M12 17h.01"/>',
        'key'      => '<circle cx="8" cy="15" r="4"/><path d="m11 12 9-9M17 6l3 3M15 8l2 2"/>',
        'server'   => '<rect x="3" y="4" width="18" height="7" rx="1.5"/><rect x="3" y="13" width="18" height="7" rx="1.5"/><path d="M7 7.5h.01M7 16.5h.01"/>',
        'theme'    => '<circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 0 0 18Z" fill="currentColor"/>',
        'chevron'  => '<path d="m9 6 6 6-6 6"/>',
        'plus'     => '<path d="M12 5v14M5 12h14"/>',
        'code'     => '<path d="m8 8-5 4 5 4M16 8l5 4-5 4M14 4l-4 16"/>',
    ];
    return '<svg class="' . h($class) . '" viewBox="0 0 24 24" aria-hidden="true">' . ($paths[$name] ?? '') . '</svg>';
}

// --- layout ------------------------------------------------------------------------------

function layout(string $title, string $active, callable $body): void {
    global $flash;
    $mods = modules();
    $navActive = $mods[$active]['parent'] ?? $active;
    $pending = $active !== '' ? (int)db()->query('SELECT COUNT(*) FROM devices WHERE approved = 0')->fetchColumn() : 0;
    ?><!doctype html>
<html lang="vi"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex, nofollow"><meta name="color-scheme" content="light dark">
<title><?= h($title) ?> · Thư viện CrossPoint</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect x='5' y='3' width='22' height='26' rx='4' fill='%231d1c1a'/%3E%3Crect x='9' y='8' width='14' height='2' fill='%23f4f1ea'/%3E%3Crect x='9' y='13' width='14' height='2' fill='%23f4f1ea'/%3E%3Crect x='9' y='18' width='9' height='2' fill='%23f4f1ea'/%3E%3C/svg%3E">
<script>try{var t=localStorage.getItem('theme');if(t)document.documentElement.dataset.theme=t}catch(e){}</script>
<link rel="stylesheet" href="style.css?v=<?= ASSET_VERSION ?>"></head>
<body class="<?= $active === '' ? 'bare' : 'app' ?>">
<?php if ($active !== ''): ?>
<aside class="side">
  <a class="brand" href="index.php"><?= brandMark() ?><span>CrossPoint<small>Thư viện</small></span></a>
  <nav class="nav">
    <?php foreach ($mods as $id => $m): if (empty($m['nav'])) continue; ?>
      <a href="<?= $id === 'home' ? 'index.php' : '?p=' . h($id) ?>" class="<?= $navActive === $id ? 'on' : '' ?>">
        <?= icon($m['icon']) ?><span><?= h($m['label']) ?></span>
        <?php if ($id === 'device' && $pending): ?><em class="badge" title="Máy chờ duyệt"><?= $pending ?></em><?php endif ?>
      </a>
    <?php endforeach ?>
  </nav>
  <div class="side-foot">
    <a href="/"><?= icon('globe') ?><span>Trang firmware</span></a>
    <a href="/guide/"><?= icon('books') ?><span>Hướng dẫn</span></a>
    <button type="button" class="linklike" data-theme-toggle><?= icon('theme') ?><span>Sáng / tối</span></button>
    <a href="?p=logout"><?= icon('logout') ?><span>Đăng xuất</span></a>
  </div>
</aside>
<div class="shell">
  <header class="topbar">
    <form class="topsearch" method="get" action="index.php" role="search">
      <input type="hidden" name="p" value="books">
      <?= icon('search') ?><input name="q" value="<?= h($active === 'books' ? (string)($_GET['q'] ?? '') : '') ?>" placeholder="Tìm sách, tác giả, thể loại…" aria-label="Tìm sách">
    </form>
  </header>
  <main>
<?php else: ?>
  <main class="center">
<?php endif ?>
<?php if (!empty($flash)): ?><div class="flash" role="status"><?= icon('check') ?><span><?= h($flash) ?></span></div><?php endif ?>
<?php $body() ?>
  </main>
<?php if ($active !== ''): ?></div><?php endif ?>
<script>
document.querySelectorAll('[data-theme-toggle]').forEach(function (b) {
  b.addEventListener('click', function () {
    var r = document.documentElement;
    var dark = r.dataset.theme ? r.dataset.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
    r.dataset.theme = dark ? 'light' : 'dark';
    try { localStorage.setItem('theme', r.dataset.theme); } catch (e) {}
  });
});
document.querySelectorAll('form[data-confirm]').forEach(function (f) {
  f.addEventListener('submit', function (e) { if (!confirm(f.dataset.confirm)) e.preventDefault(); });
});
</script>
</body></html>
<?php
}

function brandMark(): string {
    return '<svg class="mark" viewBox="0 0 32 32" aria-hidden="true"><rect x="5" y="3" width="22" height="26" rx="4" fill="currentColor"/>' .
           '<rect x="9" y="8" width="14" height="2" fill="var(--paper)"/><rect x="9" y="13" width="14" height="2" fill="var(--paper)"/>' .
           '<rect x="9" y="18" width="9" height="2" fill="var(--paper)"/></svg>';
}

function pageHead(string $title, string $sub = '', string $actions = ''): void { ?>
    <div class="pagehead">
      <div><h1><?= h($title) ?></h1><?php if ($sub !== ''): ?><p class="sub"><?= $sub ?></p><?php endif ?></div>
      <?php if ($actions !== ''): ?><div class="actions"><?= $actions ?></div><?php endif ?>
    </div>
<?php }

function notFound(string $what, string $active): never {
    http_response_code(404);
    layout('Không tìm thấy', $active, function () use ($what) { ?>
        <div class="empty"><?= icon('alert') ?><h2>Không tìm thấy <?= h($what) ?></h2><p><a href="index.php">Về trang tổng quan</a></p></div>
    <?php });
    exit;
}

// --- forms --------------------------------------------------------------------------------

function backHere(): string {
    return '?' . ($_SERVER['QUERY_STRING'] ?? '');
}

function hiddenFields(string $action, array $fields, ?string $back = null): string {
    $html = '<input type="hidden" name="csrf" value="' . h(csrfToken()) . '">' .
            '<input type="hidden" name="action" value="' . h($action) . '">' .
            '<input type="hidden" name="back" value="' . h($back ?? backHere()) . '">';
    foreach ($fields as $k => $v) $html .= '<input type="hidden" name="' . h((string)$k) . '" value="' . h((string)$v) . '">';
    return $html;
}

// A one-button form. $confirm asks before submitting.
function postButton(string $action, array $fields, string $label, string $class = '', string $confirm = '', string $iconName = ''): string {
    return '<form method="post" class="inline"' . ($confirm !== '' ? ' data-confirm="' . h($confirm) . '"' : '') . '>' .
           hiddenFields($action, $fields) .
           '<button class="btn ' . h($class) . '">' . ($iconName !== '' ? icon($iconName) : '') . h($label) . '</button></form>';
}

// "Send" for a book or word list: one button per approved reader when there are
// one or two, a picker beyond that, the shared inbox when none is approved.
function sendControls(string $action, array $fields, string $class = 'primary'): string {
    $targets = devices(true);
    if (!$targets) return postButton($action, $fields + ['to' => ''], 'Gửi tới hộp thư chung', $class, '', 'send');
    if (count($targets) <= 2) {
        return implode(' ', array_map(fn($d) => postButton($action, $fields + ['to' => $d['id']], 'Gửi tới ' . deviceLabel($d), $class, '', 'send'), $targets));
    }
    $html = '<form method="post" class="inline sendpick">' . hiddenFields($action, $fields) . '<select name="to" aria-label="Chọn máy">';
    foreach ($targets as $d) $html .= '<option value="' . h($d['id']) . '">' . h(deviceLabel($d)) . '</option>';
    return $html . '</select><button class="btn ' . h($class) . '">' . icon('send') . 'Gửi</button></form>';
}

function query(array $change): string {
    $q = array_merge($_GET, $change);
    $q = array_filter($q, fn($v) => $v !== '' && $v !== null);
    return '?' . http_build_query($q);
}

// Numbered pager: first, last and two pages around the current one.
function pager(int $page, int $pages, string $param = 'n'): string {
    if ($pages <= 1) return '';
    $html = '<nav class="pager" aria-label="Trang">';
    $html .= $page > 1 ? '<a href="' . h(query([$param => $page - 1])) . '" rel="prev">‹</a>' : '<span class="off">‹</span>';
    $last = 0;
    foreach (range(1, $pages) as $p) {
        if ($p !== 1 && $p !== $pages && abs($p - $page) > 2) continue;
        if ($last && $p > $last + 1) $html .= '<span class="gap">…</span>';
        $html .= $p === $page ? '<span class="on">' . $p . '</span>' : '<a href="' . h(query([$param => $p])) . '">' . $p . '</a>';
        $last = $p;
    }
    $html .= $page < $pages ? '<a href="' . h(query([$param => $page + 1])) . '" rel="next">›</a>' : '<span class="off">›</span>';
    return $html . '</nav>';
}

// --- books ---------------------------------------------------------------------------------

const STATUS_LABEL = ['want' => 'Muốn đọc', 'reading' => 'Đang đọc', 'read' => 'Đã đọc'];
const LANG_LABEL = ['vi' => 'Tiếng Việt', 'en' => 'Tiếng Anh', 'fr' => 'Tiếng Pháp', 'zh' => 'Tiếng Trung', 'ja' => 'Tiếng Nhật'];

function coverHtml(array $b): string {
    if ($b['cover']) return '<img loading="lazy" src="cover.php?id=' . (int)$b['id'] . '" alt="">';
    $initials = mb_strtoupper(mb_substr($b['title'], 0, 1));
    $hue = crc32($b['title']) % 360;
    return '<div class="nocover" style="--h:' . $hue . '"><span>' . h($initials) . '</span><small>' . h(strtoupper($b['format'])) . '</small></div>';
}

function bookCard(array $b): string {
    return '<a class="book" href="?p=book&id=' . (int)$b['id'] . '">' .
           '<div class="cover">' . coverHtml($b) .
           ($b['status'] ? '<em>' . h(STATUS_LABEL[$b['status']] ?? '') . '</em>' : '') .
           ($b['favorite'] ? '<i class="fav" title="Yêu thích">★</i>' : '') . '</div>' .
           '<b>' . h($b['title']) . '</b><small>' . h($b['author'] ?: '—') . '</small></a>';
}

function readableOnReader(array $b): bool {
    return in_array($b['format'], INBOX_EXT, true);
}

function ago(int $t): string {
    $d = time() - $t;
    if ($d < 120) return 'vừa xong';
    if ($d < 3600) return intdiv($d, 60) . ' phút trước';
    if ($d < 86400) return intdiv($d, 3600) . ' giờ trước';
    if ($d < 86400 * 30) return intdiv($d, 86400) . ' ngày trước';
    return date('d/m/Y', $t);
}

function num(int $n): string {
    return number_format($n, 0, ',', '.');
}
