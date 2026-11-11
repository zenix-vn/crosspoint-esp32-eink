'use strict';

// ---------- helpers ----------
const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const fold = (s) => String(s ?? '').normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/đ/g, 'd').replace(/Đ/g, 'D').toLowerCase();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function fillIcons(root = document) {
  $$('[data-icon]', root).forEach((el) => { el.innerHTML = icon(el.dataset.icon, +(el.dataset.size || 20)); });
}

function fmtSize(n) {
  if (!n) return '';
  if (n < 1024 * 1024) return Math.max(1, Math.round(n / 1024)) + ' KB';
  return (n / 1024 / 1024).toFixed(1).replace('.', ',') + ' MB';
}

function fmtTime(ms) {
  if (!ms) return '';
  const d = new Date(ms);
  const diff = (Date.now() - ms) / 1000;
  if (diff < 60) return 'vừa xong';
  if (diff < 3600) return Math.floor(diff / 60) + ' phút trước';
  if (diff < 86400) return Math.floor(diff / 3600) + ' giờ trước';
  if (diff < 7 * 86400) return Math.floor(diff / 86400) + ' ngày trước';
  return d.toLocaleDateString('vi-VN');
}

const api = {
  async req(method, url, body) {
    const opts = { method, headers: {} };
    if (body !== undefined) {
      if (body instanceof ArrayBuffer || body instanceof Blob || typeof body === 'string') opts.body = body;
      else { opts.body = JSON.stringify(body); opts.headers['Content-Type'] = 'application/json'; }
    }
    const r = await fetch(url, opts);
    const type = r.headers.get('content-type') || '';
    const data = type.includes('json') ? await r.json() : await r.text();
    if (!r.ok) {
      const err = new Error((data && data.error) || `Lỗi ${r.status}`);
      err.status = r.status;
      throw err;
    }
    return data;
  },
  get(url) { return this.req('GET', url); },
  post(url, body = {}) { return this.req('POST', url, body); },
};

// ---------- toasts, modals, menus ----------
function toast(message, { type = 'info', action, timeout = 4200 } = {}) {
  const el = document.createElement('div');
  el.className = `toast ${type}`;
  const ic = type === 'success' ? 'check' : type === 'error' ? 'alert' : 'info';
  el.innerHTML = `<span class="t-ic">${icon(ic, 15)}</span><span class="t-msg">${esc(message)}</span>${action ? `<button class="t-act">${esc(action.label)}</button>` : ''}`;
  const close = () => { el.classList.add('out'); setTimeout(() => el.remove(), 220); };
  if (action) el.querySelector('.t-act').onclick = () => { close(); action.fn(); };
  $('#toasts').appendChild(el);
  if (timeout) setTimeout(close, timeout);
  return close;
}

let modalStack = [];
function openModal(html, { wide = false, onClose } = {}) {
  const back = document.createElement('div');
  back.className = 'backdrop';
  back.innerHTML = `<div class="modal ${wide ? 'wide' : ''}" role="dialog" aria-modal="true">${html}</div>`;
  const close = () => {
    back.remove();
    modalStack = modalStack.filter((m) => m !== close);
    onClose && onClose();
  };
  back.addEventListener('mousedown', (e) => { if (e.target === back) close(); });
  $('#layer').appendChild(back);
  modalStack.push(close);
  fillIcons(back);
  const focusable = back.querySelector('[autofocus], .modal-actions .btn.primary, .modal-actions .btn.danger-fill');
  setTimeout(() => focusable && focusable.focus(), 30);
  return { el: back.firstElementChild, close };
}

function confirmDialog({ title, message, confirm = 'Đồng ý', danger = false }) {
  return new Promise((resolve) => {
    let done = false;
    const m = openModal(`
      <h2>${esc(title)}</h2>
      <p class="modal-text">${esc(message)}</p>
      <div class="modal-actions">
        <button class="btn ghost" data-r="no">Hủy</button>
        <button class="btn ${danger ? 'danger-fill' : 'primary'}" data-r="yes">${esc(confirm)}</button>
      </div>`, { onClose: () => { if (!done) resolve(false); } });
    m.el.querySelector('[data-r=no]').onclick = () => m.close();
    m.el.querySelector('[data-r=yes]').onclick = () => { done = true; m.close(); resolve(true); };
  });
}

let openMenuClose = null;
function openMenu(anchor, items) {
  if (openMenuClose) openMenuClose();
  const menu = document.createElement('div');
  menu.className = 'menu';
  menu.innerHTML = items.map((it, i) => it === '-' ? '<hr>' :
    `<button data-i="${i}" class="${it.danger ? 'danger' : ''}">${icon(it.icon, 17)}<span>${esc(it.label)}</span></button>`).join('');
  document.body.appendChild(menu);
  const r = anchor.getBoundingClientRect();
  const mw = menu.offsetWidth, mh = menu.offsetHeight;
  let left = Math.min(r.right - mw, window.innerWidth - mw - 8);
  if (left < 8) left = Math.min(r.left, window.innerWidth - mw - 8);
  let top = r.bottom + 6;
  if (top + mh > window.innerHeight - 8) top = r.top - mh - 6;
  menu.style.left = left + 'px';
  menu.style.top = Math.max(8, top) + 'px';
  const close = () => { menu.remove(); document.removeEventListener('mousedown', outside, true); openMenuClose = null; };
  const outside = (e) => { if (!menu.contains(e.target)) close(); };
  setTimeout(() => document.addEventListener('mousedown', outside, true));
  menu.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-i]');
    if (!b) return;
    close();
    items[+b.dataset.i].fn();
  });
  openMenuClose = close;
}

// ---------- covers ----------
const PALETTES = [
  ['#4f46e5', '#7c3aed'], ['#0ea5e9', '#2563eb'], ['#059669', '#0d9488'], ['#db2777', '#9333ea'],
  ['#ea580c', '#dc2626'], ['#0f766e', '#155e75'], ['#7c2d12', '#b45309'], ['#334155', '#0f172a'],
  ['#be123c', '#7f1d1d'], ['#4338ca', '#1e3a8a'],
];
function hashStr(s) { let h = 0; for (const c of String(s)) h = (h * 31 + c.charCodeAt(0)) | 0; return Math.abs(h); }
function placeholder(title, author) {
  const [a, b] = PALETTES[hashStr(title + author) % PALETTES.length];
  return `<div class="cover-ph" style="background:linear-gradient(150deg,${a},${b})"><b>${esc(title)}</b><span>${esc(author || '')}</span></div>`;
}
// A broken cover image falls back to the generated one.
document.addEventListener('error', (e) => {
  const img = e.target;
  if (img.tagName === 'IMG' && img.dataset.ph !== undefined) {
    img.insertAdjacentHTML('afterend', placeholder(img.dataset.title || '', img.dataset.author || ''));
    img.remove();
  }
}, true);
function coverImg(src, title, author) {
  return `<img src="${esc(src)}" alt="" loading="lazy" decoding="async" data-ph data-title="${esc(title)}" data-author="${esc(author)}">`;
}

