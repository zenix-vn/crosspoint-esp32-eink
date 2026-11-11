'use strict';

// ---------- translating a book (English -> Vietnamese) ----------
// The work runs in the app (src/Translator.cs); this page picks the book and
// options and shows progress. Results land in the library.

const TR_PRESETS = {
  safe: { label: 'An toàn', hint: 'Chậm mà chắc, hiếm khi bị Google chặn. Nên dùng cho sách dài.', workers: 1, delay: 2.0, jitter: 0.8, batchChars: 3800, retries: 6 },
  balanced: { label: 'Cân bằng', hint: 'Nhanh gấp đôi, thỉnh thoảng phải tạm nghỉ khi Google giới hạn.', workers: 1, delay: 1.0, jitter: 0.4, batchChars: 4000, retries: 5 },
  fast: { label: 'Nhanh', hint: 'Hai luồng song song. Dễ gặp lỗi 429, hợp với sách ngắn.', workers: 2, delay: 0.4, jitter: 0.2, batchChars: 4000, retries: 4 },
};
const TR_FIELDS = [
  ['workers', 'Số luồng', 1, 4, 1],
  ['delay', 'Nghỉ giữa lượt (giây)', 0, 15, 0.5],
  ['jitter', 'Ngẫu nhiên ± (giây)', 0, 5, 0.2],
  ['batchChars', 'Ký tự mỗi lượt', 500, 4500, 250],
  ['retries', 'Thử lại tối đa', 1, 12, 1],
];

App.tr = { opts: null, st: null, seq: 0, log: [], timer: null, google: null, cached: {} };

