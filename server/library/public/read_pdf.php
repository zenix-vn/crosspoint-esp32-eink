<?php
// The PDF reader page, included by read.php (which has checked access and set
// $title, $src, $back and $key). PDF.js renders only the pages near the screen,
// and fetches the file in byte ranges, so a large scan opens without being
// downloaded first. Page, zoom and theme are kept in localStorage.

declare(strict_types=1);

if (!isset($src, $title, $back, $key)) exit;
?><!doctype html>
<html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="robots" content="noindex, nofollow">
<title><?= h($title) ?> · Đọc sách</title>
<style>
:root { --bg: #e9e6df; --bar: #f4f1ea; --text: #1d1c1a; --muted: #7a776f; --line: #e0dacd; --chip: #ebe6db; --page: #fff; }
:root[data-theme="dark"] { --bg: #0f0f0e; --bar: #1c1b19; --text: #ece8df; --muted: #8f8a80; --line: #34322d; --chip: #2a2926; --page: #fff; }
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text);
             font: 15px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
body { overflow: hidden; }
header { position: fixed; inset: 0 0 auto 0; height: 48px; display: flex; align-items: center; gap: 6px;
         padding: 0 10px; background: var(--bar); border-bottom: 1px solid var(--line); z-index: 5; }
header .title { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-weight: 600; }
button, header a, input { background: var(--chip); color: var(--text); border: 1px solid var(--line); border-radius: 8px;
                          height: 34px; min-width: 34px; padding: 0 10px; font: inherit; }
button, header a { cursor: pointer; text-decoration: none; display: inline-flex; align-items: center; justify-content: center; }
button:hover, header a:hover { border-color: var(--muted); }
.pageno { display: inline-flex; align-items: center; gap: 4px; color: var(--muted); white-space: nowrap; }
.pageno input { width: 58px; text-align: center; padding: 0 4px; }
#pages { position: fixed; top: 48px; bottom: 0; left: 0; right: 0; overflow: auto; padding: 12px 0 40px;
         -webkit-overflow-scrolling: touch; }
.page { position: relative; margin: 0 auto 12px; background: var(--page); box-shadow: 0 1px 4px rgba(0,0,0,.25); }
.page canvas { display: block; width: 100%; height: 100%; }
.page .num { position: absolute; left: 50%; top: 50%; transform: translate(-50%,-50%); color: #aaa; font-size: 13px; }
:root[data-theme="dark"] .page canvas { filter: invert(.88) hue-rotate(180deg); }
#msg { position: fixed; inset: 48px 0 0; display: flex; align-items: center; justify-content: center; text-align: center;
       padding: 24px; color: var(--muted); z-index: 4; pointer-events: none; }
@media (max-width: 560px) { .wide { display: none; } }
</style>
<script>
  try { document.documentElement.dataset.theme = localStorage.getItem('reader:pdftheme') || 'light'; } catch (e) {}
</script>
</head>
<body>
<header>
  <a href="<?= h($back) ?>" title="Quay lại">←</a>
  <div class="title"><?= h($title) ?></div>
  <span class="pageno"><input id="page" inputmode="numeric" value="1" aria-label="Trang"> <span id="total">/ …</span></span>
  <button id="btn-out" title="Thu nhỏ">−</button>
  <button id="btn-fit" class="wide" title="Vừa chiều ngang">↔</button>
  <button id="btn-in" title="Phóng to">+</button>
  <button id="btn-theme" title="Nền tối (đảo màu trang)">◐</button>
  <a class="wide" href="<?= h($src) ?>" title="Tải xuống">⤓</a>
</header>
<div id="pages"></div>
<div id="msg">Đang mở sách…</div>