// ---------- app state ----------
const App = {
  state: null,
  books: [],
  view: null,
  libFilter: { q: '', sort: 'recent', shelf: 'all' },
  discover: { stack: [], feed: null, entries: [], next: null, loading: false, error: null },
  inbox: { status: null, loading: false, error: null, syncing: false, lastCheck: null },
};

const mq = window.matchMedia('(prefers-color-scheme: dark)');
function applyTheme() {
  const pref = App.state?.theme || 'system';
  const dark = pref === 'dark' || (pref === 'system' && mq.matches);
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  api.post('/api/window/theme', { dark }).catch(() => {});
}
mq.addEventListener('change', applyTheme);

async function refreshState() {
  App.state = await api.get('/api/state');
  renderAccount();
  return App.state;
}

async function refreshBooks() {
  App.books = await api.get('/api/library');
  const unread = App.books.filter((b) => !b.lastOpened).length;
  const badge = $('#badge-library');
  badge.hidden = unread === 0;
  badge.textContent = unread > 99 ? '99+' : unread;
  return App.books;
}

function renderAccount() {
  const s = App.state;
  const el = $('#account');
  if (s.loggedIn) {
    el.innerHTML = `<span class="avatar">${esc(s.email.slice(0, 1).toUpperCase())}</span>
      <span class="account-text"><b>${esc(s.email)}</b><span>${esc(new URL(s.serverUrl).host)}</span></span>`;
    el.title = s.email;
  } else {
    el.innerHTML = `<span class="avatar off">${icon('user', 16)}</span>
      <span class="account-text"><b>Chưa đăng nhập</b><span>Kết nối thư viện online</span></span>`;
    el.title = 'Đăng nhập';
  }
}
$('#account').onclick = () => {
  if (App.state.loggedIn) location.hash = '#/settings';
  else location.hash = '#/discover';
};

// ---------- routing ----------
const main = $('#main');
async function route() {
  const hash = location.hash || '#/library';
  const [, view, arg] = hash.split('/');
  if (view === 'read' && arg) {
    await Reader.open(arg);
    return;
  }
  if (Reader.isOpen) await Reader.close(false);
  App.lastHash = hash;
  App.view = view || 'library';
  $$('#app [data-view]').forEach((a) => a.classList.toggle('active', a.dataset.view === App.view));
  const views = { library: renderLibrary, discover: renderDiscover, words: renderWords, translate: renderTranslate, inbox: renderInbox, settings: renderSettings };
  main.scrollTop = 0;
  await (views[App.view] || renderLibrary)(arg);
}
window.addEventListener('hashchange', route);

function openBook(id) { location.hash = '#/read/' + id; }

// ---------- library ----------
function bookCard(b) {
  const pct = Math.round((b.progress || 0) * 100);
  const done = pct >= 98;
  const cover = b.hasCover ? coverImg(`/api/library/${b.id}/cover`, b.title, b.author) : placeholder(b.title, b.author);
  const badge = !b.lastOpened ? '<span class="tag new">Mới</span>' : done ? '<span class="tag done">Đã xong</span>' : '';
  const meta = !b.lastOpened ? `${icon('sparkle', 13)} Chưa đọc` : done ? `${icon('check', 13)} Đọc xong` : `${pct}% · ${esc(fmtTime(b.lastOpened))}`;
  return `<article class="book" tabindex="0" data-id="${b.id}" title="${esc(b.title)}">
    <div class="cover-wrap">${cover}
      <div class="cover-badges">${badge}</div>
      ${pct > 0 && !done ? `<div class="cover-progress"><i style="width:${pct}%"></i></div>` : ''}
      <button class="icon-btn more" data-act="menu" title="Tùy chọn">${icon('more', 18)}</button>
    </div>
    <div class="book-title">${esc(b.title)}</div>
    <div class="book-author">${esc(b.author || 'Không rõ tác giả')}</div>
    <div class="book-meta">${meta}</div>
  </article>`;
}

function filteredBooks() {
  const { q, sort, shelf } = App.libFilter;
  let list = App.books.slice();
  if (q) {
    const words = fold(q).split(/\s+/).filter(Boolean);
    list = list.filter((b) => { const hay = fold(b.title + ' ' + b.author); return words.every((w) => hay.includes(w)); });
  }
  if (shelf === 'reading') list = list.filter((b) => b.lastOpened && b.progress < 0.98);
  if (shelf === 'unread') list = list.filter((b) => !b.lastOpened);
  if (shelf === 'done') list = list.filter((b) => b.progress >= 0.98);
  const by = {
    recent: (a, b) => (b.lastOpened || b.added) - (a.lastOpened || a.added),
    added: (a, b) => b.added - a.added,
    title: (a, b) => a.title.localeCompare(b.title, 'vi'),
    author: (a, b) => (a.author || '~').localeCompare(b.author || '~', 'vi'),
  };
  return list.sort(by[sort]);
}

async function renderLibrary() {
  if (!App.books.length) await refreshBooks();
  const books = App.books;
  const last = books.filter((b) => b.lastOpened && b.progress < 0.98).sort((a, b) => b.lastOpened - a.lastOpened)[0];
  const counts = {
    all: books.length,
    reading: books.filter((b) => b.lastOpened && b.progress < 0.98).length,
    unread: books.filter((b) => !b.lastOpened).length,
    done: books.filter((b) => b.progress >= 0.98).length,
  };
  const f = App.libFilter;
  main.innerHTML = `<div class="page">
    <header class="page-head">
      <div><h1>Thư viện của tôi</h1><p class="sub">${books.length ? `${books.length} cuốn sách trên máy tính này` : 'Sách bạn thêm hoặc nhận sẽ nằm ở đây'}</p></div>
      <div class="head-actions">
        ${books.length ? `<div class="input-wrap search-box">${icon('search', 17)}<input class="input" id="lib-q" placeholder="Tìm tên sách, tác giả…" value="${esc(f.q)}"></div>` : ''}
        <button class="btn primary" data-act="import">${icon('plus', 18)} Thêm sách</button>
      </div>
    </header>
    ${last ? heroCard(last) : ''}
    ${books.length ? `
      <div class="toolbar">
        <div class="chips" id="shelves">
          ${[['all', 'Tất cả'], ['reading', 'Đang đọc'], ['unread', 'Chưa đọc'], ['done', 'Đã xong']].map(([k, l]) =>
            `<button class="chip ${f.shelf === k ? 'on' : ''}" data-shelf="${k}">${l} <span class="count">${counts[k]}</span></button>`).join('')}
        </div>
        <select class="input sm" id="lib-sort" title="Sắp xếp">
          ${[['recent', 'Đọc gần đây'], ['added', 'Mới thêm'], ['title', 'Tên sách A–Z'], ['author', 'Tác giả A–Z']].map(([k, l]) =>
            `<option value="${k}" ${f.sort === k ? 'selected' : ''}>${l}</option>`).join('')}
        </select>
      </div>
      <div class="book-grid" id="lib-grid"></div>` : emptyLibrary()}
  </div>`;
  if (books.length) renderLibraryGrid();
  wireLibrary();
}

