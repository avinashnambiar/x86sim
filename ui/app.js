'use strict';

const $ = (id) => document.getElementById(id);

const S = {
  st: null,            // latest machine state
  prev: null,          // state before the last action (for change highlighting)
  follow: true,        // keep the disassembly centred on RIP
  view: null,          // disassembly start when not following
  memExpr: 'rsp',
  memBase: null,       // BigInt
  memPrev: null,       // {base, bytes} from the previous refresh
  running: false,
  pause: false,
  outLen: 0,
  tab: 'console',
  showXmm: false,
  symbols: null,
  resumeOnInput: false,
  announced: '',
};

const GPR16 = ['ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp'];
const FLAGS = [['CF', 0], ['PF', 2], ['AF', 4], ['ZF', 6], ['SF', 7], ['TF', 8], ['IF', 9], ['DF', 10], ['OF', 11]];

/* ---------- helpers ---------- */

async function api(path, opts) {
  const r = await fetch(path, opts);
  const j = await r.json();
  if (j.error) throw new Error(j.error);
  return j;
}
const post = (path, body) => api(path, { method: 'POST', body: body ?? '' });

const big = (s) => (s === null || s === undefined ? null : BigInt(s));
const hex = (v, w = 0) => v.toString(16).padStart(w, '0');
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => t.classList.remove('show'), 2600);
}

/* Hex digits for addresses: 64-bit user space fits in 48 bits. */
function width() {
  return S.st ? (S.st.bits === 64 ? 12 : S.st.bits === 32 ? 8 : 4) : 12;
}

function regDisplay(name, value) {
  const bits = S.st.bits;
  if (bits === 64) return { name, value, digits: 16 };
  const mask = bits === 32 ? 0xffffffffn : 0xffffn;
  const n = bits === 32 ? 'e' + name.slice(1) : name.slice(1);
  return { name: n, value: value & mask, digits: bits === 32 ? 8 : 4 };
}

function regValue(name) {
  if (!S.st) return null;
  name = name.toLowerCase();
  if (name === 'rip' || name === 'eip' || name === 'ip') return big(S.st.rip);
  for (const [n, v] of S.st.regs) {
    const val = big(v);
    if (n === name) return val;
    if ('e' + n.slice(1) === name) return val & 0xffffffffn;
    if (GPR16.includes(n.slice(1)) && n.slice(1) === name) return val & 0xffffn;
  }
  return null;
}

/* Resolve an address expression: register, hex, symbol[+off]. */
function resolveLocal(expr) {
  const e = expr.trim();
  const m = e.match(/^([a-z0-9]+)\s*([+-])\s*(0x[0-9a-f]+|\d+)$/i);
  if (m) {
    const base = regValue(m[1]);
    if (base !== null) {
      const off = BigInt(m[3]);
      return m[2] === '+' ? base + off : base - off;
    }
  }
  return regValue(e);
}

/* ---------- rendering ---------- */

function renderHeader(st) {
  $('program').textContent = st.program.join(' ');
  $('program').title = st.program.join('\n');
  if (st.sys) {
    const names = { real: 'real mode', vm86: 'virtual-8086', protected: '32-bit protected', compat: 'compatibility mode', long: '64-bit long mode' };
    $('mode').textContent = `PC · ${names[st.sys.mode] || st.sys.mode} · CPL ${st.sys.cpl}${st.sys.paging ? ' · paging' : ''}`;
  } else {
    $('mode').textContent = `${st.bits === 16 ? '16-bit real mode' : st.bits + '-bit'} · ${st.os}`;
  }
  $('tab-btn-screen').classList.toggle('hidden', st.os !== 'pc');
  $('tab-btn-cache').classList.toggle('hidden', !st.cache);
  renderCpus(st);
  if (st.os === 'pc' && !S.screenShown) {
    S.screenShown = true;
    selectTab('screen');
  }
  const state = S.running ? 'running' : st.state;
  const pill = $('state');
  pill.className = 'pill ' + state;
  pill.textContent = { ready: 'paused', running: 'running', exited: 'exited', fault: 'fault', halted: 'halted', wait_input: 'needs input' }[state] || state;
  const txt = $('status-text');
  txt.textContent = S.running ? '' : st.status;
  txt.className = 'status-text' + (st.state === 'fault' ? ' bad' : '');
  $('icount').textContent = Number(st.icount).toLocaleString();
  $('where').textContent = st.where ? st.where : '';
  $('sys-count').textContent = st.syscalls ? `(${st.syscalls})` : '';
  const done = st.state === 'exited' || st.state === 'fault' || st.state === 'halted';
  for (const id of ['btn-step', 'btn-next', 'btn-finish', 'btn-continue']) $(id).disabled = S.running || done;
  $('btn-pause').disabled = !S.running;
  $('btn-reset').disabled = S.running;
  $('btn-load').disabled = S.running;
  $('input-form').classList.toggle('waiting', st.state === 'wait_input');
}