<script src="https://cdn.jsdelivr.net/npm/pdfjs-dist@3.11.174/build/pdf.min.js"></script>
<script>
(() => {
  const SRC = <?= json_encode($src) ?>;
  const KEY = 'reader:' + <?= json_encode($key, JSON_UNESCAPED_UNICODE) ?>;
  const WORKER = 'https://cdn.jsdelivr.net/npm/pdfjs-dist@3.11.174/build/pdf.worker.min.js';
  const KEEP = 4;           // pages kept rendered on each side of the visible ones
  const MAX_PIXELS = 16e6;  // canvas size cap, for phones
  const store = {
    get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : v; } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) {} },
  };
  const $ = id => document.getElementById(id);
  const box = $('pages'), msg = $('msg'), pageInput = $('page');

  // A worker cannot start from another origin; a same-origin blob that imports the CDN script can.
  pdfjsLib.GlobalWorkerOptions.workerSrc = URL.createObjectURL(
    new Blob([`importScripts(${JSON.stringify(WORKER)});`], { type: 'application/javascript' }));

  let pdf, sizes = [], slots = [], rendered = new Map();
  // zoom: 0 = fit width, otherwise a CSS scale.
  let zoom = parseFloat(store.get(KEY + ':zoom', '0')) || 0;
  let current = parseInt(store.get(KEY + ':page', '1'), 10) || 1;

  pdfjsLib.getDocument({
    url: new URL(SRC, location.href).href, withCredentials: true,
    rangeChunkSize: 262144, disableAutoFetch: true, disableStream: true,
  }).promise.then(async doc => {
    pdf = doc;
    $('total').textContent = '/ ' + pdf.numPages;
    // Page sizes: the first page's for all, corrected as each page loads (most books share one size).
    const first = (await pdf.getPage(1)).getViewport({ scale: 1 });
    sizes = Array.from({ length: pdf.numPages }, () => ({ w: first.width, h: first.height }));
    for (let i = 1; i <= pdf.numPages; i++) {
      const d = document.createElement('div');
      d.className = 'page'; d.dataset.n = i;
      d.innerHTML = '<span class="num">' + i + '</span>';
      box.appendChild(d); slots.push(d);
    }
    layout();
    msg.remove();
    goTo(Math.min(current, pdf.numPages));
    box.addEventListener('scroll', onScroll, { passive: true });
    window.addEventListener('resize', () => { const p = current; layout(); goTo(p); });
    refresh();
  }).catch(e => {
    msg.textContent = 'Không mở được tệp PDF (' + (e && e.message || e) + '). Hãy thử tải xuống.';
  });

  function scaleFor(i) {
    const w = sizes[i].w;
    if (zoom) return zoom;
    return Math.min(box.clientWidth - 16, 1400) / w;   // fit width, with a margin
  }
  function layout() {
    slots.forEach((d, i) => {
      const s = scaleFor(i);
      d.style.width = Math.floor(sizes[i].w * s) + 'px';
      d.style.height = Math.floor(sizes[i].h * s) + 'px';
    });
    for (const [n, r] of rendered) { r.cancel && r.cancel(); }
    rendered.clear();
    slots.forEach(d => { const c = d.querySelector('canvas'); if (c) c.remove(); });
  }

  function visibleRange() {
    const top = box.scrollTop, bottom = top + box.clientHeight;
    let a = 0, b = slots.length - 1, mid = 1;
    for (let i = 0; i < slots.length; i++) {
      const d = slots[i];
      if (d.offsetTop + d.offsetHeight >= top) { a = i; break; }
    }
    for (let i = a; i < slots.length; i++) {
      if (slots[i].offsetTop > bottom) { b = i - 1; break; }
      b = i;
    }
    // The current page: the one crossing the middle of the screen.
    const centre = top + box.clientHeight / 3;
    mid = a;
    for (let i = a; i <= b; i++) if (slots[i].offsetTop <= centre) mid = i;
    return [a, b, mid];
  }

  let ticking = false;
  function onScroll() {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(() => { ticking = false; refresh(); });
  }
  function refresh() {
    if (!pdf) return;
    const [a, b, mid] = visibleRange();
    current = mid + 1;
    if (document.activeElement !== pageInput) pageInput.value = current;
    store.set(KEY + ':page', String(current));
    const lo = Math.max(0, a - KEEP), hi = Math.min(slots.length - 1, b + KEEP);
    for (const n of [...rendered.keys()]) {
      if (n < lo || n > hi) { const r = rendered.get(n); r.cancel && r.cancel(); rendered.delete(n);
                              const c = slots[n].querySelector('canvas'); if (c) c.remove(); }
    }
    for (let i = a; i <= b; i++) render(i);          // visible pages first
    for (let i = lo; i <= hi; i++) render(i);
  }

  function render(i) {
    if (rendered.has(i)) return;
    const job = { cancel: null };
    rendered.set(i, job);
    pdf.getPage(i + 1).then(page => {
      if (rendered.get(i) !== job) return;
      const base = page.getViewport({ scale: 1 });
      if (Math.abs(base.width - sizes[i].w) > 1 || Math.abs(base.height - sizes[i].h) > 1) {
        sizes[i] = { w: base.width, h: base.height };   // a page of its own size
        const s0 = scaleFor(i);
        slots[i].style.width = Math.floor(base.width * s0) + 'px';
        slots[i].style.height = Math.floor(base.height * s0) + 'px';
      }
      const css = scaleFor(i);
      let ratio = Math.min(window.devicePixelRatio || 1, 2);
      const px = base.width * base.height * css * css * ratio * ratio;
      if (px > MAX_PIXELS) ratio *= Math.sqrt(MAX_PIXELS / px);
      const vp = page.getViewport({ scale: css * ratio });
      const canvas = document.createElement('canvas');
      canvas.width = Math.floor(vp.width); canvas.height = Math.floor(vp.height);
      const task = page.render({ canvasContext: canvas.getContext('2d'), viewport: vp });
      job.cancel = () => task.cancel();
      return task.promise.then(() => {
        if (rendered.get(i) !== job) return;
        const old = slots[i].querySelector('canvas');
        if (old) old.remove();
        slots[i].appendChild(canvas);
      });
    }).catch(e => { if (!(e && e.name === 'RenderingCancelledException') && rendered.get(i) === job) rendered.delete(i); });
  }

  function goTo(n) {
    n = Math.max(1, Math.min(pdf.numPages, n | 0));
    box.scrollTop = slots[n - 1].offsetTop - 8;
    refresh();
  }
  pageInput.addEventListener('change', () => goTo(parseInt(pageInput.value, 10) || current));
  pageInput.addEventListener('keydown', e => { if (e.key === 'Enter') pageInput.blur(); });

  function setZoom(z) {
    const p = current;
    zoom = z;
    store.set(KEY + ':zoom', String(zoom));
    layout(); goTo(p);
  }
  const effective = () => zoom || scaleFor(current - 1);
  $('btn-in').onclick = () => setZoom(Math.min(5, effective() * 1.2));
  $('btn-out').onclick = () => setZoom(Math.max(0.2, effective() / 1.2));
  $('btn-fit').onclick = () => setZoom(0);
  $('btn-theme').onclick = () => {
    const t = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    document.documentElement.dataset.theme = t;
    store.set('reader:pdftheme', t);
  };
  document.addEventListener('keydown', e => {
    if (e.target === pageInput || !pdf) return;
    if (e.key === 'ArrowRight' || e.key === 'PageDown') { e.preventDefault(); goTo(current + 1); }
    if (e.key === 'ArrowLeft' || e.key === 'PageUp') { e.preventDefault(); goTo(current - 1); }
    if ((e.ctrlKey || e.metaKey) && (e.key === '+' || e.key === '=')) { e.preventDefault(); $('btn-in').click(); }
    if ((e.ctrlKey || e.metaKey) && e.key === '-') { e.preventDefault(); $('btn-out').click(); }
  });
})();
</script>
</body></html>