function heroCard(b) {
  const pct = Math.round(b.progress * 100);
  return `<section class="hero">
    <div class="cover-wrap">${b.hasCover ? coverImg(`/api/library/${b.id}/cover`, b.title, b.author) : placeholder(b.title, b.author)}</div>
    <div class="hero-body">
      <div class="hero-kicker">${icon('clock', 14)} Đọc tiếp</div>
      <div class="hero-title">${esc(b.title)}</div>
      <div class="hero-author">${esc(b.author || 'Không rõ tác giả')}</div>
      <div class="hero-progress"><div class="bar"><i style="width:${pct}%"></i></div><span>${pct}%</span></div>
      ${b.chapter ? `<div class="hero-chapter">${esc(b.chapter)}</div>` : ''}
      <div class="hero-actions">
        <button class="btn primary lg" data-open="${b.id}">${icon('book', 18)} Tiếp tục đọc</button>
      </div>
    </div>
  </section>`;
}

function emptyLibrary() {
  return `<div class="empty">
    <div class="empty-art">${icon('book', 40)}</div>
    <h2>Thư viện đang trống</h2>
    <p>Kéo thả tệp EPUB vào cửa sổ này, tải sách từ thư viện CrossPoint, hoặc nhận sách gửi từ web qua Wi-Fi.</p>
    <div class="row">
      <button class="btn primary" data-act="import">${icon('upload', 17)} Chọn tệp EPUB</button>
      <a class="btn" href="#/discover">${icon('compass', 17)} Khám phá thư viện</a>
      <a class="btn" href="#/inbox">${icon('wifi', 17)} Nhận sách</a>
    </div>
  </div>`;
}

function renderLibraryGrid() {
  const grid = $('#lib-grid');
  if (!grid) return;
  const list = filteredBooks();
  grid.innerHTML = list.length ? list.map(bookCard).join('')
    : `<div class="faint" style="grid-column:1/-1;padding:40px 0;text-align:center">Không có sách nào khớp.</div>`;
}

function wireLibrary() {
  const q = $('#lib-q');
  if (q) q.oninput = () => { App.libFilter.q = q.value; renderLibraryGrid(); };
  const sort = $('#lib-sort');
  if (sort) sort.onchange = () => { App.libFilter.sort = sort.value; renderLibraryGrid(); };
  $$('#shelves .chip').forEach((c) => c.onclick = () => {
    App.libFilter.shelf = c.dataset.shelf;
    $$('#shelves .chip').forEach((x) => x.classList.toggle('on', x === c));
    renderLibraryGrid();
  });
}

main.addEventListener('click', (e) => {
  const t = e.target;
  if (t.closest('[data-act=import]')) { $('#file-input').click(); return; }
  const open = t.closest('[data-open]');
  if (open) { openBook(open.dataset.open); return; }
  if (App.view !== 'library') return;
  const card = t.closest('.book[data-id]');
  if (!card) return;
  const id = card.dataset.id;
  if (t.closest('[data-act=menu]')) { e.stopPropagation(); bookMenu(t.closest('[data-act=menu]'), id); return; }
  openBook(id);
});
main.addEventListener('keydown', (e) => {
  const card = e.target.closest?.('.book[data-id]');
  if (card && App.view === 'library' && e.key === 'Enter') openBook(card.dataset.id);
});
main.addEventListener('contextmenu', (e) => {
  const card = e.target.closest('.book[data-id]');
  if (!card || App.view !== 'library') return;
  e.preventDefault();
  bookMenu(card.querySelector('[data-act=menu]'), card.dataset.id);
});

function bookMenu(anchor, id) {
  const b = App.books.find((x) => x.id === id);
  if (!b) return;
  openMenu(anchor, [
    { icon: 'book', label: b.lastOpened ? 'Đọc tiếp' : 'Bắt đầu đọc', fn: () => openBook(id) },
    { icon: 'info', label: 'Thông tin sách', fn: () => bookInfo(b) },
    { icon: 'send', label: 'Gửi tới máy đọc', fn: () => sendBookDialog(b) },
    ...(b.source === 'translate' ? [] : [{ icon: 'translate', label: 'Dịch sang tiếng Việt', fn: () => { location.hash = '#/translate/' + id; } }]),
    { icon: 'folder', label: 'Hiện trong thư mục', fn: () => api.post(`/api/library/${id}/reveal`) },
    '-',
    { icon: 'trash', label: 'Xóa khỏi thư viện', danger: true, fn: () => deleteBook(b) },
  ]);
}

function bookInfo(b) {
  const m = openModal(`
    <button class="icon-btn close" data-r="x">${icon('x', 18)}</button>
    <div class="detail">
      <div class="cover-wrap">${b.hasCover ? coverImg(`/api/library/${b.id}/cover`, b.title, b.author) : placeholder(b.title, b.author)}</div>
      <div class="detail-body">
        <h2>${esc(b.title)}</h2>
        <div class="by">${esc(b.author || 'Không rõ tác giả')}</div>
        <div class="detail-meta">
          ${b.language ? `<span>${icon('globe', 13)} ${esc(b.language)}</span>` : ''}
          <span>${icon('download', 13)} ${esc(fmtSize(b.fileSize))}</span>
          <span>${icon(b.source === 'inbox' ? 'wifi' : b.source === 'opds' ? 'compass' : b.source === 'translate' ? 'translate' : 'upload', 13)} ${b.source === 'inbox' ? 'Nhận qua Wi-Fi' : b.source === 'opds' ? 'Từ thư viện online' : b.source === 'translate' ? 'Bản dịch' : 'Nhập từ máy'}</span>
          <span>${icon('clock', 13)} Thêm ${esc(fmtTime(b.added))}</span>
        </div>
        ${b.description ? `<div class="detail-summary">${esc(b.description)}</div>` : ''}
        <div class="detail-actions">
          <button class="btn primary" data-r="read">${icon('book', 17)} ${b.lastOpened ? `Đọc tiếp (${Math.round(b.progress * 100)}%)` : 'Bắt đầu đọc'}</button>
          <button class="btn" data-r="send">${icon('send', 16)} Gửi tới máy đọc</button>
          ${b.source === 'translate' ? '' : `<button class="btn" data-r="tr">${icon('translate', 16)} Dịch sang tiếng Việt</button>`}
        </div>
      </div>
    </div>`, { wide: true });
  m.el.querySelector('[data-r=x]').onclick = m.close;
  m.el.querySelector('[data-r=read]').onclick = () => { m.close(); openBook(b.id); };
  m.el.querySelector('[data-r=send]').onclick = () => { m.close(); sendBookDialog(b); };
  const tr = m.el.querySelector('[data-r=tr]');
  if (tr) tr.onclick = () => { m.close(); location.hash = '#/translate/' + b.id; };
}

async function deleteBook(b) {
  const ok = await confirmDialog({ title: 'Xóa sách?', message: `“${b.title}” sẽ bị xóa khỏi máy tính này, cùng tiến độ đọc và dấu trang.`, confirm: 'Xóa sách', danger: true });
  if (!ok) return;
  await api.post(`/api/library/${b.id}/delete`);
  await refreshBooks();
  if (App.view === 'library') renderLibrary();
  toast('Đã xóa sách', { type: 'success' });
}