/* CPU switcher: shown with more than one CPU; the registers, disassembly,
 * stack and memory views follow the selected CPU. Up to 8 CPUs get buttons;
 * beyond that a drop-down plus a running/halted summary. */
const CPU_BUTTONS_MAX = 8;
function cpuTitle(c, i) {
  return `cpu${i}: ${c.state}, ${Number(c.icount).toLocaleString()} instructions`;
}
function renderCpus(st) {
  const box = $('cpu-select');
  const cpus = st.cpus || [];
  box.classList.toggle('hidden', cpus.length < 2);
  if (cpus.length < 2) return;
  if (cpus.length <= CPU_BUTTONS_MAX) {
    box.innerHTML = cpus.map((c, i) =>
      `<button class="cpu-btn${i === st.view_cpu ? ' on' : ''}" data-cpu="${i}" role="tab" aria-selected="${i === st.view_cpu}" title="${esc(cpuTitle(c, i))}"><span class="dot ${c.state}"></span>cpu${i}</button>`).join('');
    return;
  }
  let sel = box.querySelector('select');
  if (!sel || sel.options.length !== cpus.length) { /* rebuilt only when the count changes, so an open list stays open */
    box.innerHTML = `<span class="dot" id="cpu-dot"></span><select id="cpu-pick" aria-label="CPU shown"></select><span class="cpu-sum" id="cpu-sum"></span>`;
    sel = box.querySelector('select');
    sel.innerHTML = cpus.map((_, i) => `<option value="${i}">cpu${i}</option>`).join('');
  }
  const count = {};
  cpus.forEach((c, i) => {
    count[c.state] = (count[c.state] || 0) + 1;
    const o = sel.options[i];
    o.textContent = `cpu${i} \u00b7 ${c.state}`;
    o.title = cpuTitle(c, i);
  });
  if (document.activeElement !== sel) sel.value = String(st.view_cpu);
  $('cpu-dot').className = 'dot ' + (cpus[st.view_cpu] || {}).state;
  $('cpu-sum').textContent = `${cpus.length} CPUs: ` + Object.entries(count).map(([k, v]) => `${v} ${k}`).join(', ');
}
async function selectCpu(n) {
  if (S.running) return;
  try {
    S.follow = true;
    const st = await post('/api/cpu?n=' + n);
    S.prev = null;
    await refresh(st);
  } catch (err) {
    toast(err.message);
  }
}
$('cpu-select').addEventListener('click', (e) => {
  const b = e.target.closest('[data-cpu]');
  if (b) selectCpu(b.dataset.cpu);
});
$('cpu-select').addEventListener('change', (e) => {
  if (e.target.id === 'cpu-pick') selectCpu(e.target.value);
});

/* ---------- caches ---------- */

const MESI_CLASS = { M: 'st-m', E: 'st-e', S: 'st-s', I: 'st-i' };

function pct(a, b) { return b ? (100 * a / b).toFixed(2) + '%' : '—'; }
function num(n) { return Number(n).toLocaleString(); }

