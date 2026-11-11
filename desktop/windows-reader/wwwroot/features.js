'use strict';

// ---------- sending to a reader ----------
// Lists the account's readers (not this PC) and sends with `send(deviceId)`.
function pickDevice({ title, text, send }) {
  const m = openModal(`
    <button class="icon-btn close" data-r="x">${icon('x', 18)}</button>
    <h2>${esc(title)}</h2>
    <p class="modal-text">${esc(text)}</p>
    <div class="dev-list" id="dev-list"><div class="dev-wait"><span class="spinner"></span> Đang tải danh sách máy đọc…</div></div>`);
  m.el.querySelector('[data-r=x]').onclick = m.close;
  const box = m.el.querySelector('#dev-list');
  const loginPrompt = (msg) => {
    box.innerHTML = `<div class="alert info">${icon('user', 17)}<span>${esc(msg)}</span></div>
      <div class="modal-actions"><button class="btn primary" data-r="login">Đăng nhập thư viện</button></div>`;
    box.querySelector('[data-r=login]').onclick = () => { m.close(); location.hash = '#/discover'; };
  };
  if (!App.state.loggedIn) { loginPrompt('Đăng nhập tài khoản thư viện để thấy các máy đọc của bạn.'); return; }
  api.get('/api/account/devices').then((r) => {
    if (!r.devices.length) {
      box.innerHTML = `<div class="dev-empty">${icon('wifi', 28)}<b>Tài khoản chưa có máy đọc nào</b>
        <span>Trên máy đọc X4: Thêm → Nhận sách, rồi nhập mã liên kết tạo ở trang Thiết bị trên web.</span>
        <button class="btn sm" data-r="web">${icon('external', 15)} Mở trang Thiết bị</button></div>`;
      box.querySelector('[data-r=web]').onclick = () => api.post('/api/open-url', { url: App.state.serverUrl + '/library/' });
      return;
    }
    box.innerHTML = r.devices.map((d) => `
      <div class="dev-row">
        <span class="dev-ic">${icon('book', 19)}</span>
        <div class="dev-body"><b>${esc(d.name)}</b>
          <span>${esc([d.model, d.firmware, d.lastSeen ? 'kết nối ' + fmtTime(d.lastSeen * 1000) : ''].filter(Boolean).join(' · '))}${d.waiting ? ` · <em>${d.waiting} tệp đang chờ</em>` : ''}</span></div>
        <button class="btn sm primary" data-to="${esc(d.id)}">${icon('send', 15)} Gửi</button>
      </div>`).join('');
    box.onclick = async (e) => {
      const b = e.target.closest('[data-to]');
      if (!b || b.disabled) return;
      b.disabled = true;
      b.innerHTML = '<span class="spinner" style="width:14px;height:14px"></span> Đang gửi';
      try {
        const res = await send(b.dataset.to);
        b.innerHTML = `${icon('check', 15)} Đã gửi`;
        b.classList.replace('primary', 'soft');
        toast(`Đã gửi tới ${res.device}. Trên máy: Thêm → Nhận sách.`, { type: 'success', timeout: 6000 });
      } catch (err) {
        b.disabled = false;
        b.innerHTML = `${icon('refresh', 15)} Thử lại`;
        toast(err.message, { type: 'error' });
      }
    };
  }).catch((e) => {
    if (e.status === 401) loginPrompt(e.message);
    else box.innerHTML = `<div class="alert error">${icon('alert', 17)}<span>${esc(e.message)}</span></div>`;
  });
}

function sendBookDialog(book) {
  pickDevice({
    title: 'Gửi tới máy đọc',
    text: `“${book.title}” sẽ chờ trong hộp thư của máy. Trên máy: Thêm → Nhận sách.`,
    send: (to) => api.post(`/api/library/${book.id}/send`, { to }),
  });
}