// Import from the file picker or a drop.
async function importFiles(files) {
  files = [...files].filter((f) => /\.(epub|txt)$/i.test(f.name));
  if (!files.length) { toast('Chỉ hỗ trợ tệp .epub và .txt', { type: 'error' }); return; }
  const closeBusy = toast(files.length > 1 ? `Đang thêm ${files.length} sách…` : `Đang thêm “${files[0].name}”…`, { timeout: 0 });
  let last = null, ok = 0;
  for (const f of files) {
    try {
      last = await api.req('POST', '/api/library/upload?name=' + encodeURIComponent(f.name), await f.arrayBuffer());
      ok++;
    } catch (e) {
      toast(`${f.name}: ${e.message}`, { type: 'error' });
    }
  }
  closeBusy();
  await refreshBooks();
  if (App.view === 'library') renderLibrary();
  if (ok) toast(ok > 1 ? `Đã thêm ${ok} sách vào thư viện` : `Đã thêm “${last.title}”`, { type: 'success', action: ok === 1 ? { label: 'Đọc ngay', fn: () => openBook(last.id) } : undefined });
}
$('#file-input').onchange = (e) => { importFiles(e.target.files); e.target.value = ''; };

let dragDepth = 0;
const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes('Files');
window.addEventListener('dragenter', (e) => { if (!hasFiles(e)) return; e.preventDefault(); dragDepth++; $('#drop').hidden = Reader.isOpen; });
window.addEventListener('dragover', (e) => { if (hasFiles(e)) e.preventDefault(); });
window.addEventListener('dragleave', (e) => { if (!hasFiles(e)) return; dragDepth = Math.max(0, dragDepth - 1); if (!dragDepth) $('#drop').hidden = true; });
window.addEventListener('drop', (e) => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  dragDepth = 0;
  $('#drop').hidden = true;
  if (!Reader.isOpen) importFiles(e.dataTransfer.files);
});

// ---------- discover (OPDS) ----------
function renderLogin(error) {
  const s = App.state;
  main.innerHTML = `<div class="page"><div class="login-wrap">
    <div class="card login-card">
      <div class="logo">${icon('compass', 28)}</div>
      <h2>Kết nối thư viện CrossPoint</h2>
      <p class="muted" style="margin-top:6px">Đăng nhập bằng tài khoản thư viện để duyệt, tìm và tải sách qua OPDS.</p>
      <form id="login" autocomplete="off">
        ${error ? `<div class="alert error">${icon('alert', 17)}<span>${esc(error)}</span></div>` : ''}
        <div class="field"><label for="l-server">Địa chỉ OPDS</label>
          <div class="input-wrap">${icon('globe', 17)}<input class="input" id="l-server" required spellcheck="false" value="${esc(s.opdsUrl)}"></div></div>
        <div class="field"><label for="l-email">Tên đăng nhập</label>
          <div class="input-wrap">${icon('user', 17)}<input class="input" id="l-email" type="text" required spellcheck="false" autocapitalize="off" placeholder="Tên đăng nhập hoặc email" value="${esc(s.email)}" autofocus></div></div>
        <div class="field"><label for="l-pass">Mật khẩu</label>
          <div class="input-wrap"><input class="input" id="l-pass" type="password" required placeholder="Mật khẩu" style="padding-left:14px;padding-right:46px">
          <button type="button" class="icon-btn trail" id="l-eye" title="Hiện mật khẩu">${icon('eye', 17)}</button></div></div>
        <button class="btn primary lg block" type="submit" id="l-submit">Đăng nhập</button>
      </form>
      <div class="login-foot">Chưa có tài khoản? <button class="linkish" id="l-signup">Đăng ký tại ${esc(new URL(s.serverUrl).host)}</button></div>
    </div></div></div>`;
  $('#l-eye').onclick = () => {
    const p = $('#l-pass');
    p.type = p.type === 'password' ? 'text' : 'password';
    $('#l-eye').innerHTML = icon(p.type === 'password' ? 'eye' : 'eyeOff', 17);
  };
  $('#l-signup').onclick = () => api.post('/api/open-url', { url: App.state.serverUrl + '/library/' });
  if (s.email) setTimeout(() => $('#l-pass').focus(), 40);
  $('#login').onsubmit = async (e) => {
    e.preventDefault();
    const btn = $('#l-submit');
    btn.disabled = true;
    btn.innerHTML = '<span class="spinner"></span> Đang kết nối…';
    try {
      App.state = await api.post('/api/opds/login', { email: $('#l-email').value, password: $('#l-pass').value, serverUrl: $('#l-server').value });
      renderAccount();
      App.discover = { stack: [], feed: null, entries: [], next: null, loading: false, error: null };
      toast('Đã kết nối thư viện', { type: 'success' });
      renderDiscover();
    } catch (err) {
      renderLogin(err.message);
    }
  };
}

const NAV_ICONS = [
  [/đang đọc/i, 'book', '#5b5bd6'], [/muốn đọc/i, 'bookmark', '#0ea5e9'], [/yêu thích/i, 'heart', '#e11d48'],
  [/mới/i, 'sparkle', '#f59e0b'], [/anh|english/i, 'globe', '#10b981'],
];
function navTile(entry, i) {
  const m = NAV_ICONS.find(([re]) => re.test(entry.title));
  const [ic, color] = m ? [m[1], m[2]] : ['layers', PALETTES[hashStr(entry.title) % PALETTES.length][0]];
  return `<button class="tile" data-nav="${i}">
    <span class="tile-ic" style="background:color-mix(in srgb, ${color} 14%, transparent);color:${color}">${icon(ic, 20)}</span>
    <span class="tile-body"><b>${esc(entry.title)}</b><span>${esc(entry.summary || 'Xem danh mục')}</span></span>
    ${icon('right', 17)}
  </button>`;
}

function opdsBookCard(e, i) {
  const cover = e.thumb ? coverImg('/api/opds/image?url=' + encodeURIComponent(e.thumb), e.title, e.author) : placeholder(e.title, e.author);
  return `<article class="book" tabindex="0" data-entry="${i}" title="${esc(e.title)}">
    <div class="cover-wrap">${cover}<div class="cover-badges">${e.libraryId ? '<span class="tag done">Đã có</span>' : ''}</div></div>
    <div class="book-title">${esc(e.title)}</div>
    <div class="book-author">${esc(e.author || 'Không rõ tác giả')}</div>
    <div class="book-actions">${opdsAction(e, i)}</div>
  </article>`;
}
function opdsAction(e, i) {
  if (e._busy) return `<button class="btn sm soft" disabled><span class="spinner" style="width:14px;height:14px"></span> Đang tải…</button>`;
  if (e.libraryId) return `<button class="btn sm soft" data-read="${i}">${icon('book', 15)} Đọc</button>`;
  return `<button class="btn sm" data-get="${i}">${icon('download', 15)} Tải về</button>`;
}