async function renderCache() {
  const addr = $('line-addr').value.trim();
  const res = await api('/api/cache?log=60' + (addr ? '&addr=' + encodeURIComponent(addr) : ''));
  if (!res.enabled) {
    $('cache-table').innerHTML = '<tr><td class="muted">Caches are not simulated (start with --cache).</td></tr>';
    return;
  }
  let html = '<tr><th>cache</th><th>geometry</th><th>hit rate</th><th>accesses</th><th>misses</th><th>evictions</th><th>writebacks</th><th title="copies invalidated by other caches or by L2 eviction">invalidated</th><th title="modified lines supplied to another cache">interventions</th><th title="S to M through BusUpgr">upgrades</th><th>lines (M E S I)</th></tr>';
  for (const c of res.caches) {
    const acc = c.reads + c.writes + c.fetches;
    const total = c.sets * c.ways;
    const bar = ['M', 'E', 'S', 'I'].map((k) => c.lines[k] ? `<span class="${MESI_CLASS[k]}" style="width:${(100 * c.lines[k] / total).toFixed(2)}%" title="${k}: ${num(c.lines[k])} lines"></span>` : '').join('');
    html += `<tr><td>${esc(c.name)}</td><td class="muted">${c.sets}×${c.ways}</td><td>${pct(c.hits, acc)}</td><td>${num(acc)}</td><td>${num(c.misses)}</td><td>${num(c.evictions)}</td><td>${num(c.writebacks)}</td><td>${num(c.invalidations)}</td><td>${num(c.interventions)}</td><td>${num(c.upgrades)}</td><td><div class="mesi-bar">${bar}</div><span class="muted">${c.lines.M} ${c.lines.E} ${c.lines.S} ${c.lines.I}</span></td></tr>`;
  }
  $('cache-table').innerHTML = html;
  const b = res.bus;
  $('cache-bus').innerHTML = `<b>bus</b> BusRd ${num(b.BusRd)} · BusRdX ${num(b.BusRdX)} · BusUpgr ${num(b.BusUpgr)} · BusWB ${num(b.BusWB)} · cache-to-cache ${num(b.c2c)} &nbsp; <b>memory</b> ${num(b.mem_reads)} line reads · ${num(b.mem_writes)} line writes · ${num(b.uncached)} uncached`;
  if (res.line) {
    const l = res.line;
    /* past 16 units (8 cores) only the units holding the line are listed */
    const many = l.states.length > 17;
    const shown = many ? l.states.filter(([n, st]) => n === 'L2' || st !== 'I') : l.states;
    const badges = shown.map(([n, st]) => {
      const k = st === 'dirty' ? 'M' : st === 'clean' ? 'E' : st;
      return `<div class="line-unit"><span>${esc(n)}</span><span class="state ${MESI_CLASS[k] || ''}">${esc(st)}</span></div>`;
    }).join('') + (many ? `<div class="muted">${l.states.length - shown.length} other L1s: I</div>` : '');
    const data = l.data.replace(/(..)/g, '$1 ').trim().replace(/((?:.. ){15}..) /g, '$1\n');
    $('line-view').className = 'line-view';
    $('line-view').innerHTML = `<div class="muted">line ${esc(l.addr)} · L1 set ${l.l1_set} · L2 set ${l.l2_set}${l.cacheable ? '' : ' · uncacheable'}</div>${badges}<pre class="line-data">${data}</pre>`;
  }
  $('cache-events').textContent = `(${num(res.events)})`;
  $('cache-log').innerHTML = res.log.length ? res.log.map((x) => `<div>${esc(x)}</div>`).join('') : '<div class="muted">no bus traffic yet</div>';
  const body = $('tab-cache');
  if (S.cacheStick !== false) $('cache-log').lastElementChild?.scrollIntoView({ block: 'nearest' });
  void body;
}
$('line-form').addEventListener('submit', (e) => { e.preventDefault(); renderTab(); });

function fmtValue(v, digits) {
  const s = hex(v, digits);
  const nz = s.search(/[^0]/);
  const cut = nz === -1 ? s.length - 1 : nz;
  return `<span class="z">${s.slice(0, cut)}</span>${s.slice(cut)}`;
}

function renderRegs(st) {
  const prev = S.prev;
  const regs = st.bits === 64 ? st.regs : st.regs.slice(0, 8);
  let html = '';
  for (let i = 0; i < regs.length; i++) {
    const [n, v] = regs[i];
    const d = regDisplay(n, big(v));
    const changed = prev && prev.regs[i][1] !== v;
    html += `<div class="reg${changed ? ' changed' : ''}" data-reg="${n}"><span class="rn">${d.name}</span><span class="rv">${fmtValue(d.value, d.digits)}</span></div>`;
  }
  const rip = big(st.rip);
  const ripChanged = prev && prev.rip !== st.rip;
  const ripName = st.bits === 64 ? 'rip' : st.bits === 32 ? 'eip' : 'ip';
  const ripDigits = st.bits === 64 ? 16 : st.bits === 32 ? 8 : 4;
  html += `<div class="reg rip${ripChanged ? ' changed' : ''}" data-reg="rip"><span class="rn">${ripName}</span><span class="rv">${fmtValue(rip, ripDigits)}</span></div>`;
  $('regs').innerHTML = html;

  const fl = big(st.rflags), pfl = prev ? big(prev.rflags) : fl;
  $('flags').innerHTML = FLAGS.map(([n, bit]) => {
    const on = (fl >> BigInt(bit)) & 1n, was = (pfl >> BigInt(bit)) & 1n;
    return `<span class="flag${on ? ' on' : ''}${on !== was ? ' changed' : ''}" data-bit="${bit}" title="${n} (click to toggle)">${n}</span>`;
  }).join('') + `<span class="flag" style="cursor:default;border:none">0x${hex(fl)}</span>`;

  $('segs').innerHTML = st.segs
    .filter(([n, sel, base]) => st.bits === 16 || sel || big(base))
    .map(([n, sel, base]) => `<span><b>${n}</b> ${hex(sel, 4)}${big(base) ? ' <b>base</b> ' + hex(big(base)) : ''}</span>`)
    .join('');

  const sr = $('sysregs');
  sr.classList.toggle('hidden', !st.sys);
  if (st.sys) {
    sr.innerHTML = ['cr0', 'cr2', 'cr3', 'cr4', 'efer'].map((n) => {
      const changed = prev && prev.sys && prev.sys[n] !== st.sys[n];
      return `<span class="${changed ? 'changed' : ''}"><b>${n}</b> ${hex(big(st.sys[n]))}</span>`;
    }).join('') + `<span><b>time</b> ${(Number(st.sys.time_ns) / 1e9).toFixed(3)} s</span>`;
  }

  const xmm = $('xmm');
  xmm.classList.toggle('hidden', !S.showXmm);
  if (S.showXmm) {
    xmm.innerHTML = st.xmm.map((v, i) => {
      const changed = prev && prev.xmm[i] !== v;
      const pretty = v.replace(/(.{8})/g, '$1 ').trim();
      return `<div><span>xmm${i}</span><span class="${changed ? 'changed' : ''}">${pretty}</span></div>`;
    }).join('');
  }
}

