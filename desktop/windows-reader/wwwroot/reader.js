'use strict';

const RTHEMES = {
  light: { name: 'Sáng', bg: '#ffffff', fg: '#1f2328', link: '#4343bf', sel: 'rgba(91,91,214,.25)', line: 'rgba(0,0,0,.12)' },
  sepia: { name: 'Giấy', bg: '#f6efe0', fg: '#4a3b2a', link: '#a0522d', sel: 'rgba(160,82,45,.22)', line: 'rgba(74,59,42,.18)' },
  slate: { name: 'Xám', bg: '#2b2f36', fg: '#d9dce2', link: '#a5b4fc', sel: 'rgba(165,180,252,.3)', line: 'rgba(255,255,255,.14)' },
  dark: { name: 'Tối', bg: '#121316', fg: '#c8c9cc', link: '#9ea7ff', sel: 'rgba(158,167,255,.28)', line: 'rgba(255,255,255,.12)' },
};
const FONTS = [
  { id: 'original', name: 'Gốc', css: '', show: '"Segoe UI", sans-serif' },
  // Every font here has the stacked Vietnamese letters (ằ, ố, ẫ…). Georgia
  // does not: Chromium splits those into letter + loose accent.
  { id: 'sitka', name: 'Sitka', css: '"Sitka Text", Cambria, serif' },
  { id: 'palatino', name: 'Palatino', css: '"Palatino Linotype", "Book Antiqua", Palatino, serif' },
  { id: 'cambria', name: 'Cambria', css: 'Cambria, "Times New Roman", serif' },
  { id: 'times', name: 'Times', css: '"Times New Roman", Times, serif' },
  { id: 'segoe', name: 'Segoe UI', css: '"Segoe UI", system-ui, sans-serif' },
];
const LINE_HEIGHTS = [[1.35, 'Hẹp'], [1.55, 'Vừa'], [1.75, 'Rộng'], [2.0, 'Thoáng']];
const MARGINS = { narrow: 24, medium: 60, wide: 120 };

function defaultPrefs() {
  return {
    theme: document.documentElement.dataset.theme === 'dark' ? 'dark' : 'light',
    font: 'original', fontSize: 110, lineHeight: 1.55, margin: 'medium', layout: 'spread', justify: true,
    turn: 'book', v: 2, lookup: true,
  };
}