// ---------- pronunciation ----------
function speak(text) {
  try {
    const synth = window.speechSynthesis;
    synth.cancel();
    const u = new SpeechSynthesisUtterance(text);
    const voices = synth.getVoices();
    u.voice = voices.find((v) => v.lang === 'en-US') || voices.find((v) => v.lang.startsWith('en')) || null;
    u.lang = u.voice ? u.voice.lang : 'en-US';
    u.rate = 0.9;
    synth.speak(u);
  } catch (_) { /* no voice on this PC */ }
}

// ---------- saved words ----------
App.words = null;
App.wordQuery = '';

async function renderWords() {
  const s = App.state;
  if (!s.loggedIn) {
    main.innerHTML = `<div class="page">
      <header class="page-head"><div><h1>Từ vựng</h1><p class="sub">Từ bạn lưu khi đọc sách tiếng Anh</p></div></header>
      <div class="empty"><div class="empty-art">${icon('languages', 40)}</div>
        <h2>Đăng nhập để dùng Từ vựng</h2>
        <p>Từ điển và bộ từ của bạn nằm trong tài khoản thư viện, dùng chung với trang web và máy đọc X4.</p>
        <div class="row"><a class="btn primary" href="#/discover">${icon('user', 17)} Đăng nhập thư viện</a></div></div></div>`;
    return;
  }
  main.innerHTML = `<div class="page">
    <header class="page-head">
      <div><h1>Từ vựng của tôi</h1><p class="sub" id="w-sub">Đang tải…</p></div>
      <div class="head-actions">
        <div class="input-wrap search-box">${icon('search', 17)}<input class="input" id="w-q" placeholder="Tìm từ hoặc nghĩa…" value="${esc(App.wordQuery)}"></div>
        <button class="btn primary" id="w-send">${icon('send', 17)} Gửi tới máy đọc</button>
      </div>
    </header>
    <div class="alert info" style="margin-bottom:18px">${icon('info', 17)}<span>Khi đọc sách, <b>bấm đúp vào một từ tiếng Anh</b> để tra nghĩa và lưu. Bộ từ “Từ đã lưu” cũng có trong mục Từ vựng trên web và ôn được trên máy đọc X4.</span></div>
    <div class="words-grid" id="w-grid">${Array.from({ length: 6 }, () => '<div class="word-card"><div class="skeleton" style="height:20px;width:40%"></div><div class="skeleton sk-line"></div><div class="skeleton sk-line short"></div></div>').join('')}</div>
  </div>`;
  $('#w-q').oninput = (e) => { App.wordQuery = e.target.value; paintWords(); };
  $('#w-send').onclick = () => pickDevice({
    title: 'Gửi bộ từ tới máy đọc',
    text: 'Bộ từ “Từ đã lưu” sẽ vào máy đọc. Trên máy: Thêm → Nhận sách, rồi Cài đặt → Từ vựng → Chọn bộ từ.',
    send: (to) => api.post('/api/words/send', { to }),
  });
  try {
    const r = await api.get('/api/words');
    App.words = r.words.slice().reverse(); // newest first
  } catch (e) {
    $('#w-grid').innerHTML = `<div class="alert error" style="grid-column:1/-1">${icon('alert', 17)}<span>${esc(e.message)}</span></div>`;
    $('#w-sub').textContent = '';
    return;
  }
  paintWords();
}