function highlight(text) {
  let s = esc(text);
  let comment = '';
  const ci = s.indexOf('    # ');
  if (ci >= 0) {
    comment = s.slice(ci);
    s = s.slice(0, ci);
  }
  s = s.replace(/^((?:lock |rep |repe |repne )?)([a-z0-9]+(?: far)?)/, '$1<span class="mn">$2</span>');
  s = s.replace(/&lt;([^&]+)&gt;/g, '<span class="sy">&lt;$1&gt;</span>');
  s = s.replace(/\b(0x[0-9a-f]+)\b/g, '<span class="nm">$1</span>');
  if (comment) comment = `<span class="cm">${comment.replace(/&lt;([^&]+)&gt;/g, '&lt;<span class="sy">$1</span>&gt;')}</span>`;
  return s + comment;
}

async function renderDisasm() {
  const count = 80;
  let q = `count=${count}`;
  if (!S.follow && S.view !== null) q += `&addr=0x${hex(S.view)}`;
  let res;
  try {
    res = await api('/api/disasm?' + q);
  } catch (e) {
    return toast(e.message);
  }
  const rip = big(S.st.rip);
  const w = width();
  let html = '';
  for (const l of res.lines) {
    const a = big(l.addr);
    if (l.label) html += `<div class="dlabel">${esc(l.label)}</div>`;
    let text = highlight(l.text);
    if (l.target) {
      text = text.replace(`<span class="nm">${l.target}</span>`, `<a class="jump" data-target="${l.target}">${l.target}</a>`);
    }
    html += `<div class="dline${a === rip ? ' cur' : ''}" data-addr="${l.addr}">` +
      `<span class="gutter${l.bp ? ' bp' : ''}" title="Toggle breakpoint">●</span>` +
      `<span class="arrow">${a === rip ? '▶' : ''}</span>` +
      `<span class="addr">${hex(a, w)}</span>` +
      `<span class="bytes">${l.bytes}</span>` +
      `<span class="text">${text}</span></div>`;
  }
  const box = $('disasm');
  box.innerHTML = html;
  if (S.follow) {
    const cur = box.querySelector('.dline.cur');
    if (cur) {
      const top = cur.offsetTop - box.offsetTop, h = box.clientHeight;
      if (top < box.scrollTop + 20 || top > box.scrollTop + h - 40) box.scrollTop = top - h / 3;
    }
  } else {
    box.scrollTop = 0;
  }
}

async function renderStack() {
  try {
    const res = await api('/api/stack?count=32');
    const w = res.width * 2;
    $('stack').innerHTML = res.entries.map((e, i) => {
      const mark = i === 0 ? 'rsp' : e.rbp ? 'rbp' : '';
      const v = e.value === null ? '<span class="cm">unmapped</span>' : hex(big(e.value), w);
      return `<div class="srow${i === 0 ? ' top' : ''}"><span class="mark">${mark}</span>` +
        `<span><span class="sa">${hex(big(e.addr), width())}</span>  <span class="sv">${v}</span></span>` +
        `<span class="ss">${e.sym ? esc('<' + e.sym + '>') : ''}</span></div>`;
    }).join('');
  } catch (e) {
    toast(e.message);
  }
}

