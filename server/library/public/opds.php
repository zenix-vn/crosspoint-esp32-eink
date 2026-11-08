<?php
// OPDS 1.2 catalog of the library for the X4 Pro (Basic credentials).
// Only formats the reader opens are listed: EPUB and TXT.
//
//   opds.php                     start: categories, new, favourites, want to read
//   opds.php?c=<cat>             a category: its groups, or its books
//   opds.php?c=<cat>&s=<group>   books of a group
//   opds.php?f=new|fav|want|reading|en
//   opds.php?q=<terms>           search

declare(strict_types=1);

require_once __DIR__ . '/../../library-app/lib.php';

requireLibraryAccess();

const OPDS_PAGE = 30;
// Books flagged as under copyright are left out for every account, the admin's too.
const READABLE = "format IN ('epub', 'txt') AND rights <> 'copyright'";
$self = 'opds.php';

function x(?string $s): string {
    return htmlspecialchars((string)$s, ENT_XML1 | ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function opdsLink(array $params): string {
    return 'opds.php' . ($params ? '?' . http_build_query($params) : '');
}

function feedStart(string $id, string $title, bool $acquisition, array $params, ?int $nextPage = null): void {
    header('Content-Type: application/atom+xml;profile=opds-catalog;kind=' . ($acquisition ? 'acquisition' : 'navigation') . ';charset=utf-8');
    echo '<?xml version="1.0" encoding="UTF-8"?>' . "\n";
    echo '<feed xmlns="http://www.w3.org/2005/Atom" xmlns:opds="http://opds-spec.org/2010/catalog" xmlns:dc="http://purl.org/dc/terms/">' . "\n";
    echo '<id>urn:crosspoint:library:' . x($id) . '</id><title>' . x($title) . '</title><updated>' . date(DATE_ATOM) . '</updated>' . "\n";
    echo '<link rel="start" href="opds.php" type="application/atom+xml;profile=opds-catalog;kind=navigation"/>' . "\n";
    echo '<link rel="search" href="opds.php?q={searchTerms}" type="application/atom+xml;profile=opds-catalog;kind=acquisition"/>' . "\n";
    if ($nextPage !== null) {
        echo '<link rel="next" href="' . x(opdsLink($params + ['n' => $nextPage])) . '" type="application/atom+xml;profile=opds-catalog;kind=acquisition"/>' . "\n";
    }
}

function navEntry(string $id, string $title, string $summary, array $params): void {
    echo '<entry><id>urn:crosspoint:nav:' . x($id) . '</id><title>' . x($title) . '</title><updated>' . date(DATE_ATOM) . '</updated>';
    if ($summary !== '') echo '<content type="text">' . x($summary) . '</content>';
    echo '<link rel="subsection" href="' . x(opdsLink($params)) . '" type="application/atom+xml;profile=opds-catalog;kind=acquisition"/></entry>' . "\n";
}

function bookEntries(string $where, array $args, string $order, int $page): bool {
    $st = db()->prepare("SELECT * FROM books WHERE " . READABLE . ($where ? " AND $where" : '') . " ORDER BY $order LIMIT " . (OPDS_PAGE + 1) . ' OFFSET ' . (($page - 1) * OPDS_PAGE));
    $st->execute($args);
    $rows = $st->fetchAll();
    foreach (array_slice($rows, 0, OPDS_PAGE) as $b) {
        $type = $b['format'] === 'epub' ? 'application/epub+zip' : 'text/plain';
        echo '<entry><id>urn:crosspoint:book:' . (int)$b['id'] . '</id><title>' . x($b['title']) . '</title>';
        echo '<updated>' . date(DATE_ATOM, (int)$b['mtime']) . '</updated>';
        if ($b['author'] !== '') echo '<author><name>' . x($b['author']) . '</name></author>';
        if ($b['language'] !== '') echo '<dc:language>' . x($b['language']) . '</dc:language>';
        $summary = trim(($b['category'] ? $b['category'] . ($b['subcategory'] ? ' › ' . $b['subcategory'] : '') . '. ' : '') . mb_substr($b['description'], 0, 600));
        if ($summary !== '') echo '<summary>' . x($summary) . '</summary>';
        if ($b['cover']) {
            echo '<link rel="http://opds-spec.org/image" href="' . x(coverUrl((int)$b['id'])) . '" type="image/jpeg"/>';
            echo '<link rel="http://opds-spec.org/image/thumbnail" href="' . x(coverUrl((int)$b['id'])) . '" type="image/jpeg"/>';
        }
        echo '<link rel="http://opds-spec.org/acquisition" href="file.php?id=' . (int)$b['id'] . '" type="' . $type . '"/>';
        echo "</entry>\n";
    }
    return count($rows) > OPDS_PAGE;
}

$c = (string)($_GET['c'] ?? '');
$s = (string)($_GET['s'] ?? '');
$f = (string)($_GET['f'] ?? '');
$q = trim((string)($_GET['q'] ?? ''));
$n = max(1, (int)($_GET['n'] ?? 1));

// Search.
if ($q !== '') {
    $where = [];
    $args = [];
    foreach (preg_split('/\s+/', fold($q)) as $word) { $where[] = 'search LIKE ?'; $args[] = "%$word%"; }
    ob_start();
    $more = bookEntries(implode(' AND ', $where), $args, 'title COLLATE NOCASE', $n);
    $body = ob_get_clean();
    feedStart('search', 'Tìm: ' . $q, true, ['q' => $q], $more ? $n + 1 : null);
    echo $body . '</feed>';
    exit;
}

// Filtered lists.
$filters = ['new' => ['Mới thêm', '', 'added DESC'], 'fav' => ['Yêu thích', 'favorite = 1', 'title COLLATE NOCASE'],
            'want' => ['Muốn đọc', "status = 'want'", 'title COLLATE NOCASE'],
            'reading' => ['Đang đọc', "status = 'reading'", 'title COLLATE NOCASE'],
            'en' => ['Sách tiếng Anh', "language = 'en'", 'title COLLATE NOCASE']];
if (isset($filters[$f])) {
    [$title, $where, $order] = $filters[$f];
    ob_start();
    $more = bookEntries($where, [], $order, $n);
    $body = ob_get_clean();
    feedStart('f:' . $f, $title, true, ['f' => $f], $more ? $n + 1 : null);
    echo $body . '</feed>';
    exit;
}

// A category: its groups when it has several, else its books.
if ($c !== '') {
    if ($s === '') {
        $st = db()->prepare("SELECT subcategory, COUNT(*) AS n FROM books WHERE " . READABLE . " AND category = ? GROUP BY subcategory ORDER BY subcategory");
        $st->execute([$c]);
        $groups = $st->fetchAll();
        if (count($groups) > 1) {
            feedStart('c:' . $c, $c, false, []);
            foreach ($groups as $g) {
                $label = $g['subcategory'] === '' ? 'Sách khác' : $g['subcategory'];
                navEntry('c:' . $c . ':' . $g['subcategory'], $label, $g['n'] . ' cuốn',
                         ['c' => $c, 's' => $g['subcategory'] === '' ? '-' : $g['subcategory']]);
            }
            echo '</feed>';
            exit;
        }
    }
    $where = 'category = ?';
    $args = [$c];
    if ($s !== '') { $where .= ' AND subcategory = ?'; $args[] = $s === '-' ? '' : $s; }
    ob_start();
    $more = bookEntries($where, $args, 'title COLLATE NOCASE', $n);
    $body = ob_get_clean();
    feedStart('c:' . $c . ':' . $s, $s !== '' && $s !== '-' ? $s : $c, true, array_filter(['c' => $c, 's' => $s]), $more ? $n + 1 : null);
    echo $body . '</feed>';
    exit;
}

// Start page.
feedStart('root', 'Thư viện CrossPoint', false, []);
foreach (['reading' => 'Đang đọc', 'want' => 'Muốn đọc', 'fav' => 'Yêu thích', 'new' => 'Mới thêm', 'en' => 'Sách tiếng Anh'] as $key => $label) {
    navEntry('f:' . $key, $label, '', ['f' => $key]);
}
foreach (db()->query("SELECT category, COUNT(*) AS n FROM books WHERE " . READABLE . " GROUP BY category ORDER BY category") as $cat) {
    navEntry('c:' . $cat['category'], $cat['category'] ?: 'Chưa phân loại', $cat['n'] . ' cuốn', ['c' => $cat['category']]);
}
echo '</feed>';
