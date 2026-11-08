<?php
// Feedback page (?p=feedback): a signed-in user reports an interface bug, a
// broken feature or an idea, with an optional screenshot, and follows the
// admin's answer. Included by public/index.php; $me is the signed-in user.

declare(strict_types=1);

layout('Góp ý & báo lỗi', 'feedback', function () use ($me) {
    $uid = (int)$me['id'];
    $st = db()->prepare('SELECT * FROM feedback WHERE user_id = ? ORDER BY created DESC LIMIT 100');
    $st->execute([$uid]);
    $mine = $st->fetchAll();
    $devs = devicesOf($uid);
    ?>
    <div class="page-head"><div><h1>Góp ý & báo lỗi</h1>
      <p class="lead">Gặp lỗi giao diện, tính năng chạy sai hoặc muốn đề xuất điều gì? Hãy mô tả ở dưới, càng cụ thể càng tốt. Quản trị viên sẽ xem và trả lời tại đây.</p></div></div>

    <section class="card">
      <form method="post" enctype="multipart/form-data" class="stack">
        <input type="hidden" name="csrf" value="<?= h(csrfToken()) ?>">
        <input type="hidden" name="action" value="feedback">
        <input type="hidden" name="back" value="?p=feedback">
        <label>Loại góp ý
          <select name="kind">
            <?php foreach (FEEDBACK_KINDS as $k => $l): ?><option value="<?= h($k) ?>"><?= h($l) ?></option><?php endforeach ?>
          </select>
        </label>
        <label>Tiêu đề <input name="title" maxlength="120" required placeholder="Ví dụ: Chữ bị cắt ở màn hình Từ vựng"></label>
        <label>Màn hình hoặc tính năng liên quan <input name="screen" maxlength="80" placeholder="Ví dụ: Trang chủ, Đọc sách, Từ điển"></label>
        <label>Mô tả <textarea name="body" rows="6" maxlength="4000" required placeholder="Bạn đã làm gì, điều gì xảy ra, và bạn mong đợi điều gì?"></textarea></label>
        <?php if ($devs): ?>
          <label>Máy đang dùng
            <select name="device">
              <option value="">Không chọn</option>
              <?php foreach ($devs as $d): ?><option value="<?= h($d['id']) ?>"><?= h(deviceLabel($d)) ?> · <?= h($d['model'] ?: '?') ?> · <?= h($d['firmware'] ?: '?') ?></option><?php endforeach ?>
            </select>
          </label>
        <?php else: ?>
          <label>Kiểu máy và phiên bản firmware <input name="model_text" maxlength="60" placeholder="Ví dụ: X3, 2.0.10"></label>
        <?php endif ?>
        <label>Ảnh chụp màn hình (không bắt buộc, tối đa 3 MB) <input type="file" name="image" accept="image/jpeg,image/png,image/webp,image/gif"></label>
        <div><button class="primary">Gửi góp ý</button></div>
      </form>
    </section>

    <section class="card flush">
      <?= sectionHead('Góp ý của bạn (' . count($mine) . ')') ?>
      <?php if (!$mine): ?><p class="empty-note pad">Bạn chưa gửi góp ý nào.</p><?php else: ?>
      <ul class="stack pad">
        <?php foreach ($mine as $f): ?>
          <li>
            <p><b><?= h($f['title']) ?></b> <span class="badge"><?= h(FEEDBACK_STATUS[$f['status']] ?? $f['status']) ?></span>
              <small class="muted"> · <?= h(FEEDBACK_KINDS[$f['kind']] ?? $f['kind']) ?> · <?= date('d/m/Y H:i', (int)$f['created']) ?></small></p>
            <p style="white-space:pre-wrap"><?= h($f['body']) ?></p>
            <?php if ($f['image'] !== ''): ?><p><a href="?p=feedbackimg&amp;id=<?= (int)$f['id'] ?>" target="_blank" rel="noopener">Xem ảnh đính kèm</a></p><?php endif ?>
            <?php if ($f['reply'] !== ''): ?><div class="callout"><b>Trả lời của quản trị viên:</b><br><span style="white-space:pre-wrap"><?= h($f['reply']) ?></span></div><?php endif ?>
            <?php if ($f['status'] === 'new'): ?><?= postButton('feedbackdelete', ['id' => $f['id']], 'Rút lại', 'small', 'Rút lại góp ý này?') ?><?php endif ?>
          </li>
        <?php endforeach ?>
      </ul>
      <?php endif ?>
    </section>
    <?php
});
exit;
