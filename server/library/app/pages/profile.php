<?php
// The signed-in user's profile (?p=profile): who they are, what they have,
// sign-in and security, their readers, their history, and closing the account.
// Included by public/index.php; $me is the signed-in user.

declare(strict_types=1);

layout('Hồ sơ', 'profile', function () use ($me) {
    $uid = (int)$me['id'];
    $devs = devicesOf($uid);
    $files = userFiles($uid);
    $used = userUsage($uid);
    $decks = count(array_filter($files, fn($f) => $f['kind'] === 'deck'));
    $sends = scalar("SELECT COUNT(*) FROM events WHERE user_id = ? AND action = 'send'", [$uid]);
    $received = scalar("SELECT COUNT(*) FROM events WHERE user_id = ? AND action = 'receive'", [$uid]);
    $waiting = array_sum(array_map(fn($d) => count(inboxWaiting($d['id'])), $devs));
    $hidden = fn(string $action) => '<input type="hidden" name="csrf" value="' . h(csrfToken()) . '"><input type="hidden" name="action" value="' . $action . '"><input type="hidden" name="back" value="?p=profile">';
    $filter = (string)($_GET['h'] ?? '');
    $historyActions = ['send' => ['send', 'unsend'], 'receive' => ['receive'], 'device' => ['device_link', 'device_unlink', 'device_rename', 'key_create', 'key_revoke'],
                       'account' => ['register', 'login', 'login_google', 'password', 'profile', 'google_link', 'google_unlink'], 'files' => ['upload', 'delete_file']];
    [$where, $args] = eventFilter(['u' => $uid, 'a' => $historyActions[$filter] ?? []]);
    $total = countEvents($where, $args);
    $n = max(1, (int)($_GET['n'] ?? 1));
    $perPage = 20;
    ?>
    <div class="profile-head card">
      <?= avatar($me, 'xl') ?>
      <div class="grow">
        <h1><?= h(userLabel($me)) ?> <?= isAdmin($me) ? '<a class="badge dark" href="?p=admin">Quản trị</a>' : '' ?></h1>
        <p class="muted"><?= h($me['email'] ?: $me['login']) ?> · <?= signInMethods($me) ?></p>
        <p class="muted small">Thành viên từ <?= date('d/m/Y', (int)$me['created']) ?></p>
      </div>
      <div class="actions"><a class="button primary" href="?p=device"><?= $devs ? 'Quản lý máy đọc sách' : 'Liên kết máy đọc sách' ?></a></div>
    </div>

    <?php if (!$devs): ?>
      <div class="callout">
        <b>Bước tiếp theo: liên kết máy đọc sách.</b>
        Vào <a href="?p=device">Thiết bị</a>, bấm <b>Tạo khoá liên kết</b>, rồi trên máy vào <b>Thêm → Nhận sách</b> và nhập khoá. Sau đó bạn gửi được sách và bộ từ tới máy.
      </div>
    <?php endif ?>

    <div class="tiles">
      <?= statTile((string)count($devs), 'máy đọc sách', '?p=device', $waiting ? $waiting . ' tệp đang chờ máy nhận' : ($devs ? 'không có tệp chờ' : 'chưa liên kết')) ?>
      <?= statTile(num($sends), 'lượt gửi tới máy', '?p=profile&h=send#history', num($received) . ' tệp máy đã nhận') ?>
      <?= statTile((string)(count($files) - $decks), 'sách của tôi', '?p=mine', humanSize($used) . ' / ' . humanSize(USER_QUOTA)) ?>
      <?= statTile((string)$decks, 'bộ từ của tôi', '?p=vocab') ?>
    </div>

    <div class="cols two">
      <div class="stack">
        <section class="card">
          <?= sectionHead('Thông tin cá nhân') ?>
          <form method="post" class="form"><?= $hidden('profile') ?>
            <label>Tên hiển thị <input name="name" value="<?= h($me['name']) ?>" maxlength="60" required autocomplete="name"></label>
            <label>Email <input value="<?= h($me['email'] ?: $me['login']) ?>" disabled><small>Email dùng để đăng nhập và làm tên đăng nhập OPDS; không đổi được.</small></label>
            <button>Lưu thay đổi</button>
          </form>
        </section>
        <section class="card">
          <?= sectionHead($me['hash'] === '' ? 'Đặt mật khẩu' : 'Đổi mật khẩu') ?>
          <?php if ($me['hash'] === ''): ?><p class="muted">Bạn đang đăng nhập bằng Google. Đặt mật khẩu để đăng nhập bằng email và dùng OPDS trên máy.</p><?php endif ?>
          <form method="post" class="form"><?= $hidden('password') ?>
            <?php if ($me['hash'] !== ''): ?><label>Mật khẩu hiện tại <input name="current" type="password" autocomplete="current-password" required></label><?php endif ?>
            <label>Mật khẩu mới <input name="new" type="password" autocomplete="new-password" minlength="8" required><small>Ít nhất 8 ký tự.</small></label>
            <label>Nhập lại mật khẩu mới <input name="confirm" type="password" autocomplete="new-password" minlength="8" required></label>
            <button class="primary">Lưu mật khẩu</button>
          </form>
        </section>
        <?php if (googleConfig()): ?>
          <section class="card">
            <?= sectionHead('Đăng nhập bằng Google') ?>
            <?php if ($me['google_sub'] !== null): ?>
              <div class="setting"><div><b>Đã liên kết</b><p>Bạn có thể đăng nhập bằng tài khoản Google.</p></div>
                <?= $me['hash'] !== '' ? postButton('unlinkgoogle', [], 'Bỏ liên kết', 'small', 'Bỏ liên kết Google? Bạn vẫn đăng nhập được bằng email và mật khẩu.')
                                       : '<span class="muted small">Đặt mật khẩu trước khi bỏ liên kết</span>' ?></div>
            <?php else: ?>
              <div class="setting"><div><b>Chưa liên kết</b><p>Liên kết để đăng nhập nhanh bằng Google.</p></div>
                <a class="button google" href="?p=google&amp;start=1&amp;link=1"><?= googleMark() ?> Liên kết Google</a></div>
            <?php endif ?>
          </section>
        <?php endif ?>
      </div>

      <div class="stack">
        <section class="card">
          <?= sectionHead('Máy đọc sách', 'Quản lý', '?p=device') ?>
          <?php if (!$devs): ?><p class="empty-note">Chưa có máy nào. <a href="?p=device">Tạo khoá liên kết</a>.</p><?php endif ?>
          <ul class="devlist">
            <?php foreach ($devs as $d): $w = count(inboxWaiting($d['id'])); ?>
              <li><div><b><?= h(deviceLabel($d)) ?></b><small><?= h($d['model'] ?: '?') ?> · kết nối <?= h(ago((int)$d['last_seen'])) ?><?= (int)$d['battery'] >= 0 ? ' · pin ' . (int)$d['battery'] . '%' : '' ?></small></div>
                <?= $w ? '<a class="badge" href="?p=device">' . $w . ' tệp chờ</a>' : '' ?></li>
            <?php endforeach ?>
          </ul>
          <p class="muted small">Khoá liên kết: <?= $me['link_key'] ? '<code>' . h(formatLinkKey($me['link_key'])) . '</code>' : 'chưa tạo' ?> · <a href="?p=device">đổi khoá</a></p>
        </section>
        <?php if (canSeeLibrary($me)): ?>
          <section class="card">
            <?= sectionHead('Đọc thư viện trên máy (OPDS)') ?>
            <dl class="facts-grid one">
              <div><dt>Địa chỉ</dt><dd><code>https://<?= h($_SERVER['HTTP_HOST'] ?? 'crosspoint.zenix.vn') ?>/library/opds.php</code></dd></div>
              <div><dt>Tên đăng nhập</dt><dd><code><?= h($me['email'] ?: $me['login']) ?></code></dd></div>
              <div><dt>Mật khẩu</dt><dd><?= $me['hash'] !== '' ? 'mật khẩu tài khoản này' : 'hãy đặt mật khẩu ở bên trái trước' ?></dd></div>
            </dl>
            <p class="muted small">Trên máy: <b>Cài đặt → Mạng → Máy chủ OPDS → Thêm máy chủ</b>.</p>
          </section>
        <?php endif ?>
      </div>
    </div>

    <section class="card" id="history">
      <?= sectionHead('Hoạt động của tôi') ?>
      <div class="chips">
        <?php foreach (['' => 'Tất cả', 'send' => 'Gửi', 'receive' => 'Máy đã nhận', 'files' => 'Tệp', 'device' => 'Thiết bị', 'account' => 'Tài khoản'] as $k => $l): ?>
          <a class="chip <?= $filter === $k ? 'on' : '' ?>" href="?p=profile<?= $k !== '' ? '&amp;h=' . $k : '' ?>#history"><?= h($l) ?></a>
        <?php endforeach ?>
      </div>
      <?= eventList(events($where, $args, $perPage, ($n - 1) * $perPage), false) ?>
      <?= pageLinks($n, max(1, (int)ceil($total / $perPage))) ?>
    </section>

    <?php if (!isAdmin($me)): ?>
      <section class="card danger-zone">
        <?= sectionHead('Xoá tài khoản') ?>
        <p>Xoá hẳn tài khoản, các tệp bạn đã tải lên và gỡ mọi máy đọc sách khỏi tài khoản. Sách đã có trên máy vẫn còn. Không hoàn tác được.</p>
        <form method="post" class="form inline-form" data-confirm="Xoá hẳn tài khoản của bạn?"><?= $hidden('deleteself') ?>
          <label>Gõ email của bạn để xác nhận <input name="confirm_email" autocomplete="off" required placeholder="<?= h($me['email']) ?>"></label>
          <button class="danger">Xoá tài khoản</button>
        </form>
      </section>
    <?php endif ?>
<?php });
exit;