async function renderMem() {
  let base = resolveLocal(S.memExpr);
  let q;
  if (base !== null) q = `addr=0x${hex(base)}`;
  else q = `addr=${encodeURIComponent(S.memExpr)}`;
  try {
    const res = await api(`/api/mem?${q}&len=256`);
    base = big(res.addr);
    S.memBase = base;
    const prev = S.memPrev && S.memPrev.base === base ? S.memPrev.bytes : null;
    /* 16 bytes per row when the panel is wide enough, otherwise 8 */
    const charW = 7.3, need16 = (width() + 2 + 16 * 3 + 2 + 16 + 4) * charW;
    const per = $('mem').clientWidth >= need16 ? 16 : 8;
    let html = '';
    for (let row = 0; row < 256 / per; row++) {
      let bytes = '', chars = '';
      for (let i = 0; i < per; i++) {
        const k = row * per + i, b = res.bytes[k];
        const cls = b < 0 ? 'u' : prev && prev[k] !== b ? 'changed' : b === 0 ? 'zero' : '';
        bytes += `<span class="${cls}">${b < 0 ? '??' : hex(b, 2)}</span>${i === 7 && per === 16 ? '  ' : ' '}`;
        chars += b >= 32 && b < 127 ? esc(String.fromCharCode(b)) : '.';
      }
      html += `<div class="mrow"><span class="ma">${hex(base + BigInt(row * per), width())}</span><span class="mb">${bytes}</span><span class="mc">${chars}</span></div>`;
    }
    $('mem').innerHTML = html;
    S.memPrev = { base, bytes: res.bytes };
  } catch (e) {
    $('mem').innerHTML = `<div class="mrow cm">${esc(e.message)}</div>`;
  }
}

async function renderOutput(st) {
  if (st.out_len < S.outLen) {
    S.outLen = 0;
    $('console').textContent = '';
  }
  const con = $('console');
  if (st.out_len > S.outLen) {
    const res = await api(`/api/output?from=${S.outLen}`);
    con.appendChild(document.createTextNode(res.text));
    S.outLen = res.len;
  }
  const key = `${st.state}:${st.status}`;
  if ((st.state === 'exited' || st.state === 'fault' || st.state === 'halted') && S.announced !== key) {
    S.announced = key;
    const span = document.createElement('span');
    span.className = st.state === 'fault' ? 'err' : 'exit';
    span.textContent = `${con.textContent && !con.textContent.endsWith('\n') ? '\n' : ''}[${st.status}]\n`;
    con.appendChild(span);
  }
  const body = $('tab-console');
  body.scrollTop = body.scrollHeight;
}

/* ---------- PC screen ---------- */

const VGA_COLORS = ['#000000', '#0000aa', '#00aa00', '#00aaaa', '#aa0000', '#aa00aa', '#aa5500', '#aaaaaa',
  '#555555', '#5555ff', '#55ff55', '#55ffff', '#ff5555', '#ff55ff', '#ffff55', '#ffffff'];

function hexBytes(h) {
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.substr(i * 2, 2), 16);
  return out;
}

