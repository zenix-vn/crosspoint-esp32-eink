<?php
// Admin pages (?p=admin&t=...): overview, users and one user, readers,
// history (with CSV export) and site settings. Included by public/index.php
// for the admin only; $me is the signed-in admin.

declare(strict_types=1);

const ADMIN_TABS = ['overview', 'users', 'user', 'devices', 'errors', 'feedback', 'history', 'settings'];
const HISTORY_PER_PAGE = 50;

function adminUrl(string $tab, array $params = []): string {
    return '?' . http_build_query(['p' => 'admin', 't' => $tab] + $params);
}

function adminTabs(string $active): string {
    $pdo = db();
    return tabs([
        'overview' => ['Tổng quan', adminUrl('overview'), null],
        'users'    => ['Người dùng', adminUrl('users'), (int)$pdo->query('SELECT COUNT(*) FROM users')->fetchColumn()],
        'devices'  => ['Thiết bị', adminUrl('devices'), (int)$pdo->query('SELECT COUNT(*) FROM devices')->fetchColumn()],
        'errors'   => ['Lỗi thiết bị', adminUrl('errors'), (int)$pdo->query("SELECT COUNT(*) FROM error_reports WHERE status = 'open'")->fetchColumn()],
        'feedback' => ['Góp ý', adminUrl('feedback'), (int)$pdo->query("SELECT COUNT(*) FROM feedback WHERE status = 'new'")->fetchColumn()],
        'history'  => ['Lịch sử', adminUrl('history'), null],
        'settings' => ['Cài đặt', adminUrl('settings'), null],
    ], $active === 'user' ? 'users' : $active);
}

// --- CSV export of the filtered history --------------------------------------------------
$tab = in_array($_GET['t'] ?? '', ADMIN_TABS, true) ? (string)$_GET['t'] : 'overview';
$filters = ['u' => (int)($_GET['u'] ?? 0), 'a' => (string)($_GET['a'] ?? ''), 'd' => (int)($_GET['d'] ?? 0), 'q' => trim((string)($_GET['q'] ?? ''))];
if ($tab === 'history' && isset($_GET['csv'])) {
    [$where, $args] = eventFilter($filters);
    header('Content-Type: text/csv; charset=utf-8');
    header('Content-Disposition: attachment; filename="crosspoint-lich-su-' . date('Ymd-Hi') . '.csv"');
    $out = fopen('php://output', 'w');
    fwrite($out, "\xEF\xBB\xBF");  // Excel reads UTF-8 only with a BOM
    fputcsv($out, ['Thời gian', 'Người dùng', 'Việc', 'Chi tiết', 'Máy', 'IP']);
    foreach (events($where, $args, 100000) as $e) {
        fputcsv($out, [date('Y-m-d H:i:s', (int)$e['time']), $e['who'] ?? '', EVENT_LABEL[$e['action']] ?? $e['action'], $e['detail'],
                       $e['device_id'] ? ($e['device_name'] ?: $e['device_id']) : '', $e['ip']]);
    }
    exit;
}