async function loadFeed(url, title, { push = true, replace = false } = {}) {
  const d = App.discover;
  d.loading = true;
  d.error = null;
  d.feed = null;
  d.entries = [];
  d.next = null;
  if (push) {
    if (replace) d.stack = d.stack.slice(0, 1);
    d.stack.push({ url, title });
  }
  renderDiscoverBody();
  try {
    const feed = await api.get('/api/opds/feed' + (url ? '?url=' + encodeURIComponent(url) : ''));
    d.feed = feed;
    d.entries = feed.entries;
    d.next = feed.next;
    if (d.stack.length) d.stack[d.stack.length - 1].title = d.stack[d.stack.length - 1].title || feed.title;
  } catch (e) {
    d.error = e.message;
    if (e.status === 401) {
      App.state = await refreshState();
    }
  }
  d.loading = false;
  renderDiscoverBody();
}

async function loadMore() {
  const d = App.discover;
  if (!d.next || d.loading) return;
  d.loading = true;
  setupMore();
  try {
    const feed = await api.get('/api/opds/feed?url=' + encodeURIComponent(d.next));
    const start = d.entries.length;
    d.entries = d.entries.concat(feed.entries);
    d.next = feed.next;
    // Cards carry their position in d.entries; append only the new ones.
    const grid = $('#opds-books');
    if (grid) grid.insertAdjacentHTML('beforeend', feed.entries.map((e, j) => e.kind === 'book' ? opdsBookCard(e, start + j) : '').join(''));
  } catch (e) {
    toast(e.message, { type: 'error' });
  }
  d.loading = false;
  setupMore();
}

function setupMore() {
  const d = App.discover;
  if (moreObserver) { moreObserver.disconnect(); moreObserver = null; }
  const more = $('#more');
  if (!more) return;
  if (!d.next) { more.remove(); return; }
  if (d.loading) { more.innerHTML = '<span class="spinner"></span> Đang tải thêm…'; return; }
  more.innerHTML = '<button class="btn" id="more-btn">Xem thêm</button>';
  $('#more-btn').onclick = loadMore;
  moreObserver = new IntersectionObserver((ents) => { if (ents.some((x) => x.isIntersecting)) loadMore(); }, { root: main, rootMargin: '400px' });
  moreObserver.observe(more);
}

async function renderDiscover() {
  if (!App.state.loggedIn) { renderLogin(); return; }
  const d = App.discover;
  main.innerHTML = `<div class="page">
    <header class="page-head">
      <div><h1>Khám phá</h1><p class="sub">Thư viện online ${esc(new URL(App.state.serverUrl).host)}</p></div>
      <form class="discover-search" id="opds-search">
        <div class="input-wrap">${icon('search', 17)}<input class="input" id="opds-q" placeholder="Tìm sách, tác giả…" value="${esc(d.query || '')}"></div>
        <button class="btn primary" type="submit">Tìm</button>
      </form>
    </header>
    <div id="discover-body"></div>
  </div>`;
  $('#opds-search').onsubmit = (e) => {
    e.preventDefault();
    const q = $('#opds-q').value.trim();
    if (!q) return;
    const tpl = d.feed?.search || (App.state.opdsUrl.split('?')[0] + '?q={searchTerms}');
    d.query = q;
    loadFeed(tpl.replace('{searchTerms}', encodeURIComponent(q)), `Tìm: “${q}”`, { replace: true });
  };
  if (!d.stack.length) { d.stack = [{ url: '', title: 'Trang chủ' }]; await loadFeed('', 'Trang chủ', { push: false }); }
  else renderDiscoverBody();
}

let moreObserver = null;
function renderDiscoverBody() {
  const body = $('#discover-body');
  if (!body) return;
  const d = App.discover;
  const crumbs = `<nav class="crumbs">${d.stack.map((s, i) => {
    const cur = i === d.stack.length - 1;
    return `${i ? `<span class="sep">${icon('right', 14)}</span>` : ''}<button class="${cur ? 'cur' : ''}" data-crumb="${i}">${i === 0 ? icon('home', 15) : ''}${esc(s.title || '…')}</button>`;
  }).join('')}</nav>`;

  if (d.loading && !d.feed) {
    body.innerHTML = crumbs + `<div class="book-grid">${Array.from({ length: 12 }, () => '<div><div class="skeleton sk-cover"></div><div class="skeleton sk-line"></div><div class="skeleton sk-line short"></div></div>').join('')}</div>`;
    wireDiscover();
    return;
  }
  if (d.error) {
    body.innerHTML = crumbs + `<div class="empty"><div class="empty-art" style="color:var(--danger);background:var(--danger-soft)">${icon('wifi', 38)}</div>
      <h2>Không tải được danh mục</h2><p>${esc(d.error)}</p>
      <div class="row"><button class="btn primary" id="retry">${icon('refresh', 16)} Thử lại</button></div></div>`;
    $('#retry').onclick = () => loadFeed(d.stack[d.stack.length - 1].url, null, { push: false });
    wireDiscover();
    return;
  }
  const navs = d.entries.map((e, i) => [e, i]).filter(([e]) => e.kind === 'nav');
  const books = d.entries.map((e, i) => [e, i]).filter(([e]) => e.kind === 'book');
  const collections = navs.filter(([e]) => /[?&]f=/.test(e.url));
  const others = navs.filter(([e]) => !/[?&]f=/.test(e.url));
  let html = crumbs;
  if (collections.length) html += `<div class="section-head" style="margin-top:0"><h2>Bộ sưu tập</h2></div><div class="tiles">${collections.map(([e, i]) => navTile(e, i)).join('')}</div>`;
  if (others.length) html += `<div class="section-head" ${collections.length ? '' : 'style="margin-top:0"'}><h2>${d.stack.length > 1 ? 'Danh mục' : 'Thể loại'}</h2></div><div class="tiles">${others.map(([e, i]) => navTile(e, i)).join('')}</div>`;
  if (books.length) {
    html += `<div class="section-head" ${navs.length ? '' : 'style="margin-top:0"'}><h2>${esc(d.feed?.title || 'Sách')}</h2></div>
      <div class="book-grid" id="opds-books">${books.map(([e, i]) => opdsBookCard(e, i)).join('')}</div>`;
  }
  if (!navs.length && !books.length) {
    html += `<div class="empty"><div class="empty-art">${icon('search', 38)}</div><h2>Không có kết quả</h2><p>Thử từ khóa khác hoặc quay lại trang chủ.</p></div>`;
  }
  if (d.next) html += '<div class="load-more" id="more"></div>';
  body.innerHTML = html;
  wireDiscover();
  setupMore();
}

function wireDiscover() {
  const body = $('#discover-body');
  body.onclick = (ev) => {
    const d = App.discover;
    const crumb = ev.target.closest('[data-crumb]');
    if (crumb) {
      const i = +crumb.dataset.crumb;
      if (i === d.stack.length - 1) return;
      d.stack = d.stack.slice(0, i + 1);
      if (i === 0) d.query = '';
      loadFeed(d.stack[i].url, d.stack[i].title, { push: false });
      return;
    }
    const nav = ev.target.closest('[data-nav]');
    if (nav) { const e = d.entries[+nav.dataset.nav]; loadFeed(e.url, e.title); main.scrollTop = 0; return; }
    const get = ev.target.closest('[data-get]');
    if (get) { ev.stopPropagation(); downloadEntry(+get.dataset.get); return; }
    const read = ev.target.closest('[data-read]');
    if (read) { ev.stopPropagation(); openBook(d.entries[+read.dataset.read].libraryId); return; }
    const card = ev.target.closest('.book[data-entry]');
    if (card) opdsDetail(+card.dataset.entry);
  };
}