function paintWords() {
  const grid = $('#w-grid');
  if (!grid || !App.words) return;
  const sub = $('#w-sub');
  if (sub) sub.textContent = App.words.length ? `${App.words.length} từ đã lưu` : 'Chưa có từ nào';
  const q = fold(App.wordQuery.trim());
  const list = q ? App.words.filter((w) => fold(w.word + ' ' + w.meaning).includes(q)) : App.words;
  if (!App.words.length) {
    grid.innerHTML = `<div class="empty" style="grid-column:1/-1"><div class="empty-art">${icon('languages', 40)}</div>
      <h2>Chưa có từ nào</h2><p>Mở một cuốn sách tiếng Anh, bấm đúp vào từ bạn chưa biết, rồi bấm “Lưu từ”.</p></div>`;
    return;
  }
  grid.innerHTML = list.length ? list.map((w) => `
    <article class="word-card">
      <div class="w-head">
        <b class="w-word">${esc(w.word)}</b>${w.ipa ? `<span class="ipa">${esc(w.ipa)}</span>` : ''}
        <span class="w-tools">
          <button class="icon-btn" data-say="${esc(w.word)}" title="Nghe phát âm">${icon('volume', 17)}</button>
          <button class="icon-btn" data-del="${esc(w.word)}" title="Xóa khỏi bộ từ">${icon('trash', 16)}</button>
        </span>
      </div>
      <div class="w-mean">${esc(w.meaning)}</div>
      ${w.example ? `<div class="w-ex">“${esc(w.example)}”</div>` : ''}
    </article>`).join('')
    : '<div class="faint" style="grid-column:1/-1;padding:30px 0;text-align:center">Không có từ nào khớp.</div>';
  grid.onclick = async (e) => {
    const say = e.target.closest('[data-say]');
    if (say) { speak(say.dataset.say); return; }
    const del = e.target.closest('[data-del]');
    if (!del) return;
    const word = del.dataset.del;
    try {
      await api.post('/api/words/delete', { word });
      App.words = App.words.filter((w) => w.word !== word);
      paintWords();
      toast(`Đã xóa “${word}”`);
    } catch (err) {
      toast(err.message, { type: 'error' });
    }
  };
}

// ---------- updates ----------
App.update = null;

async function checkUpdate(silent) {
  try {
    const r = await api.get('/api/update/check');
    App.update = r;
    $('#dot-update').hidden = !r.available;
    if (r.available) {
      if (silent) toast(`Đã có phiên bản mới ${r.latest}`, { action: { label: 'Xem', fn: updateDialog }, timeout: 12000 });
      else updateDialog();
    } else if (!silent) {
      toast(r.latest ? `Bạn đang dùng bản mới nhất (${r.current})` : 'Chưa có bản phát hành nào trên máy chủ', { type: 'success' });
    }
    if (App.view === 'settings' && !silent) renderSettings();
    return r;
  } catch (e) {
    if (!silent) toast(e.message, { type: 'error' });
    return null;
  }
}

function updateDialog() {
  const u = App.update;
  if (!u || !u.available) return;
  const m = openModal(`
    <button class="icon-btn close" data-r="x">${icon('x', 18)}</button>
    <div class="upd-head"><span class="upd-ic">${icon('upgrade', 26)}</span>
      <div><h2>Có phiên bản mới</h2><p class="modal-text" style="margin-top:2px">${esc(u.current)} → <b>${esc(u.latest)}</b>${u.size ? ' · ' + esc(fmtSize(u.size)) : ''}</p></div></div>
    ${u.notes ? `<div class="upd-notes">${esc(u.notes)}</div>` : ''}
    <p class="faint" style="margin-top:14px;font-size:13px">Ứng dụng sẽ đóng lại, cài bản mới rồi tự mở lại. Thư viện và tiến độ đọc được giữ nguyên.</p>
    <div class="modal-actions">
      <button class="btn ghost" data-r="later">Để sau</button>
      <button class="btn primary" data-r="go">${icon('download', 17)} Cập nhật ngay</button>
    </div>`);
  m.el.querySelector('[data-r=x]').onclick = m.close;
  m.el.querySelector('[data-r=later]').onclick = m.close;
  const go = m.el.querySelector('[data-r=go]');
  go.onclick = async () => {
    go.disabled = true;
    go.innerHTML = '<span class="spinner"></span> Đang tải bản cập nhật…';
    try {
      await api.post('/api/update/install');
      go.innerHTML = '<span class="spinner"></span> Đang cài đặt…';
    } catch (e) {
      go.disabled = false;
      go.innerHTML = `${icon('refresh', 17)} Thử lại`;
      toast(e.message, { type: 'error' });
    }
  };
}