// --- overview ---------------------------------------------------------------------------------
if ($tab === 'overview') {
    layout('Quản trị', 'admin', function () {
        $week = time() - 7 * 86400;
        $twoWeeks = time() - 14 * 86400;
        $day = time() - 86400;
        $users = scalar('SELECT COUNT(*) FROM users');
        $newWeek = scalar('SELECT COUNT(*) FROM users WHERE created > ?', [$week]);
        $newPrev = scalar('SELECT COUNT(*) FROM users WHERE created > ? AND created <= ?', [$twoWeeks, $week]);
        $linked = scalar('SELECT COUNT(*) FROM devices WHERE user_id IS NOT NULL');
        $activeDevices = scalar('SELECT COUNT(*) FROM devices WHERE user_id IS NOT NULL AND last_seen > ?', [$week]);
        $sends = scalar("SELECT COUNT(*) FROM events WHERE action = 'send' AND time > ?", [$week]);
        $sendsPrev = scalar("SELECT COUNT(*) FROM events WHERE action = 'send' AND time > ? AND time <= ?", [$twoWeeks, $week]);
        $received = scalar("SELECT COUNT(*) FROM events WHERE action = 'receive' AND time > ?", [$week]);
        $activeUsers = scalar('SELECT COUNT(DISTINCT user_id) FROM events WHERE user_id IS NOT NULL AND time > ?', [$week]);
        $badKeys = scalar("SELECT COUNT(*) FROM events WHERE action = 'bad_key' AND time > ?", [$day]);
        $badLogins = scalar("SELECT COUNT(*) FROM events WHERE action = 'login_fail' AND time > ?", [$day]);
        $disabled = scalar('SELECT COUNT(*) FROM users WHERE disabled = 1');
        $stale = db()->query('SELECT d.*, COALESCE(NULLIF(u.email, \'\'), u.login) AS owner FROM devices d JOIN users u ON u.id = d.user_id
                              WHERE d.last_seen < ' . (time() - 3 * 86400) . ' ORDER BY d.last_seen')->fetchAll();
        $stale = array_values(array_filter($stale, fn($d) => count(inboxWaiting($d['id'])) > 0));
        $latestUsers = db()->query('SELECT * FROM users ORDER BY created DESC LIMIT 5')->fetchAll();
        $top = db()->prepare("SELECT detail, COUNT(*) AS n FROM events WHERE action = 'send' AND time > ? GROUP BY detail ORDER BY n DESC LIMIT 6");
        $top->execute([time() - 30 * 86400]);
        $top = $top->fetchAll();
        $daily = dailyCounts(['send', 'receive', 'register'], 14);
        ?>
        <div class="page-head">
          <div><h1>Quản trị</h1><p class="lead">Người dùng, máy đọc sách và mọi việc đã diễn ra trên crosspoint.zenix.vn.</p></div>
          <div class="actions"><a class="button" href="<?= h(adminUrl('history', ['csv' => 1, 'd' => 30])) ?>">Xuất lịch sử 30 ngày (CSV)</a></div>
        </div>
        <?= adminTabs('overview') ?>

        <div class="tiles">
          <?= statTile(num($users), 'tài khoản', adminUrl('users'), delta($newWeek, $newPrev, '7 ngày') . ' đăng ký mới') ?>
          <?= statTile(num($activeUsers), 'người dùng hoạt động', adminUrl('users', ['sort' => 'active']), 'trong 7 ngày qua') ?>
          <?= statTile(num($linked), 'máy đã liên kết', adminUrl('devices'), num($activeDevices) . ' máy kết nối trong 7 ngày') ?>
          <?= statTile(num($sends), 'lượt gửi tới máy', adminUrl('history', ['a' => 'send', 'd' => 7]), delta($sends, $sendsPrev, '7 ngày')) ?>
          <?= statTile(num($received), 'tệp máy đã nhận', adminUrl('history', ['a' => 'receive', 'd' => 7]), $sends ? round(min(1, $received / $sends) * 100) . '% số lượt gửi' : 'trong 7 ngày qua') ?>
        </div>

        <div class="cols">
          <section class="card span2">
            <?= sectionHead('Hoạt động 14 ngày', 'Xem lịch sử', adminUrl('history', ['d' => 14])) ?>
            <?= barChart($daily, ['send' => 'Lượt gửi', 'receive' => 'Máy đã nhận', 'register' => 'Đăng ký']) ?>
          </section>
          <section class="card">
            <?= sectionHead('Cần chú ý') ?>
            <ul class="checks">
              <li class="<?= $badKeys ? 'warn' : 'ok' ?>"><a href="<?= h(adminUrl('history', ['a' => 'bad_key', 'd' => 1])) ?>"><b><?= $badKeys ?></b> lần nhập khoá sai trong 24 giờ</a></li>
              <li class="<?= $badLogins >= 5 ? 'warn' : 'ok' ?>"><a href="<?= h(adminUrl('history', ['a' => 'login_fail', 'd' => 1])) ?>"><b><?= $badLogins ?></b> lần đăng nhập sai trong 24 giờ</a></li>
              <li class="<?= $stale ? 'warn' : 'ok' ?>"><a href="<?= h(adminUrl('devices', ['f' => 'waiting'])) ?>"><b><?= count($stale) ?></b> máy có sách chờ nhưng 3 ngày chưa kết nối</a></li>
              <li class="<?= $disabled ? 'info' : 'ok' ?>"><a href="<?= h(adminUrl('users', ['f' => 'disabled'])) ?>"><b><?= $disabled ?></b> tài khoản đang bị khoá</a></li>
              <li class="<?= googleConfig() ? 'ok' : 'warn' ?>"><a href="<?= h(adminUrl('settings')) ?>">Đăng nhập Google <?= googleConfig() ? 'đang bật' : 'chưa bật' ?></a></li>
            </ul>
          </section>
        </div>

        <div class="cols">
          <section class="card">
            <?= sectionHead('Mới đăng ký', 'Tất cả người dùng', adminUrl('users')) ?>
            <?php if (!$latestUsers): ?><p class="empty-note">Chưa có ai.</p><?php endif ?>
            <ul class="people">
              <?php foreach ($latestUsers as $u): ?>
                <li><a href="<?= h(adminUrl('user', ['u' => $u['id']])) ?>"><?= avatar($u) ?><span><b><?= h(userLabel($u)) ?></b><small><?= h($u['email'] ?: $u['login']) ?></small></span></a>
                  <time><?= h(ago((int)$u['created'])) ?></time></li>
              <?php endforeach ?>
            </ul>
          </section>
          <section class="card">
            <?= sectionHead('Hoạt động gần đây', 'Xem tất cả', adminUrl('history')) ?>
            <?= eventList(events("e.action NOT IN ('login_fail', 'bad_key')", [], 8), true) ?>
          </section>
          <section class="card">
            <?= sectionHead('Gửi nhiều nhất (30 ngày)', 'Lịch sử gửi', adminUrl('history', ['a' => 'send', 'd' => 30])) ?>
            <?php if (!$top): ?><p class="empty-note">Chưa có lượt gửi nào.</p><?php endif ?>
            <ol class="ranking">
              <?php foreach ($top as $t): ?>
                <li><a href="<?= h(adminUrl('history', ['a' => 'send', 'q' => preg_replace('/^[^:]+:\s*/u', '', $t['detail'])])) ?>"><?= h($t['detail']) ?></a><span><?= (int)$t['n'] ?></span></li>
              <?php endforeach ?>
            </ol>
          </section>
        </div>
    <?php });
    exit;
}

// --- users --------------------------------------------------------------------------------
if ($tab === 'users') {
    layout('Người dùng · Quản trị', 'admin', function () {
        $q = trim((string)($_GET['q'] ?? ''));
        $f = (string)($_GET['f'] ?? '');
        $sort = (string)($_GET['sort'] ?? 'new');
        $where = [];
        $args = [];
        if ($q !== '') { $where[] = '(u.email LIKE ? OR u.name LIKE ? OR u.login LIKE ?)'; array_push($args, "%$q%", "%$q%", "%$q%"); }
        if ($f === 'disabled') $where[] = 'u.disabled = 1';
        elseif ($f === 'google') $where[] = 'u.google_sub IS NOT NULL';
        elseif ($f === 'nodevice') $where[] = 'NOT EXISTS (SELECT 1 FROM devices d WHERE d.user_id = u.id)';
        elseif ($f === 'admin') $where[] = "u.role = 'admin'";
        $order = ['new' => 'u.created DESC', 'login' => 'u.last_login DESC', 'sends' => 'sends DESC, u.created DESC',
                  'active' => 'last_event DESC'][$sort] ?? 'u.created DESC';
        $st = db()->prepare("SELECT u.*,
                (SELECT COUNT(*) FROM devices d WHERE d.user_id = u.id) AS devices,
                (SELECT COUNT(*) FROM events e WHERE e.user_id = u.id AND e.action = 'send') AS sends,
                (SELECT MAX(time) FROM events e WHERE e.user_id = u.id) AS last_event
            FROM users u" . ($where ? ' WHERE ' . implode(' AND ', $where) : '') . " ORDER BY $order");
        $st->execute($args);
        $rows = $st->fetchAll();
        ?>
        <div class="page-head"><div><h1>Người dùng</h1><p class="lead">Ai cũng tự đăng ký được, không cần duyệt. Bấm vào một người để xem máy, tệp và lịch sử của họ.</p></div></div>
        <?= adminTabs('users') ?>
        <form class="toolbar" method="get">
          <input type="hidden" name="p" value="admin"><input type="hidden" name="t" value="users">
          <input type="search" name="q" value="<?= h($q) ?>" placeholder="Tìm theo email hoặc tên" aria-label="Tìm người dùng">
          <select name="f" aria-label="Lọc">
            <?php foreach (['' => 'Tất cả', 'disabled' => 'Đã khoá', 'google' => 'Dùng Google', 'nodevice' => 'Chưa có máy', 'admin' => 'Quản trị'] as $k => $l): ?>
              <option value="<?= h($k) ?>" <?= $f === $k ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
          </select>
          <select name="sort" aria-label="Sắp xếp">
            <?php foreach (['new' => 'Mới đăng ký', 'active' => 'Hoạt động gần nhất', 'login' => 'Đăng nhập gần nhất', 'sends' => 'Gửi nhiều nhất'] as $k => $l): ?>
              <option value="<?= h($k) ?>" <?= $sort === $k ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
          </select>
          <button>Lọc</button>
          <?php if ($q !== '' || $f !== ''): ?><a class="button quiet" href="<?= h(adminUrl('users')) ?>">Bỏ lọc</a><?php endif ?>
        </form>
        <section class="card flush">
          <?php if (!$rows): ?><p class="empty-note pad">Không có người dùng nào khớp.</p><?php else: ?>
          <div class="scroll"><table class="data">
            <thead><tr><th>Người dùng</th><th>Đăng nhập bằng</th><th class="num">Máy</th><th class="num">Lượt gửi</th><th>Hoạt động</th><th>Đăng ký</th></tr></thead>
            <tbody>
            <?php foreach ($rows as $u): $href = adminUrl('user', ['u' => $u['id']]); ?>
              <tr class="<?= $u['disabled'] ? 'off' : '' ?>">
                <td><a class="who" href="<?= h($href) ?>"><?= avatar($u) ?><span><b><?= h(userLabel($u)) ?></b><small><?= h($u['email'] ?: $u['login']) ?></small></span></a>
                  <?= isAdmin($u) ? '<span class="badge dark">Quản trị</span>' : '' ?><?= $u['disabled'] ? '<span class="badge red">Đã khoá</span>' : '' ?></td>
                <td><?= signInMethods($u) ?></td>
                <td class="num"><a href="<?= h($href) ?>#devices"><?= (int)$u['devices'] ?></a></td>
                <td class="num"><a href="<?= h(adminUrl('history', ['u' => $u['id'], 'a' => 'send'])) ?>"><?= (int)$u['sends'] ?></a></td>
                <td><?= $u['last_event'] ? h(ago((int)$u['last_event'])) : '<span class="muted">chưa</span>' ?></td>
                <td title="<?= date('d/m/Y H:i', (int)$u['created']) ?>"><?= date('d/m/Y', (int)$u['created']) ?></td>
              </tr>
            <?php endforeach ?>
            </tbody>
          </table></div>
          <?php endif ?>
        </section>
    <?php });
    exit;
}

// --- one user -----------------------------------------------------------------------------
if ($tab === 'user') {
    $u = userById((int)($_GET['u'] ?? 0));
    if (!$u) { http_response_code(404); layout('Không tìm thấy', 'admin', fn() => print('<div class="card"><p>Không có người dùng này. <a href="' . h(adminUrl('users')) . '">Về danh sách</a></p></div>')); exit; }
    layout(userLabel($u) . ' · Quản trị', 'admin', function () use ($u) {
        $uid = (int)$u['id'];
        $devs = devicesOf($uid);
        $files = userFiles($uid);
        $n = max(1, (int)($_GET['n'] ?? 1));
        $total = countEvents('e.user_id = ?', [$uid]);
        $sends = scalar("SELECT COUNT(*) FROM events WHERE user_id = ? AND action = 'send'", [$uid]);
        $received = scalar("SELECT COUNT(*) FROM events WHERE user_id = ? AND action = 'receive'", [$uid]);
        ?>
        <p class="crumbs"><a href="<?= h(adminUrl('overview')) ?>">Quản trị</a> › <a href="<?= h(adminUrl('users')) ?>">Người dùng</a> › <?= h(userLabel($u)) ?></p>
        <div class="profile-head card">
          <?= avatar($u, 'xl') ?>
          <div class="grow">
            <h1><?= h(userLabel($u)) ?> <?= isAdmin($u) ? '<span class="badge dark">Quản trị</span>' : '' ?><?= $u['disabled'] ? '<span class="badge red">Đã khoá</span>' : '' ?></h1>
            <p class="muted"><?= h($u['email'] ?: $u['login']) ?> · <?= signInMethods($u) ?></p>
            <p class="muted small">Đăng ký <?= date('d/m/Y H:i', (int)$u['created']) ?> · đăng nhập <?= h(ago((int)$u['last_login'])) ?> · khoá liên kết: <?= $u['link_key'] ? '<code>' . h(formatLinkKey($u['link_key'])) . '</code>' : 'chưa tạo' ?></p>
          </div>
          <?php if (!isAdmin($u)): ?><div class="actions">
            <?= postButton($u['disabled'] ? 'userenable' : 'userdisable', ['user' => $uid], $u['disabled'] ? 'Mở khoá tài khoản' : 'Khoá tài khoản', '', $u['disabled'] ? '' : 'Khoá tài khoản này? Người dùng không đăng nhập được và máy của họ ngừng nhận sách.') ?>
            <?= postButton('userdelete', ['user' => $uid], 'Xoá tài khoản', 'danger', 'Xoá hẳn tài khoản ' . ($u['email'] ?: $u['login']) . ' cùng các tệp đã tải lên? Máy của họ sẽ bị gỡ. Không hoàn tác được.') ?>
          </div><?php endif ?>
        </div>
        <div class="tiles">
          <?= statTile((string)count($devs), 'máy đọc sách', '#devices') ?>
          <?= statTile(num($sends), 'lượt gửi', adminUrl('history', ['u' => $uid, 'a' => 'send'])) ?>
          <?= statTile(num($received), 'tệp máy đã nhận', adminUrl('history', ['u' => $uid, 'a' => 'receive'])) ?>
          <?= statTile((string)count($files), 'tệp tải lên', '#files', humanSize(userUsage($uid)) . ' / ' . humanSize(USER_QUOTA)) ?>
        </div>
        <div class="cols two">
          <section class="card" id="devices">
            <?= sectionHead('Máy đọc sách') ?>
            <?php if (!$devs): ?><p class="empty-note">Chưa liên kết máy nào.</p><?php endif ?>
            <ul class="devlist">
              <?php foreach ($devs as $d): $w = count(inboxWaiting($d['id'])); ?>
                <li><div><b><?= h(deviceLabel($d)) ?></b><small><?= h($d['model'] ?: '?') ?> · <?= h($d['firmware'] ?: '?') ?><?= (int)$d['battery'] >= 0 ? ' · pin ' . (int)$d['battery'] . '%' : '' ?> · kết nối <?= h(ago((int)$d['last_seen'])) ?><?= $w ? ' · <b>' . $w . ' tệp đang chờ</b>' : '' ?></small></div>
                  <?= postButton('unlinkdevice', ['device' => $d['id']], 'Gỡ', 'small', 'Gỡ máy này khỏi tài khoản của người dùng?') ?></li>
              <?php endforeach ?>
            </ul>
          </section>
          <section class="card" id="files">
            <?= sectionHead('Tệp tải lên') ?>
            <?php if (!$files): ?><p class="empty-note">Chưa tải lên tệp nào.</p><?php endif ?>
            <ul class="filelist">
              <?php foreach (array_slice($files, 0, 12) as $f): ?>
                <li><span><?= h($f['name']) ?></span><small><?= $f['kind'] === 'deck' ? 'Bộ từ' : 'Sách' ?> · <?= h(humanSize((int)$f['size'])) ?> · <?= date('d/m/Y', (int)$f['mtime']) ?></small></li>
              <?php endforeach ?>
              <?php if (count($files) > 12): ?><li class="muted">và <?= count($files) - 12 ?> tệp khác</li><?php endif ?>
            </ul>
          </section>
        </div>
        <section class="card">
          <?= sectionHead('Lịch sử (' . num($total) . ')', 'Lọc chi tiết', adminUrl('history', ['u' => $uid])) ?>
          <?= eventList(events('e.user_id = ?', [$uid], HISTORY_PER_PAGE, ($n - 1) * HISTORY_PER_PAGE), false) ?>
          <?= pageLinks($n, max(1, (int)ceil($total / HISTORY_PER_PAGE))) ?>
        </section>
    <?php });
    exit;
}

// --- devices ------------------------------------------------------------------------------
if ($tab === 'devices') {
    layout('Thiết bị · Quản trị', 'admin', function () {
        $f = (string)($_GET['f'] ?? '');
        $q = trim((string)($_GET['q'] ?? ''));
        $rows = allDevices();
        foreach ($rows as &$d) $d['waiting'] = $d['user_id'] !== null ? count(inboxWaiting($d['id'])) : 0;
        unset($d);
        $rows = array_values(array_filter($rows, function ($d) use ($f, $q) {
            if ($f === 'linked' && $d['user_id'] === null) return false;
            if ($f === 'unlinked' && $d['user_id'] !== null) return false;
            if ($f === 'waiting' && !$d['waiting']) return false;
            if ($f === 'quiet' && $d['last_seen'] > time() - 7 * 86400) return false;
            if ($q !== '' && stripos($d['name'] . ' ' . $d['id'] . ' ' . $d['model'] . ' ' . ($d['owner_email'] ?? ''), $q) === false) return false;
            return true;
        }));
        $models = [];
        foreach (allDevices() as $d) $models[$d['model'] ?: '?'] = ($models[$d['model'] ?: '?'] ?? 0) + 1;
        arsort($models);
        ?>
        <div class="page-head"><div><h1>Thiết bị</h1><p class="lead">Mọi máy đọc sách đã liên lạc với máy chủ. Máy vào một tài khoản khi người dùng nhập khoá liên kết trên máy.</p></div></div>
        <?= adminTabs('devices') ?>
        <div class="chips">
          <?php foreach ($models as $m => $c): ?><span class="badge"><?= h($m) ?> · <?= $c ?></span><?php endforeach ?>
        </div>
        <form class="toolbar" method="get">
          <input type="hidden" name="p" value="admin"><input type="hidden" name="t" value="devices">
          <input type="search" name="q" value="<?= h($q) ?>" placeholder="Tên máy, mã máy, email chủ máy" aria-label="Tìm máy">
          <select name="f" aria-label="Lọc">
            <?php foreach (['' => 'Tất cả', 'linked' => 'Đã liên kết', 'unlinked' => 'Chưa liên kết', 'waiting' => 'Có tệp đang chờ', 'quiet' => '7 ngày chưa kết nối'] as $k => $l): ?>
              <option value="<?= h($k) ?>" <?= $f === $k ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
          </select>
          <button>Lọc</button>
          <?php if ($q !== '' || $f !== ''): ?><a class="button quiet" href="<?= h(adminUrl('devices')) ?>">Bỏ lọc</a><?php endif ?>
        </form>
        <section class="card flush">
          <?php if (!$rows): ?><p class="empty-note pad">Không có máy nào khớp.</p><?php else: ?>
          <div class="scroll"><table class="data">
            <thead><tr><th>Máy</th><th>Chủ máy</th><th>Firmware</th><th class="num">Pin</th><th class="num">Đang chờ</th><th>Kết nối</th><th></th></tr></thead>
            <tbody>
            <?php foreach ($rows as $d): ?>
              <tr>
                <td><b><?= h(deviceLabel($d)) ?></b><br><small class="muted"><?= h(strtoupper(implode(':', str_split($d['id'], 2)))) ?> · <?= h($d['model'] ?: '?') ?></small></td>
                <td><?= $d['user_id'] !== null ? '<a href="' . h(adminUrl('user', ['u' => $d['user_id']])) . '">' . h($d['owner_email'] ?: $d['owner_login']) . '</a>' : '<span class="badge">Chưa liên kết</span>' ?></td>
                <td><?= h($d['firmware'] ?: '?') ?></td>
                <td class="num"><?= (int)$d['battery'] >= 0 ? (int)$d['battery'] . '%' : '—' ?></td>
                <td class="num"><?= $d['waiting'] ?: '—' ?></td>
                <td title="<?= date('d/m/Y H:i', (int)$d['last_seen']) ?>"><?= h(ago((int)$d['last_seen'])) ?></td>
                <td class="row-actions">
                  <a class="button small quiet" href="<?= h(adminUrl('history', ['q' => $d['id']])) ?>">Lịch sử</a>
                  <?php if ($d['user_id'] !== null): ?><?= postButton('unlinkdevice', ['device' => $d['id']], 'Gỡ', 'small', 'Gỡ máy này khỏi tài khoản của chủ máy? Tệp đang chờ sẽ bị huỷ.') ?><?php endif ?>
                  <?= postButton('removedevice', ['device' => $d['id']], 'Xoá', 'danger small', 'Xoá hẳn máy này? Máy sẽ đăng ký lại khi kết nối.') ?>
                </td>
              </tr>
            <?php endforeach ?>
            </tbody>
          </table></div>
          <?php endif ?>
        </section>
        <?php $shared = inboxWaiting(); if ($shared): ?>
          <section class="card">
            <?= sectionHead('Hộp thư chung (' . count($shared) . ')') ?>
            <p class="muted">Chỉ máy của quản trị và máy dùng khoá hộp thư chung cũ nhận các tệp này.</p>
            <ul class="filelist"><?php foreach ($shared as $f): ?>
              <li><span><?= h($f) ?></span><?= postButton('unsend', ['file' => $f, 'to' => ''], 'Huỷ', 'small') ?></li>
            <?php endforeach ?></ul>
          </section>
        <?php endif ?>
    <?php });
    exit;
}

// --- error reports from the firmware ---------------------------------------------------------
if ($tab === 'errors') {
    layout('Lỗi thiết bị · Quản trị', 'admin', function () {
        $pdo = db();
        $statusNames = ['open' => 'Đang mở', 'fixed' => 'Đã sửa', 'ignored' => 'Bỏ qua'];
        $one = (int)($_GET['r'] ?? 0);
        if ($one > 0) {
            $st = $pdo->prepare('SELECT * FROM error_reports WHERE id = ?');
            $st->execute([$one]);
            $rep = $st->fetch();
            $dev = $rep ? deviceById($rep['device_id']) : null;
            ?>
            <div class="page-head"><div><h1>Chi tiết lỗi</h1><p class="lead"><a href="<?= h(adminUrl('errors')) ?>">← Tất cả lỗi</a></p></div></div>
            <?= adminTabs('errors') ?>
            <?php if (!$rep): ?><p class="empty-note pad">Không tìm thấy báo lỗi này.</p><?php else: ?>
            <section class="card">
              <p><b><?= h($rep['message'] !== '' ? $rep['message'] : '(không có nội dung)') ?></b></p>
              <p class="muted"><?= h(REPORT_KINDS[$rep['kind']] ?? $rep['kind']) ?> · <?= h($rep['model'] ?: '?') ?> · firmware <?= h($rep['firmware'] ?: '?') ?>
                · <?= (int)$rep['count'] ?> lần · lần đầu <?= date('d/m/Y H:i', (int)$rep['first_seen']) ?> · gần nhất <?= date('d/m/Y H:i', (int)$rep['last_seen']) ?>
                · máy <?= $dev ? h(deviceLabel($dev)) : h($rep['device_id']) ?></p>
              <pre style="white-space:pre-wrap;word-break:break-word;max-height:60vh;overflow:auto"><?= h($rep['detail']) ?></pre>
              <form method="post" class="toolbar">
                <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>">
                <input type="hidden" name="action" value="reportstatus"><input type="hidden" name="id" value="<?= (int)$rep['id'] ?>">
                <input type="hidden" name="back" value="<?= h(adminUrl('errors', ['r' => $rep['id']])) ?>">
                <select name="status" aria-label="Trạng thái">
                  <?php foreach ($statusNames as $k => $l): ?><option value="<?= h($k) ?>" <?= $rep['status'] === $k ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
                </select>
                <input type="text" name="note" value="<?= h($rep['note']) ?>" maxlength="300" placeholder="Ghi chú (nguyên nhân, bản sửa…)" aria-label="Ghi chú">
                <button>Lưu</button>
              </form>
              <?= postButton('deletereport', ['id' => $rep['id'], 'back' => adminUrl('errors')], 'Xoá báo lỗi', 'danger small', 'Xoá báo lỗi này?') ?>
            </section>
            <?php endif ?>
            <?php return;
        }

        $f = (string)($_GET['f'] ?? 'open');
        $q = trim((string)($_GET['q'] ?? ''));
        $where = [];
        $args = [];
        if (isset($statusNames[$f])) { $where[] = 'status = ?'; $args[] = $f; } else { $f = ''; }
        if ($q !== '') {
            $where[] = '(message LIKE ? OR detail LIKE ? OR firmware LIKE ? OR model LIKE ? OR device_id LIKE ?)';
            array_push($args, ...array_fill(0, 5, '%' . $q . '%'));
        }
        $st = $pdo->prepare('SELECT * FROM error_reports' . ($where ? ' WHERE ' . implode(' AND ', $where) : '') . ' ORDER BY last_seen DESC LIMIT 200');
        $st->execute($args);
        $rows = $st->fetchAll();
        $labels = deviceLabels(allDevices());
        ?>
        <div class="page-head"><div><h1>Lỗi thiết bị</h1><p class="lead">Báo cáo treo máy và lỗi do firmware tự gửi lên. Cùng một lỗi trên cùng một máy chỉ hiện một dòng, kèm số lần.</p></div></div>
        <?= adminTabs('errors') ?>
        <form class="toolbar" method="get">
          <input type="hidden" name="p" value="admin"><input type="hidden" name="t" value="errors">
          <input type="search" name="q" value="<?= h($q) ?>" placeholder="Nội dung, firmware, kiểu máy, mã máy" aria-label="Tìm lỗi">
          <select name="f" aria-label="Trạng thái">
            <option value="" <?= $f === '' ? 'selected' : '' ?>>Tất cả</option>
            <?php foreach ($statusNames as $k => $l): ?><option value="<?= h($k) ?>" <?= $f === $k ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
          </select>
          <button>Lọc</button>
          <?= postButton('deletereport', ['id' => 0, 'back' => adminUrl('errors')], 'Xoá lỗi đã xử lý', 'small', 'Xoá mọi báo lỗi đã sửa hoặc bỏ qua?') ?>
        </form>
        <section class="card flush">
          <?php if (!$rows): ?><p class="empty-note pad">Không có báo lỗi nào.</p><?php else: ?>
          <div class="scroll"><table class="data">
            <thead><tr><th>Lỗi</th><th>Máy</th><th>Firmware</th><th class="num">Số lần</th><th>Gần nhất</th><th>Trạng thái</th><th></th></tr></thead>
            <tbody>
            <?php foreach ($rows as $e): ?>
              <tr>
                <td><a href="<?= h(adminUrl('errors', ['r' => $e['id']])) ?>"><b><?= h($e['message'] !== '' ? mb_substr($e['message'], 0, 90) : '(không có nội dung)') ?></b></a>
                  <br><small class="muted"><?= h(REPORT_KINDS[$e['kind']] ?? $e['kind']) ?><?= $e['note'] !== '' ? ' · ' . h($e['note']) : '' ?></small></td>
                <td><?= h($labels[$e['device_id']] ?? $e['device_id']) ?><br><small class="muted"><?= h($e['model'] ?: '?') ?></small></td>
                <td><?= h($e['firmware'] ?: '?') ?></td>
                <td class="num"><?= (int)$e['count'] ?></td>
                <td title="<?= date('d/m/Y H:i', (int)$e['last_seen']) ?>"><?= h(ago((int)$e['last_seen'])) ?></td>
                <td><span class="badge"><?= h($statusNames[$e['status']] ?? $e['status']) ?></span></td>
                <td class="row-actions"><?= postButton('deletereport', ['id' => $e['id'], 'back' => adminUrl('errors')], 'Xoá', 'danger small', 'Xoá báo lỗi này?') ?></td>
              </tr>
            <?php endforeach ?>
            </tbody>
          </table></div>
          <?php endif ?>
        </section>
    <?php });
    exit;
}

// --- user feedback ---------------------------------------------------------------------------
if ($tab === 'feedback') {
    layout('Góp ý · Quản trị', 'admin', function () {
        $pdo = db();
        $f = (string)($_GET['f'] ?? 'open');
        $k = (string)($_GET['k'] ?? '');
        $q = trim((string)($_GET['q'] ?? ''));
        $where = [];
        $args = [];
        if ($f === 'open') { $where[] = "f.status IN ('new', 'seen', 'working')"; }
        elseif (isset(FEEDBACK_STATUS[$f])) { $where[] = 'f.status = ?'; $args[] = $f; }
        else { $f = ''; }
        if (isset(FEEDBACK_KINDS[$k])) { $where[] = 'f.kind = ?'; $args[] = $k; } else { $k = ''; }
        if ($q !== '') {
            $where[] = '(f.title LIKE ? OR f.body LIKE ? OR f.screen LIKE ? OR u.email LIKE ?)';
            array_push($args, ...array_fill(0, 4, '%' . $q . '%'));
        }
        $st = $pdo->prepare('SELECT f.*, COALESCE(NULLIF(u.email, \'\'), u.login) AS who FROM feedback f LEFT JOIN users u ON u.id = f.user_id'
            . ($where ? ' WHERE ' . implode(' AND ', $where) : '') . ' ORDER BY f.created DESC LIMIT 100');
        $st->execute($args);
        $rows = $st->fetchAll();
        ?>
        <div class="page-head"><div><h1>Góp ý của người dùng</h1><p class="lead">Lỗi giao diện, tính năng cần sửa và đề xuất do người dùng gửi từ trang <b>Góp ý</b>. Câu trả lời hiện cho người gửi thấy.</p></div></div>
        <?= adminTabs('feedback') ?>
        <form class="toolbar" method="get">
          <input type="hidden" name="p" value="admin"><input type="hidden" name="t" value="feedback">
          <input type="search" name="q" value="<?= h($q) ?>" placeholder="Tiêu đề, nội dung, màn hình, email" aria-label="Tìm góp ý">
          <select name="k" aria-label="Loại">
            <option value="">Mọi loại</option>
            <?php foreach (FEEDBACK_KINDS as $key => $l): ?><option value="<?= h($key) ?>" <?= $k === $key ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
          </select>
          <select name="f" aria-label="Trạng thái">
            <option value="open" <?= $f === 'open' ? 'selected' : '' ?>>Chưa xong</option>
            <option value="" <?= $f === '' ? 'selected' : '' ?>>Tất cả</option>
            <?php foreach (FEEDBACK_STATUS as $key => $l): ?><option value="<?= h($key) ?>" <?= $f === $key ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
          </select>
          <button>Lọc</button>
        </form>
        <?php if (!$rows): ?><section class="card"><p class="empty-note">Không có góp ý nào.</p></section><?php endif ?>
        <?php foreach ($rows as $r): ?>
          <section class="card">
            <p><b><?= h($r['title']) ?></b> <span class="badge"><?= h(FEEDBACK_STATUS[$r['status']] ?? $r['status']) ?></span>
              <small class="muted"> · <?= h(FEEDBACK_KINDS[$r['kind']] ?? $r['kind']) ?><?= $r['screen'] !== '' ? ' · ' . h($r['screen']) : '' ?></small></p>
            <p class="muted small"><?= h($r['who'] ?: 'tài khoản đã xoá') ?> · <?= date('d/m/Y H:i', (int)$r['created']) ?>
              <?= $r['model'] !== '' || $r['firmware'] !== '' ? ' · ' . h(trim($r['model'] . ' ' . $r['firmware'])) : '' ?></p>
            <p style="white-space:pre-wrap"><?= h($r['body']) ?></p>
            <?php if ($r['image'] !== ''): ?><p><a href="?p=feedbackimg&amp;id=<?= (int)$r['id'] ?>" target="_blank" rel="noopener">Xem ảnh đính kèm</a></p><?php endif ?>
            <form method="post" class="toolbar">
              <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>">
              <input type="hidden" name="action" value="feedbackupdate"><input type="hidden" name="id" value="<?= (int)$r['id'] ?>">
              <input type="hidden" name="back" value="<?= h(adminUrl('feedback', ['f' => $f, 'k' => $k, 'q' => $q])) ?>">
              <select name="status" aria-label="Trạng thái">
                <?php foreach (FEEDBACK_STATUS as $key => $l): ?><option value="<?= h($key) ?>" <?= $r['status'] === $key ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
              </select>
              <textarea name="reply" rows="2" maxlength="2000" placeholder="Trả lời người dùng (họ sẽ thấy)" aria-label="Trả lời"><?= h($r['reply']) ?></textarea>
              <button>Lưu</button>
            </form>
            <?= postButton('feedbackdelete', ['id' => $r['id']], 'Xoá', 'danger small', 'Xoá góp ý này?') ?>
          </section>
        <?php endforeach ?>
    <?php });
    exit;
}

// --- history ------------------------------------------------------------------------------
if ($tab === 'history') {
    layout('Lịch sử · Quản trị', 'admin', function () use ($filters) {
        [$where, $args] = eventFilter($filters);
        $total = countEvents($where, $args);
        $n = max(1, (int)($_GET['n'] ?? 1));
        $pages = max(1, (int)ceil($total / HISTORY_PER_PAGE));
        $focus = $filters['u'] ? userById($filters['u']) : null;
        $groups = ['Gửi và nhận' => ['send', 'receive', 'unsend', 'upload', 'delete_file', 'deck_create'],
                   'Tài khoản' => ['register', 'login', 'login_google', 'login_fail', 'password', 'profile', 'google_link', 'google_unlink'],
                   'Thiết bị' => ['key_create', 'key_revoke', 'bad_key', 'device_link', 'device_unlink', 'device_rename'],
                   'Quản trị' => ['user_disable', 'user_enable', 'user_delete', 'setting']];
        ?>
        <div class="page-head">
          <div><h1>Lịch sử</h1><p class="lead"><?= num($total) ?> mục<?= $focus ? ' của <a href="' . h(adminUrl('user', ['u' => $focus['id']])) . '">' . h($focus['email'] ?: $focus['login']) . '</a>' : '' ?>.</p></div>
          <div class="actions"><a class="button" href="<?= h(query(['csv' => 1, 'n' => null])) ?>">Xuất CSV</a></div>
        </div>
        <?= adminTabs('history') ?>
        <form class="toolbar" method="get">
          <input type="hidden" name="p" value="admin"><input type="hidden" name="t" value="history">
          <?php if ($filters['u']): ?><input type="hidden" name="u" value="<?= (int)$filters['u'] ?>"><?php endif ?>
          <input type="search" name="q" value="<?= h($filters['q']) ?>" placeholder="Tìm trong chi tiết, IP, mã máy" aria-label="Tìm">
          <select name="a" aria-label="Việc"><option value="">Mọi việc</option>
            <?php foreach ($groups as $g => $acts): ?><optgroup label="<?= h($g) ?>">
              <?php foreach ($acts as $k): ?><option value="<?= h($k) ?>" <?= $filters['a'] === $k ? 'selected' : '' ?>><?= h(EVENT_LABEL[$k] ?? $k) ?></option><?php endforeach ?>
            </optgroup><?php endforeach ?>
          </select>
          <select name="d" aria-label="Khoảng thời gian">
            <?php foreach ([0 => 'Mọi lúc', 1 => '24 giờ qua', 7 => '7 ngày qua', 30 => '30 ngày qua', 90 => '90 ngày qua'] as $k => $l): ?>
              <option value="<?= $k ?>" <?= $filters['d'] === $k ? 'selected' : '' ?>><?= h($l) ?></option><?php endforeach ?>
          </select>
          <button>Lọc</button>
          <?php if (array_filter($filters)): ?><a class="button quiet" href="<?= h(adminUrl('history')) ?>">Bỏ lọc</a><?php endif ?>
        </form>
        <section class="card flush">
          <?php $rows = events($where, $args, HISTORY_PER_PAGE, ($n - 1) * HISTORY_PER_PAGE); ?>
          <?php if (!$rows): ?><p class="empty-note pad">Không có mục nào khớp.</p><?php else: ?>
          <div class="scroll"><table class="data">
            <thead><tr><th>Lúc</th><th>Người dùng</th><th>Việc</th><th>Chi tiết</th><th>Máy</th><th>IP</th></tr></thead>
            <tbody>
            <?php foreach ($rows as $e): ?>
              <tr>
                <td title="<?= date('d/m/Y H:i:s', (int)$e['time']) ?>" class="nowrap"><?= date('d/m H:i', (int)$e['time']) ?></td>
                <td><?= $e['user_id'] !== null ? '<a href="' . h(adminUrl('user', ['u' => $e['user_id']])) . '">' . h($e['who'] ?: '#' . $e['user_id']) . '</a>' : '<span class="muted">Khách</span>' ?></td>
                <td class="nowrap"><span class="dot a-<?= h($e['action']) ?>"></span><a href="<?= h(query(['a' => $e['action'], 'n' => null])) ?>"><?= h(EVENT_LABEL[$e['action']] ?? $e['action']) ?></a></td>
                <td><?= h($e['detail']) ?></td>
                <td><?= $e['device_id'] ? '<a href="' . h(query(['q' => $e['device_id'], 'n' => null])) . '">' . h($e['device_name'] ?: strtoupper(substr($e['device_id'], -6))) . '</a>' : '' ?></td>
                <td class="muted"><a href="<?= h(query(['q' => $e['ip'], 'n' => null])) ?>"><?= h($e['ip']) ?></a></td>
              </tr>
            <?php endforeach ?>
            </tbody>
          </table></div>
          <?php endif ?>
        </section>
        <?= pageLinks($n, $pages) ?>
    <?php });
    exit;
}

// --- settings -----------------------------------------------------------------------------
layout('Cài đặt · Quản trị', 'admin', function () {
    $flag = function (string $key, bool $on, string $label, string $hint): string {
        return '<div class="setting"><div><b>' . h($label) . '</b><p>' . h($hint) . '</p></div>' .
               postButton('setflag', ['key' => $key, 'value' => $on ? '0' : '1'], $on ? 'Đang bật' : 'Đang tắt', 'switch ' . ($on ? 'on' : ''),
                          $on ? 'Tắt "' . $label . '"?' : '') . '</div>';
    };
    $dbSize = @filesize(DB_PATH) ?: 0;
    ?>
    <div class="page-head"><div><h1>Cài đặt</h1><p class="lead">Các thay đổi có hiệu lực ngay và được ghi vào lịch sử.</p></div></div>
    <?= adminTabs('settings') ?>
    <section class="card">
      <?= sectionHead('Truy cập') ?>
      <?= $flag('registration_open', registrationOpen(), 'Cho phép đăng ký', 'Ai cũng tự tạo được tài khoản bằng email hoặc Google, không cần duyệt. Tắt thì chỉ tài khoản đã có đăng nhập được.') ?>
      <?= $flag('library_open', libraryOpen(), 'Mở kho sách chung', 'Người dùng xem, tải và gửi được sách trong kho, kể cả qua OPDS. Tắt thì họ chỉ gửi được tệp tự tải lên và các bộ từ.') ?>
    </section>
    <section class="card">
      <?= sectionHead('Đăng nhập bằng Google') ?>
      <div class="setting"><div><b><?= googleConfig() ? 'Đang bật' : 'Chưa bật' ?></b>
        <p>Cấu hình ở <code>library-data/google.json</code>. Địa chỉ chuyển hướng trong Google Cloud: <code><?= h(googleRedirectUri()) ?></code></p></div>
        <span class="badge <?= googleConfig() ? 'g' : 'red' ?>"><?= googleConfig() ? 'Hoạt động' : 'Tắt' ?></span></div>
    </section>
    <section class="card">
      <?= sectionHead('Giới hạn') ?>
      <dl class="facts-grid">
        <div><dt>Dung lượng mỗi tài khoản</dt><dd><?= h(humanSize(USER_QUOTA)) ?></dd></div>
        <div><dt>Tệp lớn nhất khi tải lên</dt><dd><?= h(humanSize(USER_FILE_MAX)) ?></dd></div>
        <div><dt>Tệp chờ tối đa mỗi máy</dt><dd><?= DEVICE_INBOX_MAX ?></dd></div>
        <div><dt>Khoá sai tối đa mỗi giờ (mỗi IP)</dt><dd>20</dd></div>
        <div><dt>Đăng ký tối đa mỗi giờ (mỗi IP)</dt><dd>5</dd></div>
        <div><dt>Cơ sở dữ liệu</dt><dd><?= h(humanSize($dbSize)) ?> · <?= num(scalar('SELECT COUNT(*) FROM events')) ?> mục lịch sử</dd></div>
      </dl>
    </section>
<?php });
exit;