function refreshEntry(i) {
  const card = $(`.book[data-entry="${i}"]`);
  const e = App.discover.entries[i];
  if (!card || !e) return;
  card.querySelector('.book-actions').innerHTML = opdsAction(e, i);
  card.querySelector('.cover-badges').innerHTML = e.libraryId ? '<span class="tag done">Đã có</span>' : '';
}

async function downloadEntry(i, after) {
  const e = App.discover.entries[i];
  if (!e || e._busy) return;
  const acq = e.acquisitions.find((a) => a.type.includes('epub')) || e.acquisitions[0];
  e._busy = true;
  refreshEntry(i);
  try {
    const book = await api.post('/api/opds/download', { url: acq.url, type: acq.type, title: e.title, author: e.author, cover: e.cover || e.thumb || '' });
    e.libraryId = book.id;
    await refreshBooks();
    toast(`Đã thêm “${book.title}” vào thư viện`, { type: 'success', action: { label: 'Đọc ngay', fn: () => openBook(book.id) } });
    after && after(book);
  } catch (err) {
    toast(err.message, { type: 'error' });
  }
  e._busy = false;
  refreshEntry(i);
}

function opdsDetail(i) {
  const e = App.discover.entries[i];
  const cover = e.cover || e.thumb ? coverImg('/api/opds/image?url=' + encodeURIComponent(e.cover || e.thumb), e.title, e.author) : placeholder(e.title, e.author);
  const fmt = e.acquisitions.map((a) => a.type.includes('epub') ? 'EPUB' : a.type.includes('text') ? 'TXT' : 'Tệp').join(', ');
  const m = openModal(`
    <button class="icon-btn close" data-r="x">${icon('x', 18)}</button>
    <div class="detail">
      <div class="cover-wrap">${cover}</div>
      <div class="detail-body">
        <h2>${esc(e.title)}</h2>
        <div class="by">${esc(e.author || 'Không rõ tác giả')}</div>
        <div class="detail-meta">
          ${e.language ? `<span>${icon('globe', 13)} ${esc(e.language)}</span>` : ''}
          <span>${icon('book', 13)} ${esc(fmt)}</span>
        </div>
        ${e.summary ? `<div class="detail-summary">${esc(e.summary)}</div>` : '<div class="detail-summary faint">Chưa có giới thiệu.</div>'}
        <div class="detail-actions" id="d-actions"></div>
      </div>
    </div>`, { wide: true });
  const paint = () => {
    const box = m.el.querySelector('#d-actions');
    if (!box) return;
    box.innerHTML = e.libraryId
      ? `<button class="btn primary lg" data-r="read">${icon('book', 18)} Đọc ngay</button>`
      : e._busy ? `<button class="btn primary lg" disabled><span class="spinner"></span> Đang tải về…</button>`
      : `<button class="btn primary lg" data-r="get">${icon('download', 18)} Tải về thư viện</button>
         <button class="btn lg" data-r="getread">Tải và đọc</button>`;
    const r = box.querySelector('[data-r=read]');
    if (r) r.onclick = () => { m.close(); openBook(e.libraryId); };
    const g = box.querySelector('[data-r=get]');
    if (g) g.onclick = () => { downloadEntry(i, paint); paint(); };
    const gr = box.querySelector('[data-r=getread]');
    if (gr) gr.onclick = () => { downloadEntry(i, (book) => { m.close(); openBook(book.id); }); paint(); };
  };
  m.el.querySelector('[data-r=x]').onclick = m.close;
  paint();
}

// ---------- inbox ----------
async function loadInbox() {
  const ib = App.inbox;
  ib.loading = true;
  ib.error = null;
  try {
    ib.status = await api.get('/api/inbox/status');
    ib.lastCheck = Date.now();
  } catch (e) {
    ib.error = e.message;
  }
  ib.loading = false;
  $('#dot-inbox').hidden = !(ib.status && ib.status.waiting && ib.status.waiting.length);
}

async function renderInbox() {
  const ib = App.inbox;
  paintInbox();
  if (!ib.status || Date.now() - (ib.lastCheck || 0) > 15000) {
    await loadInbox();
    if (App.view === 'inbox') paintInbox();
  }
}

