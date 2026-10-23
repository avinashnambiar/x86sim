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
  $('mode').textContent = `${st.bits === 16 ? '16-bit real mode' : st.bits + '-bit'} · ${st.os}`;
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

async function renderTab() {
  try {
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

document.querySelectorAll('.tab').forEach((t) => {
  t.onclick = () => {
    S.tab = t.dataset.tab;
    document.querySelectorAll('.tab').forEach((x) => x.classList.toggle('active', x === t));
    for (const name of ['console', 'syscalls', 'trace', 'maps', 'symbols'])
      $('tab-' + name).classList.toggle('hidden', name !== S.tab);
    $('input-form').classList.toggle('hidden', S.tab !== 'console');
    renderTab();
  };
});
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
  const typing = ['INPUT', 'SELECT', 'TEXTAREA'].includes(document.activeElement.tagName);
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
