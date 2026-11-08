<?php
// Small pieces the admin and profile pages share: avatars, stat tiles, the
// activity chart, compact event lists. Included by public/index.php after its
// own helpers (layout, postButton, query, ago).

declare(strict_types=1);

function scalar(string $sql, array $args = []): int {
    $st = db()->prepare($sql);
    $st->execute($args);
    return (int)$st->fetchColumn();
}

function num(int $n): string {
    return number_format($n, 0, ',', '.');
}

// A round initial in a colour derived from the name.
function avatar(array $u, string $size = ''): string {
    $label = userLabel($u);
    $hue = crc32((string)($u['email'] ?? $u['login'] ?? $u['id'])) % 360;
    return '<span class="avatar ' . h($size) . '" style="--h:' . $hue . '" aria-hidden="true">' .
           h(mb_strtoupper(mb_substr($label, 0, 1))) . '</span>';
}

// A number with a label, optionally a link and a line of context.
function statTile(string $value, string $label, string $href = '', string $context = '', string $tone = ''): string {
    $inner = '<b>' . h($value) . '</b><span>' . h($label) . '</span>' . ($context !== '' ? '<small class="' . h($tone) . '">' . $context . '</small>' : '');
    return $href !== '' ? '<a class="tile" href="' . h($href) . '">' . $inner . '<i class="go" aria-hidden="true">→</i></a>'
                        : '<div class="tile">' . $inner . '</div>';
}

// "+3 so với 7 ngày trước" style comparison.
function delta(int $now, int $before, string $period): string {
    if ($now === $before) return 'bằng ' . $period . ' trước';
    $diff = $now - $before;
    return '<em class="' . ($diff > 0 ? 'up' : 'down') . '">' . ($diff > 0 ? '+' : '−') . abs($diff) . '</em> so với ' . $period . ' trước';
}

// Per-day counts of some event actions over the last $days days (Vietnam time).
function dailyCounts(array $actions, int $days, ?int $uid = null): array {
    $start = strtotime('today -' . ($days - 1) . ' days');
    $sql = 'SELECT action, time FROM events WHERE time >= ? AND action IN (' . implode(',', array_fill(0, count($actions), '?')) . ')';
    $args = array_merge([$start], $actions);
    if ($uid !== null) { $sql .= ' AND user_id = ?'; $args[] = $uid; }
    $st = db()->prepare($sql);
    $st->execute($args);
    $out = [];
    for ($i = 0; $i < $days; $i++) $out[date('Y-m-d', strtotime("+$i days", $start))] = array_fill_keys($actions, 0);
    foreach ($st as $r) {
        $day = date('Y-m-d', (int)$r['time']);
        if (isset($out[$day])) $out[$day][$r['action']]++;
    }
    return $out;
}

// Grouped bars per day, one colour per series. $series: action => label.
function barChart(array $daily, array $series): string {
    $max = 1;
    foreach ($daily as $counts) foreach ($series as $k => $_) $max = max($max, $counts[$k]);
    $html = '<div class="chart" role="img" aria-label="Biểu đồ hoạt động theo ngày">';
    foreach ($daily as $day => $counts) {
        $title = date('d/m', strtotime($day)) . ': ' . implode(', ', array_map(fn($k) => $series[$k] . ' ' . $counts[$k], array_keys($series)));
        $html .= '<div class="day" title="' . h($title) . '"><div class="bars">';
        $i = 0;
        foreach ($series as $k => $_) {
            $html .= '<span class="s' . $i++ . '" style="height:' . round($counts[$k] / $max * 100, 1) . '%"></span>';
        }
        $html .= '</div><small>' . date('d', strtotime($day)) . '</small></div>';
    }
    $html .= '</div><div class="legend">';
    $i = 0;
    foreach ($series as $k => $label) {
        $total = array_sum(array_column($daily, $k));
        $html .= '<span><i class="s' . $i++ . '"></i>' . h($label) . ' <b>' . $total . '</b></span>';
    }
    return $html . '</div>';
}