function paintInbox() {
  const s = App.state, ib = App.inbox, st = ib.status;
  const host = new URL(s.serverUrl).host;
  const linked = st && st.linked;
  const pill = ib.loading && !st ? '<span class="pill off">Đang kiểm tra…</span>'
    : ib.error ? '<span class="pill warn">Mất kết nối</span>'
    : linked ? '<span class="pill ok">Đã liên kết</span>' : '<span class="pill off">Chưa liên kết</span>';
  const prettyId = (s.deviceId || '').match(/.{1,2}/g)?.join(':').toUpperCase() || '';

  let side = '';
  if (ib.loading && !st) {
    side = `<section class="card"><div class="skeleton" style="height:22px;width:40%"></div><div class="skeleton" style="height:14px;margin-top:16px"></div><div class="skeleton" style="height:14px;margin-top:10px;width:70%"></div></section>`;
  } else if (ib.error && !st) {
    side = `<section class="card"><div class="alert error">${icon('alert', 17)}<span>${esc(ib.error)}</span></div>
      <div class="card-actions"><button class="btn" data-ib="retry">${icon('refresh', 16)} Thử lại</button></div></section>`;
  } else if (!linked) {
    side = `<section class="card">
      <h2>Liên kết với tài khoản</h2>
      <p class="muted" style="margin-top:6px">Liên kết một lần, sau đó mọi sách bạn bấm “Gửi tới thiết bị” trên web sẽ tự về máy tính này.</p>
      <ol class="steps">
        <li><div><b>Mở thư viện trên web</b><span>Đăng nhập tại ${esc(host)}/library — <button class="linkish" data-ib="web">mở trang</button></span></div></li>
        <li><div><b>Vào mục “Thiết bị”</b><span>Bấm “Tạo mã liên kết” để nhận mã.</span></div></li>
        <li><div><b>Nhập mã vào đây</b><span>Máy tính sẽ hiện trong danh sách thiết bị của bạn.</span></div></li>
      </ol>
      ${st && st.badKey ? `<div class="alert error" style="margin-top:18px">${icon('alert', 17)}<span>Mã liên kết trước đó không đúng.</span></div>` : ''}
      <form class="link-form" id="link-form">
        <input class="input" id="link-key" placeholder="Mã liên kết" autocomplete="off" spellcheck="false" required>
        <button class="btn primary" type="submit" id="link-btn">${icon('link', 17)} Liên kết</button>
      </form>
    </section>`;
  } else {
    const waiting = st.waiting || [];
    side = `<section class="card">
      <div class="card-head"><h2>Hộp thư</h2><button class="icon-btn" data-ib="retry" title="Kiểm tra lại">${ib.loading ? '<span class="spinner" style="width:16px;height:16px"></span>' : icon('refresh', 17)}</button></div>
      <div class="sync-hero" style="margin-top:14px">
        <div class="sync-count">${waiting.length}</div>
        <div><b>${waiting.length ? 'sách đang chờ nhận' : 'Không có sách mới'}</b><div class="faint" style="font-size:13px">Kiểm tra lúc ${ib.lastCheck ? new Date(ib.lastCheck).toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit' }) : '—'}</div></div>
      </div>
      ${waiting.length ? `<div class="waiting">${waiting.map((w) => `<div class="file-row">${icon('book', 17)}<span class="name">${esc(w.name)}</span><span class="size">${esc(fmtSize(w.size))}</span></div>`).join('')}</div>` : ''}
      <div class="card-actions">
        <button class="btn primary lg" data-ib="sync" ${ib.syncing ? 'disabled' : ''}>${ib.syncing ? '<span class="spinner"></span> Đang nhận…' : `${icon('download', 18)} Nhận sách ngay`}</button>
      </div>
      <div style="margin-top:18px;padding-top:16px;border-top:1px solid var(--border)">
        <label class="switch"><input type="checkbox" id="auto-sync" ${s.autoSync ? 'checked' : ''}><span class="track"></span>
          <span><b style="font-weight:600">Tự động nhận sách</b><br><span class="faint" style="font-size:13px">Kiểm tra mỗi 5 phút khi ứng dụng đang mở</span></span></label>
      </div>
    </section>`;
  }

  const received = s.received || [];
  main.innerHTML = `<div class="page">
    <header class="page-head"><div><h1>Nhận sách qua Wi-Fi</h1>
      <p class="sub">Gửi sách từ ${esc(host)} tới máy tính này — giống như máy đọc X4.</p></div></header>
    <div class="inbox-grid">
      <section class="card wide">
        <div class="device">
          <div class="device-art">${icon('monitor', 30)}</div>
          <div class="device-body">
            <div class="label">Thiết bị này</div>
            <h2>${esc(s.deviceName)} <button class="icon-btn" data-ib="rename" title="Đổi tên">${icon('edit', 15)}</button></h2>
            <div class="mono">ID ${esc(prettyId)}</div>
          </div>
          ${pill}
        </div>
      </section>
      ${side}
      <section class="card">
        <h2>Đã nhận gần đây</h2>
        ${received.length ? `<div class="history">${received.map((r) => `
          <div class="history-row">
            <span class="ic-wrap ${r.status === 'error' ? 'err' : r.status === 'file' ? 'file' : ''}">${icon(r.status === 'error' ? 'alert' : r.status === 'file' ? 'folder' : 'check', 16)}</span>
            <span class="name" title="${esc(r.name)}">${esc(r.name)}</span>
            <time>${esc(fmtTime(r.at))}</time>
            ${r.bookId ? `<button class="btn sm ghost" data-open="${r.bookId}">Đọc</button>` : ''}
          </div>`).join('')}</div>`
        : `<p class="faint" style="margin-top:10px">Chưa nhận sách nào. Sách gửi tới sẽ hiện ở đây và trong Thư viện.</p>`}
      </section>
    </div>
  </div>`;

  main.querySelectorAll('[data-ib]').forEach((b) => b.onclick = () => inboxAction(b.dataset.ib));
  const form = $('#link-form');
  if (form) form.onsubmit = async (e) => {
    e.preventDefault();
    const btn = $('#link-btn');
    btn.disabled = true;
    btn.innerHTML = '<span class="spinner"></span> Đang liên kết…';
    try {
      App.inbox.status = await api.post('/api/inbox/link', { key: $('#link-key').value });
      App.inbox.lastCheck = Date.now();
      await refreshState();
      toast('Đã liên kết máy tính với tài khoản', { type: 'success' });
      startAutoSync();
    } catch (err) {
      toast(err.message, { type: 'error' });
    }
    paintInbox();
  };
  const auto = $('#auto-sync');
  if (auto) auto.onchange = async () => { App.state = await api.post('/api/settings', { autoSync: auto.checked }); startAutoSync(); };
}

async function inboxAction(kind) {
  if (kind === 'retry') { App.inbox.loading = true; paintInbox(); await loadInbox(); paintInbox(); }
  if (kind === 'web') api.post('/api/open-url', { url: App.state.serverUrl + '/library/' });
  if (kind === 'sync') await syncInbox(false);
  if (kind === 'rename') renameDevice();
}

function renameDevice() {
  const m = openModal(`
    <h2>Tên thiết bị</h2>
    <p class="modal-text">Tên hiển thị trong danh sách thiết bị trên web.</p>
    <form id="rn" style="margin-top:16px"><input class="input" id="rn-name" maxlength="40" value="${esc(App.state.deviceName)}" autofocus>
    <div class="modal-actions"><button type="button" class="btn ghost" data-r="no">Hủy</button><button class="btn primary">Lưu</button></div></form>`);
  m.el.querySelector('[data-r=no]').onclick = m.close;
  m.el.querySelector('#rn').onsubmit = async (e) => {
    e.preventDefault();
    App.state = await api.post('/api/settings', { deviceName: m.el.querySelector('#rn-name').value });
    m.close();
    if (App.view === 'inbox') { await loadInbox(); paintInbox(); }
    if (App.view === 'settings') renderSettings();
  };
}

async function syncInbox(silent) {
  const ib = App.inbox;
  if (ib.syncing) return;
  ib.syncing = true;
  if (App.view === 'inbox') paintInbox();
  try {
    const r = await api.post('/api/inbox/sync');
    const got = r.received.filter((x) => x.status !== 'error');
    await refreshState();
    if (got.length) {
      await refreshBooks();
      const one = got.length === 1 && got[0].bookId;
      toast(got.length === 1 ? `Đã nhận “${got[0].name}”` : `Đã nhận ${got.length} sách mới`, {
        type: 'success', timeout: 7000,
        action: one ? { label: 'Đọc', fn: () => openBook(got[0].bookId) } : { label: 'Xem', fn: () => { location.hash = '#/library'; } },
      });
      if (App.view === 'library') renderLibrary();
    } else if (!silent) {
      toast(r.failed ? 'Có sách nhận không thành công' : 'Không có sách mới', { type: r.failed ? 'error' : 'info' });
    }
    if (r.failed && silent) toast(`${r.failed} sách nhận không thành công`, { type: 'error' });
  } catch (e) {
    if (!silent) toast(e.message, { type: 'error' });
  }
  ib.syncing = false;
  await loadInbox();
  if (App.view === 'inbox') paintInbox();
}

let autoTimer = null;
function startAutoSync() {
  clearInterval(autoTimer);
  if (!App.state.autoSync || !App.state.hasKey) return;
  autoTimer = setInterval(() => syncInbox(true), 5 * 60 * 1000);
}