async function renderScreen() {
  if (S.tab !== 'screen' || !S.st || S.st.os !== 'pc') return;
  let res;
  try {
    res = await api('/api/screen');
  } catch (e) {
    return;
  }
  const pre = $('screen'), cv = $('screen-gfx');
  if (res.mode === 19) {
    pre.classList.add('hidden');
    cv.classList.remove('hidden');
    const px = hexBytes(res.pixels), pal = hexBytes(res.palette);
    const ctx = cv.getContext('2d');
    const img = ctx.createImageData(320, 200);
    for (let i = 0; i < 64000; i++) {
      const c = px[i] * 3;
      img.data[i * 4] = pal[c] * 4.05;
      img.data[i * 4 + 1] = pal[c + 1] * 4.05;
      img.data[i * 4 + 2] = pal[c + 2] * 4.05;
      img.data[i * 4 + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
    return;
  }
  cv.classList.add('hidden');
  pre.classList.remove('hidden');
  const cells = hexBytes(res.cells);
  let html = '';
  for (let r = 0; r < res.rows; r++) {
    let run = '', attr = -1;
    const flush = () => {
      if (run) html += `<span style="color:${VGA_COLORS[attr & 15]};background:${VGA_COLORS[(attr >> 4) & 7]}">${run}</span>`;
      run = '';
    };
    for (let c = 0; c < res.cols; c++) {
      const i = r * res.cols + c;
      let ch = cells[i * 2], a = cells[i * 2 + 1];
      let t = ch < 32 || ch > 126 ? (ch === 0 ? ' ' : String.fromCharCode(CP437[ch] || 0x2022)) : String.fromCharCode(ch);
      t = esc(t);
      if (i === res.cursor) {
        flush();
        html += `<span class="cursor" style="color:${VGA_COLORS[a & 15]}">${t}</span>`;
        attr = -1;
        continue;
      }
      if (a !== attr) { flush(); attr = a; }
      run += t;
    }
    flush();
    html += '\n';
  }
  pre.innerHTML = html;
}

/* CP437 glyphs for control and high characters */
const CP437 = (() => {
  const t = [];
  const low = '\u263a\u263b\u2665\u2666\u2663\u2660\u2022\u25d8\u25cb\u25d9\u2642\u2640\u266a\u266b\u263c\u25ba\u25c4\u2195\u203c\u00b6\u00a7\u25ac\u21a8\u2191\u2193\u2192\u2190\u221f\u2194\u25b2\u25bc';
  for (let i = 1; i < 32; i++) t[i] = low.charCodeAt(i - 1);
  const high = '\u00c7\u00fc\u00e9\u00e2\u00e4\u00e0\u00e5\u00e7\u00ea\u00eb\u00e8\u00ef\u00ee\u00ec\u00c4\u00c5\u00c9\u00e6\u00c6\u00f4\u00f6\u00f2\u00fb\u00f9\u00ff\u00d6\u00dc\u00a2\u00a3\u00a5\u20a7\u0192\u00e1\u00ed\u00f3\u00fa\u00f1\u00d1\u00aa\u00ba\u00bf\u2310\u00ac\u00bd\u00bc\u00a1\u00ab\u00bb\u2591\u2592\u2593\u2502\u2524\u2561\u2562\u2556\u2555\u2563\u2551\u2557\u255d\u255c\u255b\u2510\u2514\u2534\u252c\u251c\u2500\u253c\u255e\u255f\u255a\u2554\u2569\u2566\u2560\u2550\u256c\u2567\u2568\u2564\u2565\u2559\u2558\u2552\u2553\u256b\u256a\u2518\u250c\u2588\u2584\u258c\u2590\u2580\u03b1\u00df\u0393\u03c0\u03a3\u03c3\u00b5\u03c4\u03a6\u0398\u03a9\u03b4\u221e\u03c6\u03b5\u2229\u2261\u00b1\u2265\u2264\u2320\u2321\u00f7\u2248\u00b0\u2219\u00b7\u221a\u207f\u00b2\u25a0\u00a0';
  for (let i = 128; i < 256; i++) t[i] = high.charCodeAt(i - 128);
  t[127] = 0x2302;
  return t;
})();

/* Keys typed on the screen become bytes for the PC (terminal conventions). */
function keyBytes(e) {
  const map = { Enter: '\r', Backspace: '\x7f', Tab: '\t', Escape: '\x1b', ArrowUp: '\x1b[A', ArrowDown: '\x1b[B',
    ArrowRight: '\x1b[C', ArrowLeft: '\x1b[D', Home: '\x1b[H', End: '\x1b[F' };
  if (map[e.key]) return map[e.key];
  if (e.ctrlKey && e.key.length === 1 && /[a-z]/i.test(e.key)) return String.fromCharCode(e.key.toLowerCase().charCodeAt(0) - 96);
  if (e.key.length === 1 && !e.metaKey) return e.key;
  return null;
}

$('screen').addEventListener('keydown', async (e) => {
  const bytes = keyBytes(e);
  if (bytes === null) return;
  e.preventDefault();
  e.stopPropagation();
  try {
    await post('/api/input', bytes);
    if (!S.running) runLoop();
  } catch (err) {
    toast(err.message);
  }
});

async function renderTab() {
  if (S.tab === 'screen') return renderScreen();
  try {
    if (S.tab === 'cache') return await renderCache();
    if (S.tab === 'syscalls') {
      const res = await api('/api/syscalls');
      $('syscalls').innerHTML = res.lines.length ? res.lines.map((l) => `<div>${esc(l)}</div>`).join('') : '<div class="muted">no system calls yet</div>';
      const b = $('tab-syscalls');
      b.scrollTop = b.scrollHeight;
    } else if (S.tab === 'trace') {
      const res = await api('/api/trace?count=200');
      $('trace').innerHTML = res.lines.length ? res.lines.map((l) =>
        `<div class="row"><span class="sy" data-goto="${l.addr}">${esc(l.sym || l.addr)}</span><span>${highlight(l.text)}</span></div>`).join('') :
        '<div class="muted">nothing executed yet</div>';
      const b = $('tab-trace');
      b.scrollTop = b.scrollHeight;
    } else if (S.tab === 'maps') {
      const res = await api('/api/regions');
      $('maps').innerHTML = res.regions.map((r) =>
        `<div class="row maps"><span>${hex(big(r.start), 16)}-${hex(big(r.end), 16)}</span><span>${r.prot}</span><span class="muted">${esc(r.name)}</span></div>`).join('');
    } else if (S.tab === 'symbols') {
      if (!S.symbols) S.symbols = (await api('/api/symbols')).symbols;
      const f = $('sym-filter').value.trim().toLowerCase();
      const list = S.symbols.filter((s) => !f || s.name.toLowerCase().includes(f)).slice(0, 2000);
      $('symbols').innerHTML = list.map((s) =>
        `<div class="row"><span class="muted">${hex(big(s.addr), width())}</span><span class="sy" data-goto="${s.addr}">${esc(s.name)}</span></div>`).join('');
    }
  } catch (e) {
    toast(e.message);
  }
}

async function refresh(st) {
  if (st) {
    S.prev = S.st;
    S.st = st;
  } else {
    S.st = await api('/api/state');
  }
  renderHeader(S.st);
  renderRegs(S.st);
  await Promise.all([renderDisasm(), renderStack(), renderMem(), renderOutput(S.st), renderTab()]);
}

/* ---------- actions ---------- */

async function act(path) {
  if (S.running) return;
  try {
    S.follow = true;
    $('btn-follow').classList.add('on');
    const st = await post(path);
    await refresh(st);
    if (st.state === 'wait_input') {
      S.resumeOnInput = false;
      $('stdin').focus();
    }
  } catch (e) {
    toast(e.message);
  }
}

async function runLoop() {
  if (S.running) return;
  S.running = true;
  S.pause = false;
  S.follow = true;
  $('btn-follow').classList.add('on');
  const before = S.st;
  let st;
  try {
    renderHeader(S.st);
    for (;;) {
      st = await post('/api/continue');
      S.st = st;
      renderHeader(st);
      await renderOutput(st);
      if (S.tab === 'screen' && Date.now() - (S.screenAt || 0) > 100) {
        S.screenAt = Date.now();
        await renderScreen();
      }
      if (st.stop !== 'limit' || S.pause) break;
    }
  } catch (e) {
    toast(e.message);
  }
  S.running = false;
  S.st = before;
  await refresh(st);
  if (st && st.state === 'wait_input') {
    S.resumeOnInput = true;
    $('stdin').focus();
    toast('The program is waiting for input');
  }
}

async function toggleBreakpoint(addr) {
  try {
    const st = await post(`/api/break?addr=${addr}`);
    S.st = st;
    await renderDisasm();
  } catch (e) {
    toast(e.message);
  }
}

function viewAt(addr) {
  S.follow = false;
  S.view = addr;
  $('btn-follow').classList.remove('on');
  renderDisasm();
}

async function gotoExpr(expr) {
  const local = resolveLocal(expr);
  if (local !== null) return viewAt(local);
  try {
    const res = await api(`/api/disasm?count=1&addr=${encodeURIComponent(expr)}`);
    viewAt(big(res.lines[0].addr));
  } catch (e) {
    toast(e.message);
  }
}

async function editRegister(el) {
  const name = el.dataset.reg;
  const rv = el.querySelector('.rv');
  if (!rv) return;
  const cur = name === 'rip' ? big(S.st.rip) : regValue(name);
  const input = document.createElement('input');
  input.value = hex(cur);
  rv.replaceWith(input);
  input.focus();
  input.select();
  const finish = async (commit) => {
    if (commit) {
      try {
        const st = await post(`/api/setreg?name=${name}&value=${encodeURIComponent(input.value.replace(/^0x/, ''))}`);
        S.prev = S.st;
        S.st = st;
        await refresh(st);
        return;
      } catch (e) {
        toast(e.message);
      }
    }
    renderRegs(S.st);
  };
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') finish(true);
    if (e.key === 'Escape') finish(false);
  });
  input.addEventListener('blur', () => finish(false));
}

