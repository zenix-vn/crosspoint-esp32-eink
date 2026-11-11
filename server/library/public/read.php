<?php
// Reads an EPUB (epub.js) or a PDF (PDF.js, see read_pdf.php) in the browser:
// a shared-collection book (?id=) or one of the user's own EPUB files (?f=). The file itself comes from file.php or
// ?p=getfile, which check access again; the reading position, font size and
// theme are kept in the browser (localStorage).

declare(strict_types=1);

require_once __DIR__ . '/../../library-app/lib.php';

startSession();
$me = currentUser();
if (!$me) {
    header('Location: index.php');
    exit;
}
releaseSession();

$title = '';
$src = '';
$format = '';
$key = '';
$back = 'index.php';
if (isset($_GET['id'])) {
    $book = canSeeLibrary($me) ? bookById((int)$_GET['id'], $me) : null;
    if ($book && in_array($book['format'], ['epub', 'pdf'], true)) {
        $format = $book['format'];
        $title = $book['title'] . ($book['author'] ? ' — ' . $book['author'] : '');
        $src = 'file.php?id=' . (int)$book['id'];
        $back = 'index.php?p=book&id=' . (int)$book['id'];
        $key = 'b' . (int)$book['id'];
    }
} elseif (isset($_GET['f'])) {
    $f = userFile((int)$me['id'], (string)$_GET['f']);
    if ($f && strtolower(pathinfo($f['name'], PATHINFO_EXTENSION)) === 'epub') {
        $format = 'epub';
        $title = pathinfo($f['name'], PATHINFO_FILENAME);
        $src = 'index.php?p=getfile&f=' . rawurlencode($f['name']);
        $back = 'index.php?p=mine';
        $key = 'u' . (int)$me['id'] . ':' . $f['name'];
    }
}
if ($src === '') {
    http_response_code(404);
    exit('Không tìm thấy sách EPUB hoặc PDF.');
}
if ($format === 'pdf') {
    require __DIR__ . '/read_pdf.php';
    exit;
}
?><!doctype html>
<html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="robots" content="noindex, nofollow">
<title><?= h($title) ?> · Đọc sách</title>
<style>
:root { --bg: #fbfaf6; --bar: #f4f1ea; --text: #1d1c1a; --muted: #7a776f; --line: #e0dacd; --chip: #ebe6db; }
:root[data-theme="sepia"] { --bg: #f4ecd8; --bar: #ebe0c6; --text: #3b2f1e; --muted: #85745a; --line: #dccfb0; --chip: #e3d6b8; }
:root[data-theme="dark"] { --bg: #151513; --bar: #1c1b19; --text: #ece8df; --muted: #8f8a80; --line: #34322d; --chip: #2a2926; }
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text);
             font: 15px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; overflow: hidden; }
header { position: fixed; inset: 0 0 auto 0; height: 48px; display: flex; align-items: center; gap: 6px;
         padding: 0 10px; background: var(--bar); border-bottom: 1px solid var(--line); z-index: 5; }
header .title { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-weight: 600; }
button, header a { background: var(--chip); color: var(--text); border: 1px solid var(--line); border-radius: 8px;
                   height: 34px; min-width: 34px; padding: 0 10px; font: inherit; cursor: pointer; text-decoration: none;
                   display: inline-flex; align-items: center; justify-content: center; }
button:hover, header a:hover { border-color: var(--muted); }
#viewer { position: fixed; top: 48px; bottom: 36px; left: 0; right: 0; }
footer { position: fixed; inset: auto 0 0 0; height: 36px; display: flex; align-items: center; gap: 10px; padding: 0 14px;
         background: var(--bar); border-top: 1px solid var(--line); color: var(--muted); font-size: 13px; z-index: 5; }
footer .chapter { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
footer input[type=range] { width: min(40vw, 260px); accent-color: var(--muted); }
.zone { position: fixed; top: 48px; bottom: 36px; width: 18%; z-index: 3; cursor: pointer; }
.zone.prev { left: 0; } .zone.next { right: 0; }
#toc { position: fixed; top: 48px; bottom: 0; left: 0; width: min(340px, 88vw); background: var(--bar);
       border-right: 1px solid var(--line); overflow: auto; padding: 8px 0 24px; z-index: 6; transform: translateX(-105%);
       transition: transform .2s; }
#toc.open { transform: none; }
#toc a { display: block; padding: 8px 16px; color: var(--text); text-decoration: none; }
#toc a:hover { background: var(--chip); }
#toc .sub a { padding-left: 32px; font-size: 14px; color: var(--muted); }
#toc .sub .sub a { padding-left: 48px; }
#msg { position: fixed; inset: 48px 0 36px; display: flex; align-items: center; justify-content: center; text-align: center;
       padding: 24px; color: var(--muted); z-index: 4; }