// ---------- settings ----------
async function renderSettings() {
  const s = App.state;
  const host = new URL(s.serverUrl).host;
  main.innerHTML = `<div class="page">
    <header class="page-head"><div><h1>Cài đặt</h1><p class="sub">Tùy chỉnh ứng dụng và kết nối</p></div></header>
    <div class="settings">
      <section class="card">
        <h3>Giao diện</h3>
        <div class="row-item"><div class="row-text"><b>Chủ đề</b><span>Màu nền của ứng dụng (khi đọc có thể chọn riêng)</span></div>
          <div class="seg" id="theme-seg">
            ${[['system', 'monitor', 'Hệ thống'], ['light', 'sun', 'Sáng'], ['dark', 'moon', 'Tối']].map(([k, ic, l]) =>
              `<button data-theme-opt="${k}" class="${s.theme === k ? 'on' : ''}">${icon(ic, 15)} ${l}</button>`).join('')}
          </div></div>
      </section>
      <section class="card">
        <h3>Thư viện online (OPDS)</h3>
        <div class="row-item"><div class="row-text"><b>${s.loggedIn ? esc(s.email) : 'Chưa đăng nhập'}</b><span>${esc(s.opdsUrl)}</span></div>
          ${s.loggedIn ? `<button class="btn danger" id="logout">${icon('logout', 16)} Đăng xuất</button>` : `<a class="btn primary" href="#/discover">Đăng nhập</a>`}</div>
      </section>
      <section class="card">
        <h3>Nhận sách qua Wi-Fi</h3>
        <div class="row-item"><div class="row-text"><b>Tên thiết bị</b><span>Hiển thị trên trang Thiết bị của ${esc(host)}</span></div>
          <button class="btn" id="rename">${icon('edit', 15)} ${esc(s.deviceName)}</button></div>
        <div class="row-item"><div class="row-text"><b>Tự động nhận sách</b><span>Kiểm tra hộp thư mỗi 5 phút</span></div>
          <label class="switch"><input type="checkbox" id="s-auto" ${s.autoSync ? 'checked' : ''}><span class="track"></span></label></div>
        <div class="row-item"><div class="row-text"><b>Đặt lại thiết bị</b><span>Tạo định danh mới và bỏ liên kết. Xóa thiết bị cũ trên web nếu không dùng nữa.</span></div>
          <button class="btn danger" id="reset-dev">${icon('refresh', 15)} Đặt lại</button></div>
      </section>
      <section class="card">
        <h3>Cập nhật</h3>
        <div class="row-item"><div class="row-text"><b>Phiên bản ${esc(s.version)}</b><span id="upd-status">${App.update && App.update.available ? `Đã có bản mới ${esc(App.update.latest)}` : 'Ứng dụng tự kiểm tra bản mới trên ' + esc(host)}</span></div>
          <button class="btn ${App.update && App.update.available ? 'primary' : ''}" id="upd-check">${icon(App.update && App.update.available ? 'upgrade' : 'refresh', 16)} ${App.update && App.update.available ? 'Cập nhật ngay' : 'Kiểm tra cập nhật'}</button></div>
        <div class="row-item"><div class="row-text"><b>Tự động kiểm tra</b><span>Báo khi có phiên bản mới lúc mở ứng dụng</span></div>
          <label class="switch"><input type="checkbox" id="s-upd" ${s.autoUpdate ? 'checked' : ''}><span class="track"></span></label></div>
      </section>
      <section class="card">
        <h3>Dữ liệu</h3>
        <div class="row-item"><div class="row-text"><b>Thư mục thư viện</b><span class="mono">${esc(s.dataDir)}</span></div>
          <button class="btn" id="open-folder">${icon('folder', 16)} Mở thư mục</button></div>
      </section>
      <section class="card">
        <h3>Phím tắt khi đọc</h3>
        <div class="kbd-list">
          <div>Trang sau <span><kbd>→</kbd> <kbd>Space</kbd></span></div>
          <div>Trang trước <span><kbd>←</kbd> <kbd>Shift</kbd>+<kbd>Space</kbd></span></div>
          <div>Mục lục <kbd>T</kbd></div>
          <div>Tìm trong sách <span><kbd>Ctrl</kbd>+<kbd>F</kbd></span></div>
          <div>Dấu trang <kbd>B</kbd></div>
          <div>Toàn màn hình <span><kbd>F11</kbd></span></div>
          <div>Cỡ chữ <span><kbd>Ctrl</kbd>+<kbd>+</kbd> / <kbd>−</kbd></span></div>
          <div>Về thư viện <kbd>Esc</kbd></div>
        </div>
      </section>
      <p class="faint" style="text-align:center;font-size:12.5px">Zenix CrossPoint Reader cho Windows · phiên bản ${esc(s.version)}</p>
    </div>
  </div>`;
  $$('#theme-seg button').forEach((b) => b.onclick = async () => {
    App.state = await api.post('/api/settings', { theme: b.dataset.themeOpt });
    applyTheme();
    $$('#theme-seg button').forEach((x) => x.classList.toggle('on', x === b));
  });
  const lo = $('#logout');
  if (lo) lo.onclick = async () => {
    if (!await confirmDialog({ title: 'Đăng xuất?', message: 'Sách đã tải vẫn còn trong thư viện trên máy.', confirm: 'Đăng xuất' })) return;
    App.state = await api.post('/api/opds/logout');
    App.discover = { stack: [], feed: null, entries: [], next: null, loading: false, error: null };
    renderAccount();
    renderSettings();
  };
  $('#rename').onclick = renameDevice;
  $('#s-auto').onchange = async (e) => { App.state = await api.post('/api/settings', { autoSync: e.target.checked }); startAutoSync(); };
  $('#open-folder').onclick = () => api.post('/api/open-folder');
  $('#upd-check').onclick = () => (App.update && App.update.available ? updateDialog() : checkUpdate(false));
  $('#s-upd').onchange = async (e) => { App.state = await api.post('/api/settings', { autoUpdate: e.target.checked }); };
  $('#reset-dev').onclick = async () => {
    if (!await confirmDialog({ title: 'Đặt lại thiết bị?', message: 'Máy tính sẽ nhận định danh mới và cần nhập lại mã liên kết. Sách trong thư viện không bị ảnh hưởng.', confirm: 'Đặt lại', danger: true })) return;
    App.state = await api.post('/api/inbox/reset');
    App.inbox = { status: null, loading: false, error: null, syncing: false, lastCheck: null };
    startAutoSync();
    toast('Đã đặt lại thiết bị', { type: 'success' });
    renderSettings();
  };
}

// ---------- global keys ----------
window.addEventListener('keydown', (e) => {
  if (Reader.isOpen) return;
  if (e.key === 'Escape' && modalStack.length) { modalStack[modalStack.length - 1](); return; }
  if (e.ctrlKey && e.key.toLowerCase() === 'o') { e.preventDefault(); $('#file-input').click(); }
  if (e.ctrlKey && e.key.toLowerCase() === 'f') {
    const q = $('#lib-q') || $('#opds-q');
    if (q) { e.preventDefault(); q.focus(); q.select(); }
  }
});

// ---------- start ----------
(async function start() {
  fillIcons();
  try {
    await refreshState();
  } catch (e) {
    main.innerHTML = `<div class="page"><div class="alert error">${esc(e.message)}</div></div>`;
    return;
  }
  applyTheme();
  await refreshBooks();
  await route();
  if (App.state.hasKey) {
    loadInbox();
    if (App.state.autoSync) setTimeout(() => syncInbox(true), 1500);
  }
  startAutoSync();
  if (App.state.autoUpdate) {
    setTimeout(() => checkUpdate(true), 4000);
    setInterval(() => checkUpdate(true), 6 * 3600 * 1000);
  }
})();
