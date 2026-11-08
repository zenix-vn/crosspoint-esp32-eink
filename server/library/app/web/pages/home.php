<?php
// Overview: counts, readers waiting for approval, what is being read, what is new.

declare(strict_types=1);

return ['title' => 'Tổng quan', 'body' => function () {
    $pdo = db();
    $stats = $pdo->query("SELECT COUNT(*) AS books, SUM(format IN ('epub','txt')) AS readable, SUM(size) AS bytes,
                                 COUNT(DISTINCT category) AS cats, SUM(favorite) AS favs, SUM(status = 'read') AS done
                          FROM books")->fetch();
    $decks = listDecks();
    $words = array_sum(array_column($decks, 'words'));
    $devs = devices();
    $pending = array_values(array_filter($devs, fn($d) => !$d['approved']));
    $reading = $pdo->query("SELECT * FROM books WHERE status = 'reading' ORDER BY title LIMIT 12")->fetchAll();
    $want = $pdo->query("SELECT * FROM books WHERE status = 'want' ORDER BY title LIMIT 12")->fetchAll();
    $new = $pdo->query('SELECT * FROM books ORDER BY added DESC, id DESC LIMIT 12')->fetchAll();
    $cats = $pdo->query("SELECT category, COUNT(*) AS n FROM books GROUP BY category ORDER BY n DESC LIMIT 8")->fetchAll();
    $indexed = (int)(meta('indexed') ?? 0);

    pageHead('Tổng quan', 'Kho sách, bộ từ vựng và các máy đọc sách của bạn' .
        ($indexed ? ' · cập nhật chỉ mục ' . h(ago($indexed)) : ''));
    ?>
    <?php foreach ($pending as $d): ?>
      <div class="notice">
        <?= icon('device') ?>
        <div><b><?= h(deviceLabel($d)) ?></b> đang chờ duyệt (<?= h($d['model'] ?: 'không rõ máy') ?>, đăng ký <?= h(ago((int)$d['first_seen'])) ?>).</div>
        <?= postButton('approve', ['device' => $d['id']], 'Duyệt', 'primary small', '', 'check') ?>
      </div>
    <?php endforeach ?>

    <div class="stats">
      <a class="stat" href="?p=books"><span>Sách</span><b><?= num((int)$stats['books']) ?></b><small><?= humanSize((int)$stats['bytes']) ?> · <?= num((int)$stats['cats']) ?> thể loại</small></a>
      <a class="stat" href="?p=books&f=epub"><span>Đọc được trên X4</span><b><?= num((int)$stats['readable']) ?></b><small>EPUB và TXT</small></a>
      <a class="stat" href="?p=books&f=read"><span>Đã đọc</span><b><?= num((int)$stats['done']) ?></b><small><?= num((int)$stats['favs']) ?> cuốn yêu thích</small></a>
      <a class="stat" href="?p=vocab"><span>Từ vựng</span><b><?= num($words) ?></b><small><?= count($decks) ?> bộ từ</small></a>
      <a class="stat" href="?p=device"><span>Thiết bị</span><b><?= count($devs) - count($pending) ?></b><small><?= $pending ? count($pending) . ' máy chờ duyệt' : 'đã duyệt' ?></small></a>
    </div>

    <?php if ((int)$stats['books'] === 0): ?>
      <div class="empty"><?= icon('books') ?><h2>Thư viện đang trống</h2>
        <p>Chép sách vào <code>library-data/books/&lt;thể loại&gt;/&lt;nhóm&gt;/</code> trên máy chủ; chỉ mục tự cập nhật mỗi 10 phút.</p></div>
    <?php endif ?>

    <?php foreach ([['Đang đọc', $reading, '?p=books&f=reading'], ['Muốn đọc', $want, '?p=books&f=want'], ['Mới thêm', $new, '?p=books&sort=new']] as [$label, $rows, $more]):
        if (!$rows) continue; ?>
      <section class="shelf">
        <div class="shelf-head"><h2><?= h($label) ?></h2><a href="<?= h($more) ?>">Xem tất cả <?= icon('chevron') ?></a></div>
        <div class="row"><?php foreach ($rows as $b) echo bookCard($b) ?></div>
      </section>
    <?php endforeach ?>

    <?php if ($cats): ?>
      <section>
        <div class="shelf-head"><h2>Thể loại</h2><a href="?p=books">Duyệt thư viện <?= icon('chevron') ?></a></div>
        <div class="tiles">
          <?php foreach ($cats as $c): ?>
            <a class="tile" href="?p=books&c=<?= h(rawurlencode($c['category'])) ?>"><?= icon('folder') ?><b><?= h($c['category'] ?: 'Chưa phân loại') ?></b><small><?= num((int)$c['n']) ?> cuốn</small></a>
          <?php endforeach ?>
        </div>
      </section>
    <?php endif ?>
<?php }];