const Reader = {
  isOpen: false,
  el: document.getElementById('reader'),
  book: null,
  rendition: null,
  rec: null,
  prefs: null,
  location: null,
  tocFlat: [],
  locationsReady: false,
  returnHash: '#/library',
  panel: null,
  search: { q: '', results: [], running: false, token: 0, done: 0 },

  // ---------- open / close ----------
  async open(id) {
    if (this.isOpen && this.rec && this.rec.id === id) return;
    if (this.isOpen) await this.close(false);
    if (openMenuClose) openMenuClose();
    modalStack.slice().forEach((c) => c());
    this.isOpen = true;
    this.returnHash = App.lastHash || '#/library';
    this.prefs = Object.assign(defaultPrefs(), App.state.reader || {});
    if (!FONTS.some((f) => f.id === this.prefs.font)) this.prefs.font = 'original';
    // v2: a book opens on two facing pages; settings saved before that move once.
    const saved = App.state.reader;
    if (saved && (saved.v || 0) < 2) {
      if (this.prefs.layout === 'paged') this.prefs.layout = 'spread';
      this.prefs.v = 2;
      api.post('/api/settings', { reader: this.prefs }).then((s) => { App.state = s; }).catch(() => {});
    }
    this.rec = null;
    this.buildDom();
    document.getElementById('app').hidden = true;
    this.el.hidden = false;
    try {
      this.rec = await api.get(`/api/library/${id}`);
      this.rec.bookmarks = this.rec.bookmarks || [];
      this.setHeader(this.rec.title, this.rec.chapter || this.rec.author || '');
      const buf = await (await fetch(`/api/library/${id}/file`)).arrayBuffer();
      if (!this.isOpen || this.rec.id !== id) return;
      this.book = ePub(buf);
      await this.book.ready;
      await this.createRendition(this.rec.cfi);
      $('#r-loading', this.el).hidden = true;
      api.post(`/api/library/${id}/state`, { opened: true }).catch(() => {});
      this.book.loaded.navigation.then((nav) => {
        this.flattenToc(nav.toc || []);
        this.updateChapter();
        if (this.panel === 'toc') this.paintPane();
      }).catch(() => {});
      this.loadLocations();
    } catch (e) {
      console.error(e);
      toast('Không mở được sách: ' + (e.message || e), { type: 'error' });
      this.close(true);
    }
  },

  async close(navigate = true) {
    if (!this.isOpen) return;
    clearTimeout(this.saveTimer);
    await this.save().catch(() => {});
    this.isOpen = false;
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    this.destroyGhost();
    try { this.rendition && this.rendition.destroy(); } catch (_) { /* already gone */ }
    try { this.book && this.book.destroy(); } catch (_) { /* already gone */ }
    this.book = null;
    this.rendition = null;
    this.location = null;
    this.locationsReady = false;
    this.tocFlat = [];
    this.panel = null;
    clearTimeout(this.snapTimer);
    if (this.curl) { cancelAnimationFrame(this.curl.raf); this.curl = null; }
    if (this.resizeObs) { this.resizeObs.disconnect(); this.resizeObs = null; }
    this.animating = false;
    if (this.snap) URL.revokeObjectURL(this.snap.url);
    this.snap = null;
    this.search = { q: '', results: [], running: false, token: this.search.token + 1, done: 0 };
    this.el.hidden = true;
    this.el.innerHTML = '';
    document.getElementById('app').hidden = false;
    await refreshBooks();
    if (navigate) location.hash = this.returnHash || '#/library';
  },

  // ---------- DOM ----------
  buildDom() {
    this.el.innerHTML = `
      <header class="r-top">
        <button class="r-btn" data-r="close" title="Về thư viện (Esc)">${icon('back', 20)}</button>
        <button class="r-btn" data-r="toc" title="Mục lục (T)">${icon('list', 20)}</button>
        <div class="r-title"><div class="t" id="r-t"></div><div class="c" id="r-c"></div></div>
        <button class="r-btn" data-r="search" title="Tìm trong sách (Ctrl+F)">${icon('search', 19)}</button>
        <button class="r-btn" data-r="bookmark" id="r-bm" title="Dấu trang (B)">${icon('bookmark', 19)}</button>
        <button class="r-btn" data-r="prefs" title="Hiển thị">${icon('type', 19)}</button>
        <span class="r-sep"></span>
        <button class="r-btn" data-r="fullscreen" id="r-fs" title="Toàn màn hình (F11)">${icon('maximize', 18)}</button>
      </header>
      <div class="r-stage" id="r-stage">
        <button class="r-nav prev" data-hot="-1" title="Trang trước — bấm hoặc kéo để lật">${icon('left', 26)}</button>
        <div class="r-viewer" id="r-viewer"></div>
        <button class="r-nav next" data-hot="1" title="Trang sau — bấm hoặc kéo để lật">${icon('right', 26)}</button>
        <div class="r-loading" id="r-loading"><span class="spinner"></span><span>Đang mở sách…</span></div>
      </div>
      <footer class="r-bottom">
        <span class="r-pct" id="r-pct">0%</span>
        <input type="range" class="r-slider" id="r-slider" min="0" max="1000" value="0" aria-label="Vị trí trong sách">
        <span class="r-page" id="r-page"></span>
      </footer>
      <div id="r-overlay"></div>`;
    this.applyChrome();

    // With the menus hidden, moving the mouse to the top or bottom edge brings them back.
    this.el.onmousemove = (e) => {
      if (!this.el.classList.contains('immersive')) return;
      this.el.classList.toggle('peek-top', e.clientY < 72);
      this.el.classList.toggle('peek-bottom', e.clientY > innerHeight - 68);
    };
    this.el.onmouseleave = () => this.el.classList.remove('peek-top', 'peek-bottom');

    this.el.onclick = (e) => {
      const b = e.target.closest('[data-r]');
      if (!b) return;
      const act = b.dataset.r;
      if (act === 'close') this.close(true);
      if (act === 'toc') this.togglePanel('toc');
      if (act === 'search') this.togglePanel('search');
      if (act === 'prefs') this.togglePanel('prefs');
      if (act === 'bookmark') this.toggleBookmark();
      if (act === 'fullscreen') this.toggleFullscreen();
      if (act === 'prev') this.prev();
      if (act === 'next') this.next();
    };
    const stage = $('#r-stage', this.el);
    stage.addEventListener('click', (e) => {
      if (e.target.closest('[data-r]') || e.target.closest('[data-hot]') || e.target.closest('iframe')) return;
      if (this.closePanels()) return;
      this.tapAt(e.clientX);
    });
    stage.addEventListener('wheel', (e) => this.onWheel(e), { passive: true });
    this.el.querySelectorAll('[data-hot]').forEach((hot) => {
      const dir = +hot.dataset.hot;
      hot.addEventListener('pointerdown', (e) => this.hotDown(e, dir));
      hot.addEventListener('pointerenter', (e) => this.hotPeek(dir, true, e.clientY));
      hot.addEventListener('pointerleave', () => this.hotPeek(dir, false));
    });
    this.resizeObs = new ResizeObserver(() => this.onViewerResize());
    this.resizeObs.observe($('#r-viewer', this.el));

    const slider = $('#r-slider', this.el);
    slider.addEventListener('input', () => {
      const v = slider.value / 1000;
      slider.style.setProperty('--p', (v * 100) + '%');
      this.showTip(slider, v);
    });
    slider.addEventListener('change', () => { this.hideTip(); this.goTo(slider.value / 1000); });
    slider.addEventListener('pointerup', () => this.hideTip());
  },

  applyChrome() {
    const p = this.prefs;
    this.el.dataset.rtheme = p.theme;
    this.el.dataset.layout = p.layout;
    const v = $('#r-viewer', this.el);
    if (!v) return;
    const pad = MARGINS[p.margin] ?? 60;
    v.style.maxWidth = p.layout === 'spread' ? '1560px' : (740 + pad * 2) + 'px';
    v.style.padding = p.layout === 'scroll' ? `0 ${pad}px` : `26px ${pad}px 14px`;
  },

  setHeader(title, sub) {
    const t = $('#r-t', this.el), c = $('#r-c', this.el);
    if (t) t.textContent = title || '';
    if (c) c.textContent = sub || '';
  },

  // ---------- rendering ----------
  async createRendition(target) {
    const viewer = $('#r-viewer', this.el);
    this.destroyGhost();
    if (this.rendition) { try { this.rendition.destroy(); } catch (_) { /* replaced */ } }
    viewer.innerHTML = '';
    this.applyChrome();
    const p = this.prefs;
    const scroll = p.layout === 'scroll';
    const r = this.book.renderTo(viewer, {
      width: '100%',
      height: '100%',
      flow: scroll ? 'scrolled' : 'paginated',
      manager: scroll ? 'continuous' : 'default',
      spread: p.layout === 'spread' ? 'auto' : 'none',
      minSpreadWidth: 760,
      allowScriptedContent: false,
    });
    this.rendition = r;
    r.themes.fontSize(p.fontSize + '%');
    r.hooks.content.register((contents) => this.onContent(contents));
    r.on('relocated', (loc) => this.onRelocated(loc));
    r.on('keydown', (e) => this.onKey(e));
    r.on('mousemove', () => this.el.classList.remove('peek-top', 'peek-bottom'));
    r.on('click', (e) => this.onFrameClick(e));
    r.on('selected', (cfi, contents) => this.onSelected(contents));
    try {
      await r.display(target || undefined);
    } catch (_) {
      await r.display();
    }
  },

  onContent(contents) {
    this.injectStyle(contents);
    const doc = contents.document;
    doc.addEventListener('wheel', (e) => this.onWheel(e), { passive: true });
  },

  contentCss() {
    const p = this.prefs, t = RTHEMES[p.theme] || RTHEMES.light;
    const font = FONTS.find((f) => f.id === p.font) || FONTS[0];
    // Books that ask for Georgia get Cambria under that name, so Vietnamese
    // keeps its accents even with the book's own fonts.
    const georgia = [['normal', 'normal', 'Cambria'], ['bold', 'normal', 'Cambria Bold'],
      ['normal', 'italic', 'Cambria Italic'], ['bold', 'italic', 'Cambria Bold Italic']]
      .map(([w, s, face]) => `@font-face { font-family: "Georgia"; font-weight: ${w}; font-style: ${s}; src: local("${face}"); }`).join('\n');
    let css = `${georgia}
      html { -webkit-font-smoothing: antialiased; }
      html, body { background: transparent !important; color: ${t.fg} !important; }
      body { line-height: ${p.lineHeight} !important; }
      p, li, blockquote, dd, dt, div, span, td { line-height: inherit !important; }
      a, a:link, a:visited { color: ${t.link} !important; }
      img, svg, video { max-width: 100% !important; }
      ::selection { background: ${t.sel}; }`;
    if (p.justify) css += 'p { text-align: justify !important; hyphens: auto; }';
    if (p.theme !== 'light') {
      css += `body *:not(img):not(svg):not(image):not(a) { color: inherit !important; background-color: transparent !important; border-color: ${t.line} !important; }`;
    }
    if (font.css) {
      css += `body, body p, body li, body div, body span, body blockquote, body td, body h1, body h2, body h3, body h4, body h5, body h6 { font-family: ${font.css} !important; }`;
    }
    return css;
  },

  injectStyle(contents) {
    const doc = contents.document;
    if (!doc || !doc.head) return;
    let st = doc.getElementById('cp-reader-style');
    if (!st) {
      st = doc.createElement('style');
      st.id = 'cp-reader-style';
      doc.head.appendChild(st);
    }
    st.textContent = this.contentCss();
  },

  restyle(relayout) {
    if (!this.rendition) return;
    this.rendition.getContents().forEach((c) => this.injectStyle(c));
    if (relayout) this.keepPlace();
  },

  // Reflowed text moves the page; go back to the same spot.
  keepPlace() {
    clearTimeout(this.placeTimer);
    const cfi = this.location && this.location.start && this.location.start.cfi;
    this.placeTimer = setTimeout(() => { if (cfi && this.rendition) this.rendition.display(cfi); }, 260);
  },

  async rebuild() {
    const cfi = this.location && this.location.start && this.location.start.cfi;
    await this.createRendition(cfi);
  },

  // ---------- position ----------
  percentOf(loc) {
    if (!loc || !loc.start) return 0;
    if (loc.atEnd) return 1;
    const s = loc.start;
    if (this.locationsReady && typeof s.percentage === 'number' && this.book.locations.length()) return s.percentage;
    const n = this.book.spine.length || 1;
    const d = s.displayed || { page: 1, total: 1 };
    return Math.min(1, (s.index + (d.page - 1) / Math.max(1, d.total)) / n);
  },

  onRelocated(loc) {
    if (!loc || !loc.start) return;
    this.location = loc;
    const pct = this.percentOf(loc);
    this.pct = pct;
    const slider = $('#r-slider', this.el);
    if (slider && document.activeElement !== slider) {
      slider.value = Math.round(pct * 1000);
      slider.style.setProperty('--p', (pct * 100) + '%');
    }
    const pctEl = $('#r-pct', this.el);
    if (pctEl) pctEl.textContent = Math.round(pct * 100) + '%';
    const d = loc.start.displayed;
    const page = $('#r-page', this.el);
    if (page) {
      if (this.prefs.layout === 'scroll' || !d) page.textContent = this.locationsReady ? '' : 'Đang tính vị trí…';
      else {
        const left = Math.max(0, d.total - (loc.end && loc.end.displayed ? loc.end.displayed.page : d.page));
        page.textContent = left === 0 ? 'Trang cuối của chương' : `Còn ${left} trang trong chương`;
      }
    }
    this.updateChapter();
    this.updateBookmarkBtn();
    const layout = this.rendition && this.rendition.manager && this.rendition.manager.layout;
    this.el.classList.toggle('two-up', this.prefs.layout === 'spread' && !!layout && layout.divisor > 1);
    this.layoutHotspots();
    this.scheduleSnapshot();
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => this.save(), 700);
  },

  async save() {
    if (!this.rec || !this.location || !this.location.start) return;
    const ch = this.currentChapter();
    await api.post(`/api/library/${this.rec.id}/state`, {
      cfi: this.location.start.cfi,
      progress: this.pct || 0,
      chapter: ch ? ch.label : '',
    });
  },

  async loadLocations() {
    const book = this.book;
    const id = this.rec.id;
    try {
      const saved = await api.get(`/api/library/${id}/locations`);
      if (book !== this.book) return;
      if (saved && saved.length) {
        book.locations.load(saved);
      } else {
        await sleep(300);
        await book.locations.generate(1600);
        if (book !== this.book) return;
        api.req('POST', `/api/library/${id}/locations`, book.locations.save()).catch(() => {});
      }
      this.locationsReady = true;
      const cur = this.rendition && this.rendition.currentLocation();
      if (cur && cur.start) this.onRelocated(cur);
    } catch (e) {
      console.warn('locations', e);
    }
  },

  goTo(pct) {
    if (!this.rendition) return;
    if (this.locationsReady && this.book.locations.length()) {
      this.rendition.display(this.book.locations.cfiFromPercentage(Math.min(0.9999, pct)));
    } else {
      const n = this.book.spine.length;
      const item = this.book.spine.get(Math.min(n - 1, Math.floor(pct * n)));
      if (item) this.rendition.display(item.href);
    }
  },

  showTip(slider, v) {
    let tip = $('.r-tip', this.el);
    if (!tip) { tip = document.createElement('div'); tip.className = 'r-tip'; this.el.appendChild(tip); }
    const r = slider.getBoundingClientRect();
    tip.style.left = (r.left + v * r.width) + 'px';
    let label = Math.round(v * 100) + '%';
    if (this.tocFlat.length) {
      const n = this.book.spine.length;
      const ch = this.chapterAt(Math.min(n - 1, Math.floor(v * n)));
      if (ch) label += ' · ' + ch.label;
    }
    tip.textContent = label;
  },
  hideTip() { const t = $('.r-tip', this.el); if (t) t.remove(); },

  next() { return this.turn(1); },
  prev() { return this.turn(-1); },

  // ---------- page turn ----------
  // The page on screen is captured shortly after it appears. A turn draws
  // that picture on a canvas over the book and folds its corner back like a
  // sheet of paper: the fold line is the perpendicular bisector between the
  // corner and the point it is dragged to; the part beyond it is mirrored
  // across the fold and painted as the back of the sheet. epub.js renders the
  // next page underneath, so what the fold uncovers is the real next page.
  scheduleSnapshot(delay = 260) {
    clearTimeout(this.snapTimer);
    if (!this.isOpen || this.prefs.turn === 'none' || this.prefs.layout === 'scroll') return;
    this.snapTimer = setTimeout(() => this.takeSnapshot(), delay);
  },

  validSnap() {
    const s = this.snap, loc = this.location;
    return s && loc && s.cfi === loc.start.cfi && s.w === innerWidth && s.h === innerHeight ? s : null;
  },

  async takeSnapshot() {
    if (!this.isOpen || this.animating || this.panel || !this.location) return;
    const seq = this.turnSeq || 0;
    const cfi = this.location.start.cfi;
    const w = innerWidth, h = innerHeight;
    try {
      const r = await fetch('/api/window/snapshot', { method: 'POST' });
      if (!r.ok) return;
      const url = URL.createObjectURL(await r.blob());
      const img = new Image();
      img.src = url;
      await img.decode();
      const fresh = this.isOpen && !this.animating && !this.panel && (this.turnSeq || 0) === seq &&
        this.location && this.location.start.cfi === cfi && innerWidth === w && innerHeight === h;
      if (!fresh) { URL.revokeObjectURL(url); return; }
      if (this.snap) URL.revokeObjectURL(this.snap.url);
      this.snap = { url, cfi, w, h, img };
      this.prerenderGhost();
    } catch (_) { /* this page turns without animation */ }
  },

  // ---------- the back of the sheet ----------
  // A second, hidden rendition of the book holds the page that is printed on
  // the back of the turning sheet (the next left page in a spread). While a
  // page turns it is mirrored across the fold and clipped to the sheet, so the
  // back shows real text that lands exactly where that page will be.
  ghostKey() {
    const p = this.prefs;
    return [innerWidth, innerHeight, p.layout, p.margin, p.fontSize, p.font, p.lineHeight, p.justify, p.theme].join('|');
  },

  ensureGhost() {
    if (this.ghost && this.ghost.key === this.ghostKey()) return this.ghost;
    this.destroyGhost();
    const viewer = $('#r-viewer', this.el);
    const vr = viewer.getBoundingClientRect();
    const wrap = document.createElement('div');
    wrap.className = 'r-ghost-wrap';
    const gv = document.createElement('div');
    gv.className = 'r-ghost-viewer';
    Object.assign(gv.style, {
      left: vr.left + 'px', top: vr.top + 'px', width: vr.width + 'px', height: vr.height + 'px',
      padding: getComputedStyle(viewer).padding,
    });
    wrap.appendChild(gv);
    this.el.appendChild(wrap);
    const p = this.prefs;
    const r = this.book.renderTo(gv, {
      width: '100%', height: '100%', flow: 'paginated', manager: 'default',
      spread: p.layout === 'spread' ? 'auto' : 'none', minSpreadWidth: 760, allowScriptedContent: false,
    });
    r.themes.fontSize(p.fontSize + '%');
    r.hooks.content.register((c) => this.injectStyle(c));
    const g = { key: this.ghostKey(), wrap, gv, r, at: null, queue: Promise.resolve() };
    // One display at a time on the hidden rendition.
    g.run = (fn) => (g.queue = g.queue.then(fn, fn));
    this.ghost = g;
    return g;
  },

  destroyGhost() {
    const g = this.ghost;
    this.ghost = null;
    if (!g) return;
    try { g.r.destroy(); } catch (_) { /* gone */ }
    g.wrap.remove();
  },

  hideGhost() {
    const g = this.ghost;
    if (!g) return;
    Object.assign(g.wrap.style, { opacity: '0', transform: 'none', clipPath: 'none' });
  },

  // The page after this one, rendered ahead of time for the next forward turn.
  prerenderGhost() {
    if (this.prefs.turn !== 'book' || this.prefs.layout === 'scroll' || !this.location) return;
    const cfi = this.location.start.cfi;
    const g = this.ensureGhost();
    g.run(async () => {
      if (this.ghost !== g || this.animating) return;
      await g.r.display(cfi);
      await g.r.next();
      const loc = g.r.currentLocation();
      g.at = loc && loc.start ? loc.start.cfi : null;
    }).catch(() => { g.at = null; });
  },

  // Puts the page now shown underneath (the turn's destination) on the back of the sheet.
  async attachBack(k) {
    const cfi = this.location && this.location.start && this.location.start.cfi;
    if (!cfi || this.prefs.turn !== 'book') return;
    const g = this.ensureGhost();
    try {
      await g.run(async () => {
        if (g.at !== cfi) { await g.r.display(cfi); g.at = cfi; }
      });
    } catch (_) { return; }
    if (this.curl !== k || this.ghost !== g) return;
    k.back = g;
    this.drawCurl(k);
  },

  canAnimate(dir) {
    const loc = this.location;
    if (this.prefs.turn === 'none' || this.prefs.layout === 'scroll' || this.panel) return false;
    if (matchMedia('(prefers-reduced-motion: reduce)').matches) return false;
    if (!loc || (dir > 0 && loc.atEnd) || (dir < 0 && loc.atStart)) return false;
    return !!this.validSnap();
  },

  step(dir) {
    this.closeLookup();
    // A selection left on the old page would follow onto the new one.
    try { this.rendition.getContents().forEach((c) => c.window.getSelection().removeAllRanges()); } catch (_) { /* no frame */ }
    return dir > 0 ? this.rendition.next() : this.rendition.prev();
  },

  // Key, click or wheel: the corner rises and the sheet turns on its own.
  async turn(dir) {
    if (!this.rendition) return;
    this.turnSeq = (this.turnSeq || 0) + 1;
    if (this.animating) {
      // Already turning: finish that at once and go on.
      const k = this.curl;
      if (k && !k.stepped) { k.stepped = true; this.step(k.dir); }
      this.endCurl();
      return this.step(dir);
    }
    if (this.curl) this.endCurl(); // a hover peek
    if (!this.canAnimate(dir)) return this.step(dir);
    const k = this.beginCurl(dir, 'bottom');
    this.animating = true;
    k.stepped = true;
    try { await this.step(dir); } catch (_) { /* stays */ }
    await this.frames(2);
    if (this.curl !== k) return;
    k.ready = true;
    // Give the back page a moment to be ready, so the sheet shows text from the start.
    await Promise.race([this.attachBack(k), sleep(280)]);
    if (this.curl !== k) return;
    if (this.prefs.turn === 'slide') await this.animateSlide(k);
    else await this.animateCurl(k, k.C, k.E, 1050, true);
    if (this.curl === k) this.endCurl();
  },

  frames(n) {
    return new Promise((r) => { const tick = () => (--n <= 0 ? r() : requestAnimationFrame(tick)); requestAnimationFrame(tick); });
  },

  beginCurl(dir, corner) {
    const viewer = $('#r-viewer', this.el);
    const vr = viewer.getBoundingClientRect();
    const two = this.el.classList.contains('two-up');
    const half = vr.width / 2;
    const leaf = two
      ? (dir > 0 ? { x: vr.left + half, y: vr.top, w: vr.width - half, h: vr.height } : { x: vr.left, y: vr.top, w: half, h: vr.height })
      : { x: vr.left, y: vr.top, w: vr.width, h: vr.height };
    const rest = two ? (dir > 0 ? { x: vr.left, w: half } : { x: vr.left + half, w: vr.width - half }) : null;
    const spine = dir > 0 ? leaf.x : leaf.x + leaf.w;
    const C = { x: dir > 0 ? leaf.x + leaf.w : leaf.x, y: corner === 'top' ? leaf.y : leaf.y + leaf.h };
    const E = { x: 2 * spine - C.x, y: C.y };
    const canvas = document.createElement('canvas');
    canvas.className = 'r-curl';
    const dpr = devicePixelRatio || 1;
    canvas.width = Math.round(innerWidth * dpr);
    canvas.height = Math.round(innerHeight * dpr);
    this.el.appendChild(canvas);
    const topCanvas = document.createElement('canvas');
    topCanvas.className = 'r-curl r-curl-top';
    topCanvas.width = canvas.width;
    topCanvas.height = canvas.height;
    this.el.appendChild(topCanvas);
    const snap = this.validSnap();
    this.snap = null;
    const k = {
      dir, leaf, rest, spine, C, E, canvas, top: topCanvas, back: null, ctx: canvas.getContext('2d'), dpr, snap,
      paper: (RTHEMES[this.prefs.theme] || RTHEMES.light).bg,
      P: { ...C }, ready: false, alpha: 1, hideRest: false, raf: 0,
    };
    this.curl = k;
    this.drawCurl(k);
    return k;
  },

  endCurl() {
    const k = this.curl;
    this.curl = null;
    this.animating = false;
    if (k) {
      cancelAnimationFrame(k.raf);
      k.canvas.remove();
      k.top.remove();
      if (k.snap) URL.revokeObjectURL(k.snap.url);
    }
    this.hideGhost();
    this.scheduleSnapshot(160);
  },

  // The corner stays attached to the book: it cannot pull further from the
  // spine than the page is wide (or tall, across the diagonal).
  constrain(k, P) {
    const L = k.leaf;
    const near = { x: k.spine, y: k.C.y };
    const far = { x: k.spine, y: k.C.y === L.y ? L.y + L.h : L.y };
    const clamp = (c, r) => {
      const dx = P.x - c.x, dy = P.y - c.y, d = Math.hypot(dx, dy);
      if (d > r) P = { x: c.x + dx * r / d, y: c.y + dy * r / d };
    };
    clamp(near, L.w);
    clamp(far, Math.hypot(L.w, L.h));
    return P;
  },

  drawCurl(k) {
    const { ctx, leaf: L, snap, dpr } = k;
    const img = snap.img, sx = img.naturalWidth / snap.w, sy = img.naturalHeight / snap.h;
    const blit = (x, y, w, h) => ctx.drawImage(img, x * sx, y * sy, w * sx, h * sy, x, y, w, h);
    const path = (pts) => { ctx.beginPath(); pts.forEach((p, i) => (i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]))); ctx.closePath(); };
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, innerWidth, innerHeight);
    if (k.rest && !k.hideRest) blit(k.rest.x, L.y, k.rest.w, L.h);

    const C = k.C, P = k.P;
    const dist = Math.hypot(C.x - P.x, C.y - P.y);
    if (dist < 0.5) {
      blit(L.x, L.y, L.w, L.h);
      k.top.getContext('2d').clearRect(0, 0, k.top.width, k.top.height);
      this.hideGhost();
      return;
    }
    const nx = (C.x - P.x) / dist, ny = (C.y - P.y) / dist;           // fold normal, toward the corner
    const M = { x: (C.x + P.x) / 2, y: (C.y + P.y) / 2 };              // a point on the fold
    const side = (p) => (p[0] - M.x) * nx + (p[1] - M.y) * ny;        // > 0: the part that folds
    const rect = [[L.x, L.y], [L.x + L.w, L.y], [L.x + L.w, L.y + L.h], [L.x, L.y + L.h]];
    const clip = (pts, keep) => {
      const out = [];
      for (let i = 0; i < pts.length; i++) {
        const a = pts[i], b = pts[(i + 1) % pts.length], fa = keep(a), fb = keep(b);
        if (fa >= 0) out.push(a);
        if ((fa >= 0) !== (fb >= 0)) { const t = fa / (fa - fb); out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]); }
      }
      return out;
    };
    const kept = clip(rect, (p) => -side(p));
    const flap = clip(rect, side);
    const reach = Math.max(1, dist / 2);                               // fold to corner

    // What the fold uncovers: the next page (or blank paper until it has rendered),
    // darkened where the sheet still hangs over it.
    if (flap.length > 2) {
      ctx.save();
      path(flap); ctx.clip();
      if (!k.ready) { ctx.fillStyle = k.paper; ctx.fill(); }
      const g = ctx.createLinearGradient(M.x, M.y, M.x + nx * Math.min(reach * 1.4, 160), M.y + ny * Math.min(reach * 1.4, 160));
      g.addColorStop(0, `rgba(0,0,0,${0.32 * k.alpha})`);
      g.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.fillStyle = g;
      ctx.fillRect(L.x, L.y, L.w, L.h);
      ctx.restore();
    }
    // The page still lying flat, a little shaded where it starts to lift.
    if (kept.length > 2) {
      ctx.save();
      path(kept); ctx.clip();
      blit(L.x, L.y, L.w, L.h);
      const g = ctx.createLinearGradient(M.x, M.y, M.x - nx * 60, M.y - ny * 60);
      g.addColorStop(0, 'rgba(0,0,0,0.10)');
      g.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.fillStyle = g;
      ctx.fillRect(L.x, L.y, L.w, L.h);
      ctx.restore();
    }
    // The back of the sheet, mirrored across the fold.
    const top = k.top.getContext('2d');
    top.setTransform(dpr, 0, 0, dpr, 0, 0);
    top.clearRect(0, 0, innerWidth, innerHeight);
    if (flap.length <= 2) { this.hideGhost(); return; }
    const ux = -ny, uy = nx;
    const a = ux * ux - uy * uy, b = 2 * ux * uy, d = uy * uy - ux * ux;
    const e = M.x - (a * M.x + b * M.y), f = M.y - (b * M.x + d * M.y);
    const reflect = (c) => c.setTransform(dpr * a, dpr * b, dpr * b, dpr * d, dpr * e, dpr * f);
    ctx.save();
    reflect(ctx);
    path(flap);
    ctx.globalAlpha = k.alpha;
    ctx.shadowColor = 'rgba(0,0,0,0.30)';
    ctx.shadowBlur = 22 * dpr;
    ctx.fillStyle = k.paper;
    ctx.fill();
    ctx.shadowColor = 'transparent';
    if (!k.back) {
      ctx.clip();
      ctx.globalAlpha = 0.09 * k.alpha;                                // ink showing through, until the real page is ready
      blit(L.x, L.y, L.w, L.h);
    }
    ctx.restore();

    if (k.back) {
      // The printed back: the destination page, carried from where it will
      // land (the leaf mirrored across the spine) onto the sheet.
      const s = k.spine;
      const off = k.rest ? 0 : k.dir * L.w;                             // one-page view: the next page sits where this one is
      const c0 = 2 * s + off;
      const m = [-a, -b, b, d, a * c0 + e, b * c0 + f];
      const clipPts = flap.map(([x, y]) => `${(2 * s - x + off).toFixed(1)}px ${y.toFixed(1)}px`).join(',');
      Object.assign(k.back.wrap.style, {
        opacity: String(k.alpha),
        transform: `matrix(${m.map((v) => v.toFixed(5)).join(',')})`,
        clipPath: `polygon(${clipPts})`,
      });
    }

    // Curvature on top of the back: dark in the crease, a highlight where the paper bends.
    top.save();
    reflect(top);
    path(flap);
    top.clip();
    top.globalAlpha = k.alpha;
    const g = top.createLinearGradient(M.x, M.y, M.x + nx * reach, M.y + ny * reach);
    g.addColorStop(0, 'rgba(0,0,0,0.26)');
    g.addColorStop(0.08, 'rgba(0,0,0,0.10)');
    g.addColorStop(0.22, 'rgba(255,255,255,0.16)');
    g.addColorStop(0.55, 'rgba(0,0,0,0.02)');
    g.addColorStop(1, 'rgba(0,0,0,0.10)');
    top.fillStyle = g;
    top.fillRect(L.x, L.y, L.w, L.h);
    top.restore();
  },

  // Moves the corner from A to B; the corner lifts in an arc on the way.
  animateCurl(k, from, to, ms, full) {
    const ease = (t) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
    const lift = (k.C.y === k.leaf.y ? 1 : -1) * k.leaf.h * 0.16;
    const span = Math.abs(to.x - from.x) / Math.max(1, Math.abs(k.E.x - k.C.x));
    return new Promise((resolve) => {
      const t0 = performance.now();
      const tick = (now) => {
        if (this.curl !== k) return resolve();
        const t = Math.min(1, (now - t0) / ms), e = ease(t);
        const x = from.x + (to.x - from.x) * e;
        const y = from.y + (to.y - from.y) * e + lift * span * Math.sin(Math.PI * e);
        k.P = this.constrain(k, { x, y });
        // In a spread whose back page is real, the sheet lands exactly on that
        // page: no fade, and the old left page stays until it is covered.
        if (full && !(k.back && k.rest)) {
          k.hideRest = e > 0.82;
          k.alpha = e > 0.88 ? Math.max(0, 1 - (e - 0.88) / 0.12) : 1;
        }
        this.drawCurl(k);
        if (t < 1) k.raf = requestAnimationFrame(tick); else resolve();
      };
      k.raf = requestAnimationFrame(tick);
    });
  },

  animateSlide(k) {
    const L = k.leaf;
    return new Promise((resolve) => {
      const t0 = performance.now(), ms = 320;
      const tick = (now) => {
        if (this.curl !== k) return resolve();
        const t = Math.min(1, (now - t0) / ms), e = 1 - Math.pow(1 - t, 3);
        const { ctx, snap, dpr } = k;
        const sx = snap.img.naturalWidth / snap.w, sy = snap.img.naturalHeight / snap.h;
        const vx = k.rest ? Math.min(L.x, k.rest.x) : L.x, vw = k.rest ? L.w + k.rest.w : L.w;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, innerWidth, innerHeight);
        ctx.globalAlpha = 1 - e;
        ctx.drawImage(snap.img, vx * sx, L.y * sy, vw * sx, L.h * sy, vx - k.dir * vw * 0.3 * e, L.y, vw, L.h);
        ctx.globalAlpha = 1;
        if (t < 1) k.raf = requestAnimationFrame(tick); else resolve();
      };
      k.raf = requestAnimationFrame(tick);
    });
  },

  // ---------- dragging a page by its edge ----------
  hotDown(e, dir) {
    if (e.button !== 0 || this.prefs.layout === 'scroll') return;
    const hot = e.currentTarget;
    hot.setPointerCapture(e.pointerId);
    const start = { x: e.clientX, y: e.clientY, t: performance.now() };
    let drag = null;
    const samples = [];
    const move = (ev) => {
      samples.push({ x: ev.clientX, t: performance.now() });
      if (samples.length > 6) samples.shift();
      if (!drag && Math.hypot(ev.clientX - start.x, ev.clientY - start.y) > 6) drag = this.startDrag(dir, start);
      if (drag) {
        const k = drag.k;
        k.P = this.constrain(k, { x: drag.P0.x + (ev.clientX - start.x), y: drag.P0.y + (ev.clientY - start.y) });
        this.drawCurl(k);
      }
    };
    const up = (ev) => {
      hot.removeEventListener('pointermove', move);
      hot.removeEventListener('pointerup', up);
      hot.removeEventListener('pointercancel', up);
      if (!drag) { if (ev.type === 'pointerup') this.turn(dir); return; }
      const first = samples[0], last = samples[samples.length - 1];
      const v = first && last && last.t > first.t ? (last.x - first.x) / (last.t - first.t) : 0; // px/ms
      this.endDrag(drag.k, v);
    };
    hot.addEventListener('pointermove', move);
    hot.addEventListener('pointerup', up);
    hot.addEventListener('pointercancel', up);
  },

  startDrag(dir, start) {
    if (this.animating && !(this.curl && this.curl.peek)) return null;
    let k = this.curl;
    if (!k || !k.peek) {
      if (k) this.endCurl();
      if (!this.canAnimate(dir)) return null;
      const vr = $('#r-viewer', this.el).getBoundingClientRect();
      k = this.beginCurl(dir, start.y < vr.top + vr.height / 2 ? 'top' : 'bottom');
    }
    k.peek = false;
    cancelAnimationFrame(k.raf);
    this.animating = true;
    this.turnSeq = (this.turnSeq || 0) + 1;
    k.stepped = true;
    this.step(dir).catch(() => {}).then(() => this.frames(2)).then(() => {
      if (this.curl === k) { k.ready = true; this.drawCurl(k); this.attachBack(k); }
    });
    return { k, P0: { ...k.P } };
  },

  async endDrag(k, v) {
    if (this.curl !== k) return;
    const travelled = (k.C.x - k.P.x) * k.dir;              // toward the spine
    const fling = -v * k.dir;                                // px/ms toward the spine
    const done = fling > 0.45 || (travelled > k.leaf.w * 0.5 && fling > -0.3);
    const left = done ? Math.abs(k.E.x - k.P.x) : Math.abs(k.C.x - k.P.x);
    const ms = Math.max(260, Math.min(800, left * 1.1));
    if (done) {
      await this.animateCurl(k, k.P, k.E, ms, true);
      if (this.curl === k) this.endCurl();
      return;
    }
    // Let go too early: the sheet falls back, and the page returns underneath.
    await this.animateCurl(k, k.P, k.C, ms, false);
    if (this.curl !== k) return;
    k.ready = false;
    this.drawCurl(k);
    try { await this.step(-k.dir); } catch (_) { /* stays */ }
    await this.frames(2);
    if (this.curl === k) this.endCurl();
  },

  // Hovering over an edge lifts the corner a little: this page can be dragged.
  async hotPeek(dir, on, y) {
    if (this.animating || this.prefs.layout === 'scroll') return;
    if (on) {
      if (this.curl || !this.canAnimate(dir)) return;
      const vr = $('#r-viewer', this.el).getBoundingClientRect();
      const k = this.beginCurl(dir, y < vr.top + vr.height / 2 ? 'top' : 'bottom');
      k.peek = true;
      const to = this.constrain(k, { x: k.C.x - dir * 46, y: k.C.y + (k.C.y === k.leaf.y ? 34 : -34) });
      await this.animateCurl(k, k.C, to, 220, false);
    } else {
      const k = this.curl;
      if (!k || !k.peek) return;
      await this.animateCurl(k, k.P, k.C, 180, false);
      if (this.curl === k && k.peek) {
        // Not turned: keep the picture for the next turn.
        this.curl = null;
        cancelAnimationFrame(k.raf);
        k.canvas.remove();
        k.top.remove();
        this.hideGhost();
        if (this.validSnap() == null && k.snap && this.location && k.snap.cfi === this.location.start.cfi) this.snap = k.snap;
      }
    }
  },

  // The edges where a page can be grabbed: the outer margins of the book.
  layoutHotspots() {
    const viewer = $('#r-viewer', this.el), stage = $('#r-stage', this.el);
    if (!viewer || !stage) return;
    const vr = viewer.getBoundingClientRect(), sr = stage.getBoundingClientRect();
    const pad = Math.max(36, (MARGINS[this.prefs.margin] ?? 60) * 0.9);
    const prev = $('.r-nav.prev', this.el), next = $('.r-nav.next', this.el);
    if (!prev || !next) return;
    prev.style.left = '0px';
    prev.style.width = Math.max(44, vr.left - sr.left + pad) + 'px';
    next.style.right = '0px';
    next.style.width = Math.max(44, sr.right - vr.right + pad) + 'px';
  },

  // The book follows the window: lay the pages out again at the same place.
  onViewerResize() {
    clearTimeout(this.resizeTimer);
    this.layoutHotspots();
    this.resizeTimer = setTimeout(() => {
      if (!this.rendition || this.animating) return;
      const cfi = this.location && this.location.start && this.location.start.cfi;
      try { this.rendition.resize(); } catch (_) { /* next relocation fixes it */ }
      if (cfi) this.rendition.display(cfi);
      this.layoutHotspots();
    }, 220);
  },

  // ---------- contents ----------
  flattenToc(toc) {
    const out = [];
    const walk = (items, depth) => items.forEach((it) => {
      out.push({ label: (it.label || '').trim() || 'Không tên', href: it.href, depth: Math.min(depth, 3), index: this.spineIndexOf(it.href) });
      if (it.subitems && it.subitems.length) walk(it.subitems, depth + 1);
    });
    walk(toc, 0);
    this.tocFlat = out;
  },

  spineIndexOf(href) {
    if (!href || !this.book) return -1;
    const path = href.split('#')[0];
    const direct = this.book.spine.get(path);
    if (direct) return direct.index;
    const base = decodeURIComponent(path).split('/').pop();
    const item = this.book.spine.spineItems.find((s) => decodeURIComponent(s.href).split('/').pop() === base);
    return item ? item.index : -1;
  },

  resolveHref(href) {
    const [path, frag] = href.split('#');
    if (this.book.spine.get(path)) return href;
    const idx = this.spineIndexOf(href);
    const item = idx >= 0 ? this.book.spine.get(idx) : null;
    return item ? item.href + (frag ? '#' + frag : '') : href;
  },

  chapterAt(index) {
    let best = null;
    for (const t of this.tocFlat) {
      if (t.index >= 0 && t.index <= index && (!best || t.index > best.index)) best = t;
    }
    return best;
  },

  currentChapter() {
    return this.location && this.location.start ? this.chapterAt(this.location.start.index) : null;
  },

  updateChapter() {
    const ch = this.currentChapter();
    if (this.rec) this.setHeader(this.rec.title, ch ? ch.label : (this.rec.author || ''));
  },

  // ---------- bookmarks ----------
  markIndex() {
    const s = this.location && this.location.start, e = this.location && this.location.end;
    if (!s || !this.rec) return -1;
    const C = ePub.CFI ? new ePub.CFI() : null;
    return this.rec.bookmarks.findIndex((m) => {
      if (m.cfi === s.cfi) return true;
      if (!C || !e) return false;
      try { return C.compare(m.cfi, s.cfi) >= 0 && C.compare(m.cfi, e.cfi) <= 0; } catch (_) { return false; }
    });
  },

  updateBookmarkBtn() {
    const b = $('#r-bm', this.el);
    if (b) b.classList.toggle('on', this.markIndex() >= 0);
  },

  async toggleBookmark() {
    if (!this.location || !this.rec) return;
    const i = this.markIndex();
    if (i >= 0) {
      this.rec.bookmarks.splice(i, 1);
      toast('Đã bỏ dấu trang');
    } else {
      const ch = this.currentChapter();
      this.rec.bookmarks.push({ cfi: this.location.start.cfi, label: ch ? ch.label : this.rec.title, percent: this.pct || 0, created: Date.now() });
      toast('Đã thêm dấu trang', { type: 'success' });
    }
    this.updateBookmarkBtn();
    if (this.panel === 'marks') this.paintPane();
    await api.post(`/api/library/${this.rec.id}/state`, { bookmarks: this.rec.bookmarks });
  },

  // ---------- panels ----------
  togglePanel(kind) {
    if (this.panel === kind) { this.closePanels(); return; }
    if (kind === 'prefs') this.openPrefs();
    else if (this.panel && this.panel !== 'prefs') { this.panel = kind; this.paintPane(); }
    else this.openDrawer(kind);
  },

  closePanels() {
    const ov = $('#r-overlay', this.el);
    if (!ov || !this.panel) return false;
    ov.innerHTML = '';
    this.panel = null;
    this.scheduleSnapshot();
    return true;
  },

  openDrawer(tab) {
    const ov = $('#r-overlay', this.el);
    const r = this.rec;
    const cover = r.hasCover ? `<img src="/api/library/${r.id}/cover" alt="">` : placeholder(r.title, r.author);
    ov.innerHTML = `<div class="r-scrim"></div>
      <aside class="r-drawer">
        <div class="r-drawer-head">
          <div class="cover-mini">${cover}</div>
          <div class="meta"><b>${esc(r.title)}</b><span>${esc(r.author || '')}</span></div>
          <button class="r-btn" data-p="close" title="Đóng">${icon('x', 18)}</button>
        </div>
        <div class="r-tabs">
          <button data-tab="toc">Mục lục</button><button data-tab="marks">Dấu trang</button><button data-tab="search">Tìm kiếm</button>
        </div>
        <div class="r-pane" id="r-pane"></div>
      </aside>`;
    ov.querySelector('.r-scrim').onclick = () => this.closePanels();
    ov.querySelector('[data-p=close]').onclick = () => this.closePanels();
    ov.querySelectorAll('[data-tab]').forEach((b) => b.onclick = () => { this.panel = b.dataset.tab; this.paintPane(); });
    this.panel = tab;
    this.paintPane();
  },

  paintPane() {
    const ov = $('#r-overlay', this.el);
    const pane = $('#r-pane', ov);
    if (!pane) return;
    ov.querySelectorAll('[data-tab]').forEach((b) => b.classList.toggle('on', b.dataset.tab === this.panel));
    if (this.panel === 'toc') {
      if (!this.tocFlat.length) { pane.innerHTML = `<div class="r-empty">${icon('list', 30)}Sách không có mục lục.</div>`; return; }
      const cur = this.currentChapter();
      pane.innerHTML = this.tocFlat.map((t, i) =>
        `<button class="toc-item d${t.depth} ${t === cur ? 'on' : ''}" data-toc="${i}">${esc(t.label)}</button>`).join('');
      pane.onclick = (e) => {
        const b = e.target.closest('[data-toc]');
        if (!b) return;
        this.rendition.display(this.resolveHref(this.tocFlat[+b.dataset.toc].href));
        this.closePanels();
      };
      const on = pane.querySelector('.on');
      if (on) on.scrollIntoView({ block: 'center' });
    } else if (this.panel === 'marks') {
      const marks = this.rec.bookmarks.slice().sort((a, b) => a.percent - b.percent);
      if (!marks.length) {
        pane.innerHTML = `<div class="r-empty">${icon('bookmark', 30)}Chưa có dấu trang.<br>Bấm biểu tượng dấu trang hoặc phím <b>B</b> để đánh dấu trang đang đọc.</div>`;
        return;
      }
      pane.innerHTML = marks.map((m) => `<div class="mark-item" data-cfi="${esc(m.cfi)}">
        ${icon('bookmark', 17)}
        <div class="body"><b>${esc(m.label)}</b><span>${Math.round(m.percent * 100)}% · ${esc(fmtTime(m.created))}</span></div>
        <button class="r-btn del" data-del="${esc(m.cfi)}" title="Xóa">${icon('trash', 16)}</button></div>`).join('');
      pane.onclick = async (e) => {
        const del = e.target.closest('[data-del]');
        if (del) {
          this.rec.bookmarks = this.rec.bookmarks.filter((m) => m.cfi !== del.dataset.del);
          this.paintPane();
          this.updateBookmarkBtn();
          await api.post(`/api/library/${this.rec.id}/state`, { bookmarks: this.rec.bookmarks });
          return;
        }
        const it = e.target.closest('[data-cfi]');
        if (it) { this.rendition.display(it.dataset.cfi); this.closePanels(); }
      };
    } else if (this.panel === 'search') {
      const s = this.search;
      pane.innerHTML = `<form class="r-search" id="r-sform">${icon('search', 16)}<input id="r-sq" placeholder="Tìm trong sách…" value="${esc(s.q)}" autocomplete="off"></form>
        <div id="r-sres"></div>`;
      const input = $('#r-sq', pane);
      setTimeout(() => { input.focus(); input.select(); }, 30);
      $('#r-sform', pane).onsubmit = (e) => { e.preventDefault(); this.runSearch(input.value.trim()); };
      this.paintResults();
      pane.onclick = (e) => {
        const hit = e.target.closest('[data-hit]');
        if (!hit) return;
        const cfi = this.search.results[+hit.dataset.hit].cfi;
        this.rendition.display(cfi).then(() => {
          try {
            if (this.hl) this.rendition.annotations.remove(this.hl, 'highlight');
            this.hl = cfi;
            this.rendition.annotations.highlight(cfi, {}, () => {}, 'cp-hl', { fill: '#facc15', 'fill-opacity': '0.4', 'mix-blend-mode': 'multiply' });
          } catch (_) { /* highlight is a nicety */ }
        });
        this.closePanels();
      };
    }
  },

  async runSearch(q) {
    const s = this.search;
    s.q = q;
    s.results = [];
    s.token++;
    s.done = 0;
    if (q.length < 2) { s.running = false; this.paintResults(); return; }
    const token = s.token;
    s.running = true;
    this.paintResults();
    const book = this.book;
    for (const item of book.spine.spineItems) {
      if (token !== s.token || book !== this.book) return;
      try {
        await item.load(book.load.bind(book));
        const found = item.find(q);
        found.forEach((f) => s.results.push({ cfi: f.cfi, excerpt: f.excerpt, index: item.index }));
        item.unload();
      } catch (_) { /* unreadable section */ }
      s.done++;
      if (s.results.length >= 300) break;
      if (s.done % 4 === 0) this.paintResults();
    }
    s.running = false;
    this.paintResults();
  },

  paintResults() {
    const box = $('#r-sres', this.el);
    if (!box || this.panel !== 'search') return;
    const s = this.search;
    const total = this.book ? this.book.spine.length : 0;
    let html = '';
    if (s.running) html += `<div class="r-status"><span class="spinner" style="width:14px;height:14px"></span> Đang tìm… ${s.done}/${total} chương · ${s.results.length} kết quả</div>`;
    else if (s.q && s.q.length >= 2) html += `<div class="r-status">${s.results.length ? `${s.results.length}${s.results.length >= 300 ? '+' : ''} kết quả cho “${esc(s.q)}”` : `Không tìm thấy “${esc(s.q)}”`}</div>`;
    else html += `<div class="r-empty">${icon('search', 30)}Nhập từ cần tìm rồi nhấn Enter.</div>`;
    const re = s.q ? new RegExp(s.q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'gi') : null;
    html += s.results.map((r, i) => {
      const ch = this.chapterAt(r.index);
      const text = esc(r.excerpt || '').replace(re, (m) => `<mark>${m}</mark>`);
      return `<button class="hit" data-hit="${i}">${ch ? `<small>${esc(ch.label)}</small>` : ''}…${text}…</button>`;
    }).join('');
    box.innerHTML = html;
  },

  openPrefs() {
    const ov = $('#r-overlay', this.el);
    ov.innerHTML = `<div class="r-scrim" style="background:transparent"></div><div class="r-pop" id="r-pop"></div>`;
    ov.querySelector('.r-scrim').onclick = () => this.closePanels();
    this.panel = 'prefs';
    this.paintPrefs();
  },

  paintPrefs() {
    const pop = $('#r-pop', this.el);
    if (!pop) return;
    const p = this.prefs;
    const seg = (key, opts) => `<div class="r-seg">${opts.map(([v, l, ic]) =>
      `<button data-k="${key}" data-v="${v}" class="${String(p[key]) === String(v) ? 'on' : ''}">${ic ? icon(ic, 15) : ''}${l}</button>`).join('')}</div>`;
    pop.innerHTML = `
      <div class="r-group"><div class="r-label">Màu trang</div>
        <div class="swatches">${Object.entries(RTHEMES).map(([k, t]) =>
          `<button class="swatch ${p.theme === k ? 'on' : ''}" data-k="theme" data-v="${k}" style="background:${t.bg};color:${t.fg}"><b>Aa</b>${t.name}</button>`).join('')}</div></div>
      <div class="r-group"><div class="r-label">Cỡ chữ <span>${p.fontSize}%</span></div>
        <div class="stepper">
          <button data-fs="-10" style="font-size:13px" title="Nhỏ hơn (Ctrl+−)">A</button>
          <input type="range" id="r-fs-range" min="70" max="240" step="5" value="${p.fontSize}">
          <button data-fs="10" style="font-size:20px" title="Lớn hơn (Ctrl++)">A</button>
        </div></div>
      <div class="r-group"><div class="r-label">Phông chữ</div>
        <div class="fonts">${FONTS.map((f) => `<button data-k="font" data-v="${f.id}" class="${p.font === f.id ? 'on' : ''}" style="font-family:${f.show || f.css}">${f.name}</button>`).join('')}</div></div>
      <div class="r-group"><div class="r-label">Giãn dòng</div>${seg('lineHeight', LINE_HEIGHTS.map(([v, l]) => [v, l]))}</div>
      <div class="r-group"><div class="r-label">Lề trang</div>${seg('margin', [['narrow', 'Hẹp'], ['medium', 'Vừa'], ['wide', 'Rộng']])}</div>
      <div class="r-group"><div class="r-label">Bố cục</div>${seg('layout', [['spread', 'Hai trang'], ['paged', 'Một trang'], ['scroll', 'Cuộn dọc']])}</div>
      <div class="r-group"><div class="r-label">Lật trang</div>${seg('turn', [['book', 'Lật sách'], ['slide', 'Trượt'], ['none', 'Tắt']])}</div>
      <div class="r-group r-row"><span style="font-weight:600;font-size:13.5px">Bấm đúp để tra từ tiếng Anh</span>
        <label class="switch"><input type="checkbox" id="r-lookup" ${p.lookup ? 'checked' : ''}><span class="track"></span></label></div>
      <div class="r-group r-row"><span style="font-weight:600;font-size:13.5px">Căn đều hai bên</span>
        <label class="switch"><input type="checkbox" id="r-justify" ${p.justify ? 'checked' : ''}><span class="track"></span></label></div>`;
    pop.onclick = (e) => {
      const b = e.target.closest('[data-k]');
      if (b) {
        let v = b.dataset.v;
        if (b.dataset.k === 'lineHeight') v = parseFloat(v);
        this.setPref(b.dataset.k, v);
        return;
      }
      const fs = e.target.closest('[data-fs]');
      if (fs) this.setPref('fontSize', Math.max(70, Math.min(240, p.fontSize + +fs.dataset.fs)));
    };
    const range = $('#r-fs-range', pop);
    range.oninput = () => { pop.querySelector('.r-label span').textContent = range.value + '%'; };
    range.onchange = () => this.setPref('fontSize', +range.value);
    $('#r-justify', pop).onchange = (e) => this.setPref('justify', e.target.checked);
    $('#r-lookup', pop).onchange = (e) => this.setPref('lookup', e.target.checked);
  },

  setPref(key, value) {
    if (this.prefs[key] === value) return;
    this.prefs[key] = value;
    if (key === 'theme') { this.applyChrome(); this.restyle(false); }
    else if (key === 'fontSize') { this.rendition.themes.fontSize(value + '%'); this.keepPlace(); }
    else if (key === 'font' || key === 'lineHeight' || key === 'justify') this.restyle(true);
    else if (key === 'margin' || key === 'layout') this.rebuild();
    this.scheduleSnapshot(700); // the page looks different now
    this.paintPrefs();
    clearTimeout(this.prefTimer);
    this.prefTimer = setTimeout(() => {
      api.post('/api/settings', { reader: this.prefs }).then((s) => { App.state = s; }).catch(() => {});
    }, 400);
  },

  // ---------- input ----------
  tapAt(clientX) {
    if (this.prefs.layout === 'scroll') { this.toggleImmersive(); return; }
    const vr = $('#r-viewer', this.el).getBoundingClientRect();
    const rel = (clientX - vr.left) / vr.width;
    if (rel < 0.28) this.prev();
    else if (rel > 0.72) this.next();
    else this.toggleImmersive();
  },

  onFrameClick(e) {
    if (this.closeLookup()) return;
    if (this.closePanels()) return;
    clearTimeout(this.tapTimer);
    if (e.detail > 1) return; // the second click of a double click: a word lookup, not a tap
    const win = e.view;
    const sel = win && win.getSelection ? String(win.getSelection()) : '';
    if (sel.length) return;
    if (e.target && e.target.closest && e.target.closest('a')) return;
    const frame = win && win.frameElement;
    if (!frame) return;
    const x = frame.getBoundingClientRect().left + e.clientX;
    // Wait a moment: a double click on a word looks it up instead of turning.
    this.tapTimer = setTimeout(() => {
      if (String(win.getSelection() || '').trim()) return;
      this.tapAt(x);
    }, this.prefs.lookup ? 230 : 0);
  },

  // ---------- looking up a word ----------
  onSelected(contents) {
    if (!this.prefs.lookup || this.animating) return;
    const win = contents.window;
    const sel = win.getSelection();
    const text = String(sel || '').replace(/\s+/g, ' ').trim();
    // English words and short phrases only; Vietnamese and long selections are left alone.
    if (!text || text.length > 40 || !/^[A-Za-z][A-Za-z'’-]*(?: [A-Za-z'’-]+){0,2}$/.test(text)) return;
    const rect = sel.getRangeAt(0).getBoundingClientRect();
    const frame = win.frameElement.getBoundingClientRect();
    this.showLookup(text.replace(/['’]s$/, ''), this.sentenceOf(sel), {
      x: frame.left + rect.left + rect.width / 2, top: frame.top + rect.top, bottom: frame.top + rect.bottom,
    });
  },

  // The sentence around the selection, saved with the word as its example.
  sentenceOf(sel) {
    try {
      const node = sel.getRangeAt(0).startContainer;
      const el = node.nodeType === 3 ? node.parentElement : node;
      const block = el.closest('p, li, blockquote, td, h1, h2, h3, h4, h5, h6, div') || el;
      const text = block.textContent.replace(/\s+/g, ' ').trim();
      const word = String(sel).trim();
      const i = text.indexOf(word);
      if (i < 0) return '';
      // Sentences end at . ! ? possibly followed by closing quotes or brackets.
      const ends = /[.!?…]+["”’»)\]]*(?=\s|$)/g;
      let start = 0, end = text.length, m;
      while ((m = ends.exec(text))) {
        const stop = m.index + m[0].length;
        if (stop <= i) start = stop;
        else { end = stop; break; }
      }
      let s = text.slice(start, end).trim();
      if (s.length <= 220) return s;
      // A long sentence: the words around the one looked up.
      const at = s.indexOf(word);
      let from = Math.max(0, at - 100), to = Math.min(s.length, at + word.length + 100);
      if (from > 0) from = s.indexOf(' ', from) + 1;
      if (to < s.length) to = s.lastIndexOf(' ', to);
      return (from > 0 ? '…' : '') + s.slice(from, to).trim() + (to < s.length ? '…' : '');
    } catch (_) {
      return '';
    }
  },

  async showLookup(word, sentence, at) {
    this.closeLookup();
    const pop = document.createElement('div');
    pop.className = 'r-lookup';
    pop.innerHTML = `<div class="lk-head"><b class="lk-q">${esc(word)}</b>
        <button class="r-btn" data-lk="say" title="Nghe phát âm">${icon('volume', 18)}</button>
        <button class="r-btn" data-lk="close" title="Đóng (Esc)">${icon('x', 17)}</button></div>
      <div class="lk-body"><div class="r-status"><span class="spinner" style="width:14px;height:14px"></span> Đang tra từ…</div></div>`;
    this.el.appendChild(pop);
    this.lookup = { pop, word, sentence, entries: [] };
    const place = () => {
      const w = pop.offsetWidth, h = pop.offsetHeight;
      const left = Math.max(12, Math.min(innerWidth - w - 12, at.x - w / 2));
      const below = at.bottom + 10;
      const top = below + h < innerHeight - 12 ? below : Math.max(12, at.top - h - 10);
      Object.assign(pop.style, { left: left + 'px', top: top + 'px' });
    };
    place();
    pop.onclick = (e) => {
      const b = e.target.closest('[data-lk]');
      if (!b) return;
      if (b.dataset.lk === 'close') this.closeLookup();
      if (b.dataset.lk === 'say') speak(word);
      if (b.dataset.lk === 'login') { this.close(true).then(() => { location.hash = '#/discover'; }); }
      if (b.dataset.lk === 'save') this.saveWord(+b.dataset.i, b);
    };
    const body = pop.querySelector('.lk-body');
    let r;
    try {
      r = await api.get('/api/dict/lookup?q=' + encodeURIComponent(word));
    } catch (e) {
      if (!this.lookup || this.lookup.pop !== pop) return;
      body.innerHTML = e.status === 401
        ? `<div class="lk-none">Đăng nhập thư viện để tra từ và lưu vào bộ từ của bạn.</div>
           <button class="btn sm primary" data-lk="login">${icon('user', 15)} Đăng nhập</button>`
        : `<div class="lk-none">${esc(e.message)}</div>`;
      place();
      return;
    }
    if (!this.lookup || this.lookup.pop !== pop) return;
    this.lookup.entries = r.entries;
    body.innerHTML = r.entries.length ? r.entries.map((e, i) => `
      <div class="lk-entry">
        <div class="lk-word">${e.word.toLowerCase() !== word.toLowerCase() ? `<b>${esc(e.word)}</b>` : ''}${e.ipa ? `<span class="ipa">${esc(e.ipa)}</span>` : ''}</div>
        <div class="lk-mean">${esc(e.meaning)}</div>
        ${e.example ? `<div class="lk-ex">${esc(e.example)}</div>` : ''}
        <button class="btn sm ${e.saved ? 'soft' : ''}" data-lk="save" data-i="${i}" ${e.saved ? 'disabled' : ''}>${e.saved ? icon('check', 14) + ' Đã lưu' : icon('plus', 14) + ' Lưu từ'}</button>
      </div>`).join('')
      : '<div class="lk-none">Từ này chưa có trong từ điển.</div>';
    if (sentence && r.entries.length) body.insertAdjacentHTML('beforeend', `<div class="lk-ctx">${icon('book', 13)} <span>${esc(sentence)}</span></div>`);
    place();
  },

  async saveWord(i, btn) {
    const lk = this.lookup;
    const e = lk && lk.entries[i];
    if (!e) return;
    btn.disabled = true;
    try {
      // The sentence from the book is the example: the word as it was met.
      await api.post('/api/words/save', { word: e.word, ipa: e.ipa, meaning: e.meaning, example: lk.sentence || e.example });
      btn.classList.add('soft');
      btn.innerHTML = icon('check', 14) + ' Đã lưu';
      e.saved = true;
      App.words = null; // the word list reloads next time
    } catch (err) {
      btn.disabled = false;
      toast(err.message, { type: 'error' });
    }
  },

  closeLookup() {
    const lk = this.lookup;
    this.lookup = null;
    if (!lk) return false;
    lk.pop.remove();
    return true;
  },

  toggleImmersive(on) {
    const hide = this.el.classList.toggle('immersive', on);
    this.el.classList.remove('peek-top', 'peek-bottom');
    if (hide && !this.immersiveHinted) {
      this.immersiveHinted = true;
      toast('Đã ẩn menu. Bấm giữa trang, nhấn M hoặc đưa chuột lên mép trên để hiện lại.');
    }
  },

  onWheel(e) {
    if (this.prefs.layout === 'scroll' || this.panel) return;
    const now = Date.now();
    if (now - (this.lastWheel || 0) < 380) return;
    const d = Math.abs(e.deltaY) > Math.abs(e.deltaX) ? e.deltaY : e.deltaX;
    if (Math.abs(d) < 12) return;
    this.lastWheel = now;
    if (d > 0) this.next(); else this.prev();
  },

  toggleFullscreen() {
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    else this.el.requestFullscreen().catch(() => {});
  },

  onKey(e) {
    if (!this.isOpen) return;
    const tag = (e.target && e.target.tagName) || '';
    if (/INPUT|TEXTAREA|SELECT/.test(tag)) {
      if (e.key === 'Escape') { e.target.blur(); this.closePanels(); }
      return;
    }
    const k = e.key;
    const ctrl = e.ctrlKey || e.metaKey;
    const stop = () => { if (e.preventDefault) e.preventDefault(); };
    if (ctrl && (k === 'f' || k === 'F')) { stop(); this.openDrawer('search'); return; }
    if (ctrl && (k === '+' || k === '=')) { stop(); this.setPref('fontSize', Math.min(240, this.prefs.fontSize + 10)); return; }
    if (ctrl && (k === '-' || k === '_')) { stop(); this.setPref('fontSize', Math.max(70, this.prefs.fontSize - 10)); return; }
    if (ctrl) return;
    if (k === 'ArrowRight' || k === 'PageDown' || (k === ' ' && !e.shiftKey)) {
      if (this.prefs.layout === 'scroll' && k !== 'ArrowRight') return;
      stop(); this.next();
    } else if (k === 'ArrowLeft' || k === 'PageUp' || (k === ' ' && e.shiftKey)) {
      if (this.prefs.layout === 'scroll' && k !== 'ArrowLeft') return;
      stop(); this.prev();
    } else if (k === 'Escape') {
      if (this.closeLookup()) return;
      if (this.closePanels()) return;
      if (this.el.classList.contains('immersive')) { this.toggleImmersive(false); return; }
      if (document.fullscreenElement) { document.exitFullscreen().catch(() => {}); return; }
      this.close(true);
    } else if (k === 'm' || k === 'M') {
      stop(); this.toggleImmersive();
    } else if (k === 'F11' || k === 'f' || k === 'F') {
      stop(); this.toggleFullscreen();
    } else if (k === 't' || k === 'T') {
      this.togglePanel('toc');
    } else if (k === 'b' || k === 'B') {
      this.toggleBookmark();
    }
  },
};

window.addEventListener('keydown', (e) => { if (Reader.isOpen) Reader.onKey(e); });
document.addEventListener('fullscreenchange', () => {
  const b = document.getElementById('r-fs');
  if (b) b.innerHTML = icon(document.fullscreenElement ? 'minimize' : 'maximize', 18);
});