// One line per event: who (optional), what, detail, when.
function eventList(array $events, bool $showUser, string $userHref = '?p=admin&t=user&u='): string {
    if (!$events) return '<p class="empty-note">Chưa có hoạt động nào.</p>';
    $html = '<ul class="feed">';
    foreach ($events as $e) {
        $who = '';
        if ($showUser) {
            $who = $e['user_id'] !== null
                ? '<a href="' . h($userHref . (int)$e['user_id']) . '">' . h($e['who'] ?: '#' . $e['user_id']) . '</a> '
                : '<span class="muted">Khách</span> ';
        }
        $device = $e['device_id'] ? ' · <span class="muted">' . h($e['device_name'] ?: strtoupper(substr($e['device_id'], -6))) . '</span>' : '';
        $html .= '<li><span class="dot a-' . h($e['action']) . '"></span><div>' . $who .
                 '<b>' . h(EVENT_LABEL[$e['action']] ?? $e['action']) . '</b>' .
                 ($e['detail'] !== '' ? ' <span class="what">' . h($e['detail']) . '</span>' : '') . $device .
                 '</div><time datetime="' . date('c', (int)$e['time']) . '" title="' . date('d/m/Y H:i', (int)$e['time']) . '">' . h(ago((int)$e['time'])) . '</time></li>';
    }
    return $html . '</ul>';
}

// Tabs as links; $active is the current key.
function tabs(array $items, string $active): string {
    $html = '<nav class="tabs" aria-label="Mục">';
    foreach ($items as $key => [$label, $href, $count]) {
        $html .= '<a href="' . h($href) . '"' . ($key === $active ? ' class="on" aria-current="page"' : '') . '>' . h($label) .
                 ($count !== null ? ' <span>' . h((string)$count) . '</span>' : '') . '</a>';
    }
    return $html . '</nav>';
}

function sectionHead(string $title, string $linkLabel = '', string $href = ''): string {
    return '<div class="section-head"><h2>' . h($title) . '</h2>' .
           ($href !== '' ? '<a href="' . h($href) . '">' . h($linkLabel) . ' →</a>' : '') . '</div>';
}

// Page links for $page of $pages, keeping the other query parameters.
function pageLinks(int $page, int $pages, string $param = 'n'): string {
    if ($pages <= 1) return '';
    $html = '<nav class="pager" aria-label="Trang">';
    $html .= $page > 1 ? '<a href="' . h(query([$param => $page - 1])) . '">‹ Trước</a>' : '<span class="off">‹ Trước</span>';
    $html .= '<span>Trang ' . $page . ' / ' . $pages . '</span>';
    $html .= $page < $pages ? '<a href="' . h(query([$param => $page + 1])) . '">Sau ›</a>' : '<span class="off">Sau ›</span>';
    return $html . '</nav>';
}

function signInMethods(array $u): string {
    $m = [];
    if ($u['hash'] !== '') $m[] = '<span class="badge">Mật khẩu</span>';
    if ($u['google_sub'] !== null) $m[] = '<span class="badge g">Google</span>';
    return implode(' ', $m);
}

// Events of the history filters: user, action, period in days, text.
function eventFilter(array $f): array {
    $where = [];
    $args = [];
    if (!empty($f['u'])) { $where[] = 'e.user_id = ?'; $args[] = (int)$f['u']; }
    if (!empty($f['a'])) {
        $actions = (array)$f['a'];
        $where[] = 'e.action IN (' . implode(',', array_fill(0, count($actions), '?')) . ')';
        array_push($args, ...$actions);
    }
    if (!empty($f['d'])) { $where[] = 'e.time >= ?'; $args[] = time() - (int)$f['d'] * 86400; }
    if (($f['q'] ?? '') !== '') { $where[] = "(e.detail LIKE ? OR e.ip LIKE ? OR e.device_id LIKE ?)"; array_push($args, "%{$f['q']}%", "%{$f['q']}%", "%{$f['q']}%"); }
    return [implode(' AND ', $where), $args];
}

function countEvents(string $where, array $args): int {
    $st = db()->prepare('SELECT COUNT(*) FROM events e' . ($where !== '' ? " WHERE $where" : ''));
    $st->execute($args);
    return (int)$st->fetchColumn();
}