/* ---------- wiring ---------- */

$('btn-step').onclick = () => act('/api/step');
$('btn-next').onclick = () => act('/api/next');
$('btn-finish').onclick = () => act('/api/finish');
$('btn-continue').onclick = () => runLoop();
$('btn-pause').onclick = () => { S.pause = true; };
$('btn-reset').onclick = async () => {
  S.outLen = 0;
  S.announced = '';
  S.symbols = null;
  S.memPrev = null;
  $('console').textContent = '';
  await act('/api/reset');
  S.prev = null;
  renderRegs(S.st);
};
$('btn-follow').onclick = () => {
  S.follow = !S.follow;
  $('btn-follow').classList.toggle('on', S.follow);
  renderDisasm();
};
$('btn-xmm').onclick = () => {
  S.showXmm = !S.showXmm;
  $('btn-xmm').classList.toggle('on', S.showXmm);
  renderRegs(S.st);
};

$('disasm').addEventListener('click', (e) => {
  const g = e.target.closest('.gutter');
  if (g) return toggleBreakpoint(g.parentElement.dataset.addr);
  const j = e.target.closest('a.jump');
  if (j) return viewAt(big(j.dataset.target));
});
$('disasm').addEventListener('dblclick', (e) => {
  const line = e.target.closest('.dline');
  if (line && !e.target.closest('.gutter')) toggleBreakpoint(line.dataset.addr);
});