const isEnglish = (b) => /^en\b/i.test(b.language || '');
const trBook = () => App.books.find((b) => b.id === App.tr.opts?.bookId);
const fmtClock = (s) => `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;

function presetOf(o) {
  for (const [k, p] of Object.entries(TR_PRESETS)) {
    if (p.workers === o.workers && p.delay === o.delay && p.jitter === o.jitter && p.batchChars === o.batchChars && p.retries === o.retries) return k;
  }
  return 'custom';
}

async function renderTranslate(arg) {
  const t = App.tr;
  if (!t.opts) t.opts = await api.get('/api/translate/options');
  if (arg) t.opts.bookId = arg;
  if (!trBook()) t.opts.bookId = (App.books.find(isEnglish) || App.books.find((b) => b.source !== 'translate') || {}).id || '';
  await pollTranslate(true);

  main.innerHTML = `<div class="page tr-page">
    <header class="page-head">
      <div><h1>Dịch sách</h1><p class="sub">Dịch sách tiếng Anh sang tiếng Việt bằng Google Dịch. Bản dịch được thêm thẳng vào thư viện.</p></div>
      <div class="head-actions"><button class="g-pill" id="tr-google" title="Gửi một câu thử tới Google Dịch"></button></div>
    </header>
    <div class="tr-layout">
      <div class="tr-main">
        <section class="card" id="tr-book"></section>
        <section class="card">
          <div class="tr-h"><h3>Bản xuất</h3><span class="faint">Chọn một hoặc cả hai</span></div>
          <div class="tr-kinds">
            ${trKind('vi', 'Tiếng Việt', 'Thay toàn bộ chữ tiếng Anh bằng bản dịch.',
              `<i class="l vi w9"></i><i class="l vi w7"></i><i class="l vi w8"></i><i class="l vi w5"></i>`)}
            ${trKind('bi', 'Song ngữ Anh – Việt', 'Mỗi đoạn tiếng Anh, ngay dưới là bản dịch in nghiêng. Hợp để học tiếng Anh.',
              `<i class="l en w9"></i><i class="l vi it w8"></i><i class="l en w7"></i><i class="l vi it w6"></i>`)}
          </div>
          <div class="tr-copy">
            <div class="row-item"><div class="row-text"><b>Lưu thêm bản sao ra thư mục</b><span>Để chép sang thẻ nhớ, gửi bạn bè hoặc lưu trữ</span></div>
              <label class="switch"><input type="checkbox" id="tr-save"><span class="track"></span></label></div>
            <div class="tr-dir" id="tr-dir">
              <button class="btn sm" id="tr-pick">${icon('folder', 15)} <span class="mono" id="tr-dir-path"></span></button>
              <label class="tr-check"><input type="checkbox" id="tr-orig"><span class="box">${icon('check', 12)}</span> Chép kèm bản gốc tiếng Anh</label>
            </div>
          </div>
        </section>
        <section class="card">
          <div class="tr-h"><h3>Tốc độ</h3>
            <div class="seg" id="tr-preset">${Object.entries(TR_PRESETS).map(([k, p]) => `<button data-p="${k}">${p.label}</button>`).join('')}<button data-p="custom">Tùy chỉnh</button></div></div>
          <p class="tr-hint" id="tr-hint"></p>
          <details class="tr-adv" id="tr-adv">
            <summary>${icon('settings', 15)} Tinh chỉnh nâng cao</summary>
            <div class="tr-fields">${TR_FIELDS.map(([k, l, min, max, step]) => `
              <label class="tr-field"><span>${l}</span><input class="input" type="number" data-f="${k}" min="${min}" max="${max}" step="${step}"></label>`).join('')}</div>
            <div class="row-item"><div class="row-text"><b>Dịch tiếp từ lần trước</b><span>Ghi nhớ đoạn đã dịch; dừng giữa chừng hay bị chặn vẫn không mất công</span></div>
              <label class="switch"><input type="checkbox" id="tr-resume"><span class="track"></span></label></div>
          </details>
        </section>
      </div>
      <aside class="tr-side">
        <section class="card tr-run" id="tr-run"></section>
        <section class="card tr-logcard">
          <div class="tr-h"><h3>Nhật ký</h3><button class="icon-btn" id="tr-clear" title="Xóa nhật ký">${icon('trash', 16)}</button></div>
          <div class="tr-log" id="tr-log"></div>
        </section>
      </aside>
    </div>
  </div>`;
  paintTrBook();
  paintTrOptions();
  paintGoogle();
  paintTrRun();
  paintTrLog(true);
  wireTranslate();
}

function trKind(k, title, text, lines) {
  return `<button class="tr-kind" data-kind="${k}">
    <span class="tr-tick">${icon('check', 13)}</span>
    <span class="tr-mock">${lines}</span>
    <span class="tr-kt"><b>${title}</b><span>${text}</span></span>
  </button>`;
}

function paintTrBook() {
  const box = $('#tr-book');
  if (!box) return;
  const b = trBook();
  if (!b) {
    box.innerHTML = `<div class="tr-nobook"><div class="empty-art">${icon('book', 34)}</div>
      <div><b>Thư viện chưa có sách</b><span>Thêm một cuốn EPUB tiếng Anh để bắt đầu dịch.</span></div>
      <button class="btn primary" id="tr-import">${icon('plus', 17)} Thêm sách</button></div>`;
    $('#tr-import').onclick = () => $('#file-input').click();
    return;
  }
  const cached = App.tr.cached[b.id];
  box.innerHTML = `<div class="tr-bookrow">
    <div class="tr-cover">${b.hasCover ? coverImg(`/api/library/${b.id}/cover`, b.title, b.author) : placeholder(b.title, b.author)}</div>
    <div class="tr-bookmeta">
      <span class="eyebrow">Sách cần dịch</span>
      <h2>${esc(b.title)}</h2>
      <div class="by">${esc(b.author || 'Không rõ tác giả')}</div>
      <div class="tr-chips">
        <span class="tr-tag ${isEnglish(b) ? 'ok' : b.language ? 'warn' : ''}">${icon('globe', 13)} ${b.language ? esc(b.language) : 'Chưa rõ ngôn ngữ'}</span>
        <span class="tr-tag">${icon('download', 13)} ${esc(fmtSize(b.fileSize))}</span>
        ${cached ? `<span class="tr-tag accent">${icon('refresh', 13)} Đã dịch sẵn ${cached} đoạn</span>` : ''}
      </div>
      ${b.language && !isEnglish(b) ? `<p class="tr-warn">${icon('alert', 14)} Sách này không ghi là tiếng Anh. Google vẫn dịch được, nhưng kết quả có thể không như ý.</p>` : ''}
    </div>
    <button class="btn" id="tr-change" ${App.tr.st?.running ? 'disabled' : ''}>${icon('refresh', 15)} Đổi sách</button>
  </div>`;
  $('#tr-change').onclick = pickTrBook;
  if (cached === undefined) {
    api.get('/api/translate/info?id=' + b.id).then((r) => { App.tr.cached[b.id] = r.cached; if (r.cached) paintTrBook(); }).catch(() => {});
  }
}

function pickTrBook() {
  const books = App.books.filter((b) => b.source !== 'translate')
    .sort((a, b) => (isEnglish(b) - isEnglish(a)) || ((b.lastOpened || b.added) - (a.lastOpened || a.added)));
  const m = openModal(`
    <button class="icon-btn close" data-r="x">${icon('x', 18)}</button>
    <h2>Chọn sách để dịch</h2>
    <div class="input-wrap search-box" style="margin:14px 0 12px">${icon('search', 17)}<input class="input" id="trp-q" placeholder="Tìm theo tên sách hoặc tác giả…"></div>
    <div class="trp-list" id="trp-list"></div>
    <div class="modal-actions"><button class="btn ghost" data-r="import">${icon('upload', 16)} Thêm sách từ máy</button></div>`, { wide: true });
  m.el.querySelector('[data-r=x]').onclick = m.close;
  m.el.querySelector('[data-r=import]').onclick = () => { m.close(); $('#file-input').click(); };
  const list = m.el.querySelector('#trp-list');
  const paint = (q) => {
    const f = fold(q.trim());
    const rows = f ? books.filter((b) => fold(b.title + ' ' + b.author).includes(f)) : books;
    list.innerHTML = rows.map((b) => `
      <button class="trp-row ${b.id === App.tr.opts.bookId ? 'on' : ''}" data-id="${b.id}">
        <span class="trp-cover">${b.hasCover ? coverImg(`/api/library/${b.id}/cover`, b.title, b.author) : placeholder(b.title, b.author)}</span>
        <span class="trp-text"><b>${esc(b.title)}</b><span>${esc(b.author || 'Không rõ tác giả')}</span></span>
        <span class="tr-tag ${isEnglish(b) ? 'ok' : ''}">${esc(b.language || '?')}</span>
      </button>`).join('') || '<div class="faint" style="padding:24px;text-align:center">Không có sách nào khớp.</div>';
  };
  paint('');
  m.el.querySelector('#trp-q').oninput = (e) => paint(e.target.value);
  setTimeout(() => m.el.querySelector('#trp-q').focus(), 50);
  list.onclick = (e) => {
    const r = e.target.closest('[data-id]');
    if (!r) return;
    App.tr.opts.bookId = r.dataset.id;
    history.replaceState(null, '', '#/translate/' + r.dataset.id);
    m.close();
    paintTrBook();
  };
}

function paintTrOptions() {
  const o = App.tr.opts;
  $$('.tr-kind').forEach((k) => k.classList.toggle('on', k.dataset.kind === 'vi' ? o.makeVi : o.makeBi));
  $('#tr-save').checked = o.saveCopy;
  $('#tr-orig').checked = o.copyOriginal;
  $('#tr-dir').classList.toggle('show', o.saveCopy);
  $('#tr-dir-path').textContent = o.outDir;
  $('#tr-resume').checked = o.resume;
  const p = presetOf(o);
  $$('#tr-preset button').forEach((b) => b.classList.toggle('on', b.dataset.p === p));
  $('#tr-hint').innerHTML = p === 'custom'
    ? `${icon('settings', 14)} Thông số tự chỉnh: ${o.workers} luồng, nghỉ ${o.delay}s ± ${o.jitter}s, ${o.batchChars} ký tự mỗi lượt.`
    : `${icon('info', 14)} ${TR_PRESETS[p].hint}`;
  $$('[data-f]').forEach((i) => { if (document.activeElement !== i) i.value = o[i.dataset.f]; });
  if (p === 'custom') $('#tr-adv').open = true;
  const run = $('#tr-start');
  if (run) run.disabled = !(o.makeVi || o.makeBi) || !trBook();
}

function paintGoogle() {
  const el = $('#tr-google');
  if (!el) return;
  const g = App.tr.google;
  const [cls, text] = !g ? ['', 'Kiểm tra Google Dịch'] : g.checking ? ['wait', 'Đang kiểm tra…']
    : g.ok ? ['ok', `Google Dịch sẵn sàng · “${g.text}”`] : [g.limited ? 'warn' : 'bad', g.limited ? 'Google đang chặn tạm thời' : 'Không kết nối được'];
  el.className = 'g-pill ' + cls;
  el.innerHTML = `<span class="dot"></span>${esc(text)}`;
  el.title = g && g.error ? g.error : 'Gửi một câu thử tới Google Dịch';
}

async function testGoogle() {
  App.tr.google = { checking: true };
  paintGoogle();
  try {
    App.tr.google = await api.post('/api/translate/test');
  } catch (e) {
    App.tr.google = { ok: false, error: e.message };
  }
  paintGoogle();
  if (App.tr.google.error) toast(App.tr.google.error, { type: 'error' });
}

function ring(pct, cls) {
  const r = 52, c = 2 * Math.PI * r;
  return `<svg class="tr-ring ${cls}" viewBox="0 0 120 120" aria-hidden="true">
    <circle cx="60" cy="60" r="${r}" class="bg"/>
    <circle cx="60" cy="60" r="${r}" class="fg" stroke-dasharray="${c}" stroke-dashoffset="${c * (1 - pct / 100)}"/></svg>`;
}

function paintTrRun() {
  const box = $('#tr-run');
  if (!box) return;
  const s = App.tr.st || {};
  const mine = s.bookId && s.bookId === App.tr.opts.bookId;
  const pct = s.chapters ? Math.round((s.chaptersDone / s.chapters) * 1000) / 10 : 0;
  const done = s.success !== null && s.success !== undefined && !s.running;
  let state = 'idle';
  if (s.running) state = s.paused ? 'paused' : 'running';
  else if (done && mine) state = s.success ? 'done' : 'warn';

  const head = {
    idle: ['Sẵn sàng dịch', 'Bấm Bắt đầu. Bạn vẫn đọc sách bình thường trong lúc chờ.'],
    running: [`Đang dịch… ${pct}%`, s.status],
    paused: ['Đang tạm dừng', 'Bấm Tiếp tục để dịch tiếp.'],
    done: ['Đã dịch xong', s.summary],
    warn: ['Chưa trọn vẹn', s.summary],
  }[state];
  const other = s.running && !mine;
  const stats = state === 'idle' ? '' : `
    <div class="tr-stats">
      <div><span>Chương</span><b>${s.chaptersDone} / ${s.chapters}</b></div>
      <div><span>Đoạn đã dịch</span><b>${(s.paras || 0).toLocaleString('vi-VN')}</b></div>
      <div class="${s.failed ? 'bad' : ''}"><span>Lỗi</span><b>${s.failed || 0}</b></div>
      <div><span>Thời gian</span><b>${fmtClock(s.elapsed || 0)}</b></div>
    </div>`;
  const outputs = state === 'done' || state === 'warn' ? (s.outputs || []).map((o) => `
    <div class="tr-out">
      <span class="tr-out-ic ${o.kind}">${o.kind === 'bi' ? 'EN<br>VI' : 'VI'}</span>
      <div class="tr-out-t"><b>${o.kind === 'bi' ? 'Bản song ngữ' : 'Bản tiếng Việt'}</b><span>${esc(o.title)}</span></div>
      <button class="btn sm primary" data-read="${o.bookId}">${icon('book', 14)} Đọc</button>
      ${o.file ? `<button class="icon-btn" data-reveal="${esc(o.file)}" title="Hiện trong thư mục">${icon('folder', 16)}</button>` : ''}
    </div>`).join('') : '';

  const buttons = s.running
    ? (other ? `<button class="btn block" data-go="${s.bookId}">${icon('eye', 16)} Xem tiến độ “${esc(s.title)}”</button>`
      : `<div class="tr-btns">
          <button class="btn lg" id="tr-pause">${icon(s.paused ? 'play' : 'pause', 17)} ${s.paused ? 'Tiếp tục' : 'Tạm dừng'}</button>
          <button class="btn lg danger" id="tr-stop">${icon('stop', 16)} Dừng</button></div>`)
    : `<button class="btn primary lg block" id="tr-start">${icon(state === 'warn' ? 'refresh' : 'translate', 18)} ${state === 'warn' ? 'Dịch lại phần còn thiếu' : state === 'done' ? 'Dịch lại' : 'Bắt đầu dịch'}</button>`;

  box.className = `card tr-run st-${state}`;
  box.innerHTML = `
    <div class="tr-ringwrap">${ring(state === 'idle' ? 0 : state === 'done' ? 100 : pct, state)}
      <div class="tr-ringlabel">${state === 'done' ? icon('check', 34) : state === 'idle' ? icon('translate', 30) : `<b>${Math.floor(pct)}<small>%</small></b>`}</div></div>
    <h3 class="tr-runh">${esc(other ? 'Đang dịch sách khác' : head[0])}</h3>
    <p class="tr-runp">${esc(other ? `“${s.title}” đang được dịch. Mỗi lần chỉ dịch một cuốn.` : head[1] || '')}</p>
    ${other ? '' : stats}
    ${outputs ? `<div class="tr-outs">${outputs}</div>` : ''}
    ${buttons}`;

  const start = $('#tr-start');
  if (start) {
    start.disabled = !(App.tr.opts.makeVi || App.tr.opts.makeBi) || !trBook();
    start.onclick = startTranslate;
  }
  const pause = $('#tr-pause');
  if (pause) pause.onclick = () => api.post('/api/translate/pause', { pause: !s.paused }).then(() => pollTranslate());
  const stop = $('#tr-stop');
  if (stop) stop.onclick = async () => {
    if (!await confirmDialog({ title: 'Dừng dịch?', message: 'Phần đã dịch được giữ lại. Lần sau bấm Bắt đầu, app dịch tiếp từ chỗ dừng.', confirm: 'Dừng', danger: true })) return;
    api.post('/api/translate/stop').then(() => pollTranslate());
  };
  box.onclick = (e) => {
    const r = e.target.closest('[data-read]');
    if (r) openBook(r.dataset.read);
    const v = e.target.closest('[data-reveal]');
    if (v) api.post('/api/translate/open-folder', { path: v.dataset.reveal });
    const g = e.target.closest('[data-go]');
    if (g) { App.tr.opts.bookId = g.dataset.go; renderTranslate(); }
  };
}

function paintTrLog(all) {
  const box = $('#tr-log');
  if (!box) return;
  const line = (l) => `<div class="ln ${l.level}"><time>${new Date(l.at).toLocaleTimeString('vi-VN', { hour12: false })}</time><span>${esc(l.text)}</span></div>`;
  if (all) box.innerHTML = App.tr.log.length ? App.tr.log.map(line).join('') : '<div class="tr-logempty">Chưa có hoạt động nào.</div>';
  box.scrollTop = box.scrollHeight;
}

function wireTranslate() {
  const o = App.tr.opts;
  $('#tr-google').onclick = testGoogle;
  $$('.tr-kind').forEach((k) => k.onclick = () => {
    if (k.dataset.kind === 'vi') o.makeVi = !o.makeVi; else o.makeBi = !o.makeBi;
    paintTrOptions();
  });
  $('#tr-save').onchange = (e) => { o.saveCopy = e.target.checked; paintTrOptions(); };
  $('#tr-orig').onchange = (e) => { o.copyOriginal = e.target.checked; };
  $('#tr-resume').onchange = (e) => { o.resume = e.target.checked; };
  $('#tr-pick').onclick = async () => {
    const r = await api.post('/api/translate/pick-folder', { path: o.outDir });
    if (r.path) { o.outDir = r.path; paintTrOptions(); }
  };
  $$('#tr-preset button').forEach((b) => b.onclick = () => {
    if (b.dataset.p === 'custom') { $('#tr-adv').open = true; return; }
    Object.assign(o, TR_PRESETS[b.dataset.p], { preset: b.dataset.p });
    delete o.label; delete o.hint;
    paintTrOptions();
  });
  $$('[data-f]').forEach((i) => i.oninput = () => {
    const v = parseFloat(i.value);
    if (!Number.isNaN(v)) { o[i.dataset.f] = v; paintTrOptions(); }
  });
  $('#tr-clear').onclick = async () => { await api.post('/api/translate/clear-log'); App.tr.log = []; paintTrLog(true); };
}

async function startTranslate() {
  const o = App.tr.opts;
  const btn = $('#tr-start');
  btn.disabled = true;
  try {
    const body = { ...o };
    TR_FIELDS.forEach(([k]) => { body[k] = Number(body[k]); });
    await api.post('/api/translate/start', body);
    delete App.tr.cached[o.bookId];
    await pollTranslate();
  } catch (e) {
    btn.disabled = false;
    toast(e.message, { type: 'error' });
  }
}

// Polls while a translation runs, on any page: the nav item shows the percentage and a
// toast says when it is done.
async function pollTranslate(once) {
  const t = App.tr;
  clearTimeout(t.timer);
  let s;
  try {
    s = await api.get('/api/translate/status?after=' + t.seq);
  } catch (_) {
    if (!once) t.timer = setTimeout(pollTranslate, 3000);
    return;
  }
  const was = t.st;
  t.st = s;
  if (s.seq < t.seq) t.log = []; // the log was cleared
  t.log.push(...s.log);
  if (t.log.length > 600) t.log.splice(0, t.log.length - 600);
  t.seq = s.seq;

  const badge = $('#badge-translate');
  if (badge) { badge.hidden = !s.running; badge.textContent = s.chapters ? Math.floor((s.chaptersDone / s.chapters) * 100) + '%' : '…'; }

  if (was && was.running && !s.running) {
    delete t.cached[s.bookId];
    await refreshBooks();
    const read = (s.outputs || []).find((x) => x.kind === 'bi') || (s.outputs || [])[0];
    toast(s.summary, {
      type: s.success ? 'success' : 'error', timeout: 10000,
      action: read && App.view !== 'translate' ? { label: 'Đọc', fn: () => openBook(read.bookId) } : null,
    });
  }
  if (App.view === 'translate' && !Reader.isOpen) {
    paintTrRun();
    if (s.log.length) {
      const box = $('#tr-log');
      if (box && !box.querySelector('.ln')) box.innerHTML = '';
      if (box) {
        box.insertAdjacentHTML('beforeend', s.log.map((l) => `<div class="ln ${l.level}"><time>${new Date(l.at).toLocaleTimeString('vi-VN', { hour12: false })}</time><span>${esc(l.text)}</span></div>`).join(''));
        box.scrollTop = box.scrollHeight;
      }
    }
    if (was && was.running !== s.running) paintTrBook();
  }
  if (s.running) t.timer = setTimeout(pollTranslate, App.view === 'translate' ? 800 : 2500);
}