@media (max-width: 560px) { .wide { display: none; } }
</style>
<script>
  // Theme first, so the page does not flash light before the reader starts.
  try { document.documentElement.dataset.theme = localStorage.getItem('reader:theme') || 'light'; } catch (e) {}
</script>
</head>
<body>
<header>
  <a href="<?= h($back) ?>" title="Quay lại">←</a>
  <button id="btn-toc" title="Mục lục">☰</button>
  <div class="title"><?= h($title) ?></div>
  <button id="btn-smaller" title="Chữ nhỏ hơn">A−</button>
  <button id="btn-bigger" title="Chữ to hơn">A+</button>
  <button id="btn-theme" title="Đổi nền: sáng / sepia / tối">◐</button>
  <a class="wide" href="<?= h($src) ?>" title="Tải xuống">⤓</a>
</header>
<nav id="toc" aria-label="Mục lục"></nav>
<div id="viewer"></div>
<div class="zone prev" id="zone-prev" title="Trang trước"></div>
<div class="zone next" id="zone-next" title="Trang sau"></div>
<div id="msg">Đang tải sách…</div>
<footer><span class="chapter" id="chapter"></span><input type="range" id="seek" min="0" max="1000" value="0" disabled><span id="pct"></span></footer>

<script src="https://cdn.jsdelivr.net/npm/jszip@3.10.1/dist/jszip.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/epubjs@0.3.93/dist/epub.min.js"></script>
<script>
(() => {
  const SRC = <?= json_encode($src) ?>;
  const KEY = 'reader:' + <?= json_encode($key, JSON_UNESCAPED_UNICODE) ?>;
  const THEMES = {
    light: { body: { background: '#fbfaf6', color: '#1d1c1a' } },
    sepia: { body: { background: '#f4ecd8', color: '#3b2f1e' } },
    dark:  { body: { background: '#151513', color: '#ece8df' }, a: { color: '#8bb8e0' } },
  };
  const ORDER = ['light', 'sepia', 'dark'];
  const store = {
    get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : v; } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) {} },
  };
  const $ = id => document.getElementById(id);
  const msg = $('msg');
  let theme = store.get('reader:theme', 'light');
  let size = parseInt(store.get('reader:size', '100'), 10) || 100;

  fetch(SRC, { credentials: 'same-origin' })
    .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.arrayBuffer(); })
    .then(start)
    .catch(e => { msg.textContent = 'Không mở được sách (' + e.message + '). Hãy thử tải xuống.'; });

  function start(data) {
    const book = ePub(data);
    const rendition = book.renderTo('viewer', {
      width: '100%', height: '100%', flow: 'paginated', spread: 'auto', minSpreadWidth: 1000, allowScriptedContent: false,
    });
    for (const [name, css] of Object.entries(THEMES)) rendition.themes.register(name, css);
    rendition.themes.default({ body: { 'line-height': '1.6', padding: '0 4px !important' }, img: { 'max-width': '100%' } });
    rendition.themes.select(theme);
    rendition.themes.fontSize(size + '%');

    const saved = store.get(KEY, null);
    rendition.display(saved || undefined).catch(() => rendition.display()).then(() => { msg.remove(); });

    const next = () => rendition.next(), prev = () => rendition.prev();
    $('zone-next').onclick = next;
    $('zone-prev').onclick = prev;
    const onKey = e => {
      if (e.key === 'ArrowRight' || e.key === 'PageDown' || e.key === ' ') { e.preventDefault(); next(); }
      if (e.key === 'ArrowLeft' || e.key === 'PageUp') { e.preventDefault(); prev(); }
    };
    document.addEventListener('keydown', onKey);
    // Keys and swipes inside the book's own frame.
    rendition.hooks.content.register(contents => {
      const doc = contents.document;
      doc.addEventListener('keydown', onKey);
      let x0 = null, y0 = null;
      doc.addEventListener('touchstart', e => { x0 = e.touches[0].clientX; y0 = e.touches[0].clientY; }, { passive: true });
      doc.addEventListener('touchend', e => {
        if (x0 === null) return;
        const dx = e.changedTouches[0].clientX - x0, dy = e.changedTouches[0].clientY - y0;
        if (Math.abs(dx) > 50 && Math.abs(dx) > Math.abs(dy)) (dx < 0 ? next : prev)();
        x0 = null;
      }, { passive: true });
    });

    $('btn-bigger').onclick = () => setSize(size + 10);
    $('btn-smaller').onclick = () => setSize(size - 10);
    function setSize(s) {
      size = Math.max(60, Math.min(220, s));
      store.set('reader:size', String(size));
      rendition.themes.fontSize(size + '%');
    }
    $('btn-theme').onclick = () => {
      theme = ORDER[(ORDER.indexOf(theme) + 1) % ORDER.length];
      store.set('reader:theme', theme);
      document.documentElement.dataset.theme = theme;
      rendition.themes.select(theme);
    };

    // Table of contents.
    const toc = $('toc');
    $('btn-toc').onclick = () => toc.classList.toggle('open');
    const labels = {};
    book.loaded.navigation.then(nav => {
      const build = (items, parent) => {
        for (const it of items) {
          const wrap = document.createElement('div');
          const a = document.createElement('a');
          a.href = '#'; a.textContent = (it.label || '').trim();
          a.onclick = e => { e.preventDefault(); rendition.display(it.href); toc.classList.remove('open'); };
          labels[it.href.split('#')[0]] = a.textContent;
          wrap.appendChild(a);
          if (it.subitems && it.subitems.length) { const sub = document.createElement('div'); sub.className = 'sub'; build(it.subitems, sub); wrap.appendChild(sub); }
          parent.appendChild(wrap);
        }
      };
      build(nav.toc, toc);
      if (!nav.toc.length) toc.textContent = 'Sách không có mục lục.';
    });

    // Progress: locations are slow to compute for long books, so they are cached.
    const seek = $('seek'), pct = $('pct');
    book.ready.then(() => {
      const cached = store.get(KEY + ':loc', null);
      if (cached) { book.locations.load(cached); return; }
      return book.locations.generate(1600).then(() => store.set(KEY + ':loc', book.locations.save()));
    }).then(() => { seek.disabled = false; update(rendition.currentLocation()); });
    seek.oninput = () => rendition.display(book.locations.cfiFromPercentage(seek.value / 1000));

    function update(loc) {
      if (!loc || !loc.start) return;
      const href = (loc.start.href || '').split('#')[0];
      $('chapter').textContent = labels[href] || labels[Object.keys(labels).find(k => k.endsWith(href) || href.endsWith(k))] || '';
      if (book.locations.length()) {
        const p = book.locations.percentageFromCfi(loc.start.cfi);
        pct.textContent = Math.round(p * 100) + '%';
        seek.value = Math.round(p * 1000);
      }
    }
    rendition.on('relocated', loc => { store.set(KEY, loc.start.cfi); update(loc); });
  }
})();
</script>
</body></html>
