<?php
// The book list: categories and groups, search, filters, sort, grid or table.

declare(strict_types=1);

const BOOK_FILTERS = ['' => 'Tất cả', 'fav' => '★ Yêu thích', 'want' => 'Muốn đọc', 'reading' => 'Đang đọc', 'read' => 'Đã đọc',
                      'vi' => 'Tiếng Việt', 'en' => 'Tiếng Anh', 'epub' => 'Đọc được trên X4'];
const BOOK_SORTS = ['title' => ['Tên', 'title COLLATE NOCASE'], 'author' => ['Tác giả', 'author COLLATE NOCASE, title'],
                    'new' => ['Mới thêm', 'added DESC, title'], 'size' => ['Dung lượng', 'size DESC']];

return ['title' => 'Sách', 'body' => function () {
    $c = (string)($_GET['c'] ?? '');
    $s = (string)($_GET['s'] ?? '');
    $q = trim((string)($_GET['q'] ?? ''));
    $f = (string)($_GET['f'] ?? '');
    $sort = isset(BOOK_SORTS[$_GET['sort'] ?? '']) ? (string)$_GET['sort'] : 'title';
    $view = ($_GET['v'] ?? '') === 'list' ? 'list' : 'grid';
    $n = max(1, (int)($_GET['n'] ?? 1));

    $where = [];
    $args = [];
    if ($c !== '') { $where[] = 'category = ?'; $args[] = $c; }
    if ($s !== '') { $where[] = 'subcategory = ?'; $args[] = $s; }
    if ($q !== '') { foreach (preg_split('/\s+/', fold($q)) as $word) { $where[] = 'search LIKE ?'; $args[] = "%$word%"; } }
    if ($f === 'fav') $where[] = 'favorite = 1';
    elseif (isset(STATUS_LABEL[$f])) { $where[] = 'status = ?'; $args[] = $f; }
    elseif ($f === 'en' || $f === 'vi') { $where[] = 'language = ?'; $args[] = $f; }
    elseif ($f === 'epub') { $where[] = "format IN ('epub', 'txt')"; }
    $sql = $where ? ' WHERE ' . implode(' AND ', $where) : '';

    $count = db()->prepare("SELECT COUNT(*) FROM books$sql");
    $count->execute($args);
    $total = (int)$count->fetchColumn();
    $pages = max(1, (int)ceil($total / PAGE_SIZE));
    $n = min($n, $pages);
    $st = db()->prepare("SELECT * FROM books$sql ORDER BY " . BOOK_SORTS[$sort][1] . ' LIMIT ' . PAGE_SIZE . ' OFFSET ' . (($n - 1) * PAGE_SIZE));
    $st->execute($args);
    $books = $st->fetchAll();

    $cats = db()->query('SELECT category, COUNT(*) AS n FROM books GROUP BY category ORDER BY category')->fetchAll();
    $subs = [];
    if ($c !== '') {
        $st2 = db()->prepare("SELECT subcategory, COUNT(*) AS n FROM books WHERE category = ? AND subcategory <> '' GROUP BY subcategory ORDER BY subcategory");
        $st2->execute([$c]);
        $subs = $st2->fetchAll();
    }
    $all = (int)db()->query('SELECT COUNT(*) FROM books')->fetchColumn();
    $title = $q !== '' ? 'Kết quả cho “' . $q . '”' : ($s !== '' ? $s : ($c !== '' ? ($c ?: 'Chưa phân loại') : 'Tất cả sách'));
    ?>
    <div class="library">
      <aside class="cats" aria-label="Thể loại">
        <h3>Thể loại</h3>
        <a class="cat <?= $c === '' && $s === '' ? 'on' : '' ?>" href="<?= h(query(['c' => null, 's' => null, 'n' => null])) ?>">Tất cả <span><?= num($all) ?></span></a>
        <?php foreach ($cats as $cat): ?>
          <a class="cat <?= $c === $cat['category'] && $s === '' ? 'on' : '' ?>" href="<?= h(query(['c' => $cat['category'], 's' => null, 'n' => null])) ?>"><?= h($cat['category'] ?: 'Chưa phân loại') ?> <span><?= num((int)$cat['n']) ?></span></a>
          <?php if ($c === $cat['category']): foreach ($subs as $sub): ?>
            <a class="cat sub <?= $s === $sub['subcategory'] ? 'on' : '' ?>" href="<?= h(query(['s' => $sub['subcategory'], 'n' => null])) ?>"><?= h($sub['subcategory']) ?> <span><?= num((int)$sub['n']) ?></span></a>
          <?php endforeach; endif ?>
        <?php endforeach ?>
      </aside>
      <section>
        <div class="pagehead">
          <div>
            <?php if ($c !== ''): ?><p class="crumbs"><a href="<?= h(query(['c' => null, 's' => null, 'n' => null])) ?>">Sách</a> › <?= $s !== '' ? '<a href="' . h(query(['s' => null, 'n' => null])) . '">' . h($c) . '</a>' : h($c) ?></p><?php endif ?>
            <h1><?= h($title) ?></h1>
            <p class="sub"><?= num($total) ?> cuốn<?= $all === 0 ? ' — thư viện đang được lập chỉ mục, hãy quay lại sau ít phút.' : '' ?></p>
          </div>
          <div class="seg" role="group" aria-label="Kiểu xem">
            <a class="<?= $view === 'grid' ? 'on' : '' ?>" href="<?= h(query(['v' => null])) ?>" title="Lưới"><?= icon('grid') ?></a>
            <a class="<?= $view === 'list' ? 'on' : '' ?>" href="<?= h(query(['v' => 'list'])) ?>" title="Danh sách"><?= icon('list') ?></a>
          </div>
        </div>
        <div class="filters">
          <div class="chips">
            <?php foreach (BOOK_FILTERS as $k => $label): ?>
              <a class="chip <?= $f === $k ? 'on' : '' ?>" href="<?= h(query(['f' => $k, 'n' => null])) ?>"><?= h($label) ?></a>
            <?php endforeach ?>
          </div>
          <label class="sort">Sắp xếp
            <select onchange="location.href=this.value">
              <?php foreach (BOOK_SORTS as $k => [$label]): ?>
                <option value="<?= h(query(['sort' => $k === 'title' ? null : $k, 'n' => null])) ?>" <?= $sort === $k ? 'selected' : '' ?>><?= h($label) ?></option>
              <?php endforeach ?>
            </select>
          </label>
        </div>

        <?php if (!$books && $all > 0): ?>
          <div class="empty"><?= icon('search') ?><h2>Không có sách nào khớp</h2><p>Thử bỏ bớt bộ lọc hoặc tìm không dấu, ví dụ <i>tieng viet</i>.</p></div>
        <?php elseif ($view === 'grid'): ?>
          <div class="grid"><?php foreach ($books as $b) echo bookCard($b) ?></div>
        <?php else: ?>
          <div class="tablewrap"><table class="table">
            <thead><tr><th>Tên sách</th><th>Tác giả</th><th class="hide-s">Thể loại</th><th>Định dạng</th><th class="num hide-s">Dung lượng</th></tr></thead>
            <tbody><?php foreach ($books as $b): ?>
              <tr>
                <td><a href="?p=book&id=<?= (int)$b['id'] ?>"><b><?= h($b['title']) ?></b></a><?= $b['favorite'] ? ' <span class="star">★</span>' : '' ?>
                  <?php if ($b['status']): ?><span class="pill"><?= h(STATUS_LABEL[$b['status']] ?? '') ?></span><?php endif ?></td>
                <td><?= h($b['author']) ?></td>
                <td class="muted hide-s"><?= h($b['category']) ?><?= $b['subcategory'] ? ' › ' . h($b['subcategory']) : '' ?></td>
                <td><span class="fmt <?= readableOnReader($b) ? 'ok' : '' ?>"><?= h(strtoupper($b['format'])) ?></span></td>
                <td class="num muted hide-s"><?= h(humanSize((int)$b['size'])) ?></td>
              </tr>
            <?php endforeach ?></tbody>
          </table></div>
        <?php endif ?>
        <?= pager($n, $pages) ?>
      </section>
    </div>
<?php }];