$('regs').addEventListener('click', (e) => {
  const r = e.target.closest('.reg');
  if (r && !r.querySelector('input') && !S.running) editRegister(r);
});
$('flags').addEventListener('click', async (e) => {
  const f = e.target.closest('.flag[data-bit]');
  if (!f || S.running) return;
  const v = big(S.st.rflags) ^ (1n << BigInt(f.dataset.bit));
  try {
    await refresh(await post(`/api/setreg?name=rflags&value=${hex(v)}`));
  } catch (err) {
    toast(err.message);
  }
});

$('goto').addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && e.target.value.trim()) gotoExpr(e.target.value.trim());
});
$('mem-addr').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    S.memExpr = e.target.value.trim() || 'rsp';
    S.memPrev = null;
    renderMem();
  }
});
const memMove = (delta) => {
  if (S.memBase === null) return;
  const nb = S.memBase + BigInt(delta);
  S.memExpr = '0x' + hex(nb < 0n ? 0n : nb);
  $('mem-addr').value = S.memExpr;
  renderMem();
};
$('btn-mem-up').onclick = () => memMove(-256);
$('btn-mem-down').onclick = () => memMove(256);

function selectTab(name) {
  S.tab = name;
  document.querySelectorAll('.tab').forEach((x) => x.classList.toggle('active', x.dataset.tab === name));
  for (const n of ['screen', 'console', 'syscalls', 'trace', 'maps', 'cache', 'symbols'])
    $('tab-' + n).classList.toggle('hidden', n !== name);
  $('input-form').classList.toggle('hidden', name !== 'console');
  renderTab();
}
document.querySelectorAll('.tab').forEach((t) => { t.onclick = () => selectTab(t.dataset.tab); });
$('sym-filter').addEventListener('input', renderTab);
for (const id of ['trace', 'symbols']) {
  $(id).addEventListener('click', (e) => {
    const g = e.target.closest('[data-goto]');
    if (g) viewAt(big(g.dataset.goto));
  });
}

$('input-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const text = $('stdin').value + '\n';
  $('stdin').value = '';
  try {
    const st = await post('/api/input', text);
    await refresh(st);
    if (S.resumeOnInput && st.state === 'wait_input') {
      S.resumeOnInput = false;
      runLoop();
    }
  } catch (err) {
    toast(err.message);
  }
});

$('btn-load').onclick = () => {
  $('load-error').textContent = '';
  $('load-files').value = S.st ? S.st.program.join(', ') : '';
  $('load-dialog').showModal();
};
$('load-ok').addEventListener('click', async (e) => {
  e.preventDefault();
  const files = $('load-files').value.split(',').map((s) => s.trim()).filter(Boolean).join(',');
  const q = `files=${encodeURIComponent(files)}&args=${encodeURIComponent($('load-args').value)}&bits=${$('load-bits').value}`;
  try {
    const st = await post('/api/load?' + q);
    $('load-dialog').close();
    S.outLen = 0;
    S.announced = '';
    S.symbols = null;
    S.prev = null;
    S.memPrev = null;
    $('console').textContent = '';
    await refresh(st);
  } catch (err) {
    $('load-error').textContent = err.message;
  }
});

document.addEventListener('keydown', (e) => {
  const typing = ['INPUT', 'SELECT', 'TEXTAREA'].includes(document.activeElement.tagName) || document.activeElement.id === 'screen';
  if (e.key === 'F10') { e.preventDefault(); act('/api/next'); }
  else if (e.key === 'F11' && e.shiftKey) { e.preventDefault(); act('/api/finish'); }
  else if (e.key === 'F11') { e.preventDefault(); act('/api/step'); }
  else if (e.key === 'F5') { e.preventDefault(); runLoop(); }
  else if (e.key === 'F6') { e.preventDefault(); S.pause = true; }
  else if (e.key === 'r' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); $('btn-reset').click(); }
  else if (!typing && !e.metaKey && !e.ctrlKey && !e.altKey) {
    const k = e.key.toLowerCase();
    if (k === 's') act('/api/step');
    else if (k === 'n') act('/api/next');
    else if (k === 'f') act('/api/finish');
    else if (k === 'c') runLoop();
  }
});

window.addEventListener('resize', () => { clearTimeout(S.resizeTimer); S.resizeTimer = setTimeout(renderMem, 150); });

refresh().catch((e) => toast('Cannot reach x86sim: ' + e.message));
