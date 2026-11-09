// Headless checks for the visualizer. Run: node viz/test/viz_test.js
//
// What this verifies: the data files parse and are internally consistent, the model/prepare code
// matches the counts in the export, every draw function runs on the real sample data at many
// cursor positions without emitting NaN/Infinity coordinates (against a mock canvas), the DOM
// wiring (play/pause/step/scrub/speed/keyboard/selectors) works against a fake DOM, and the page
// references only local files (no network). It does NOT verify pixels or layout; see the
// headless-browser screenshot step in docs/DESIGN.md for that.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const VIZ = path.resolve(__dirname, '..');
const LobViz = require(path.join(VIZ, 'app.js'));
let failures = 0, checks = 0;
function test(name, fn) {
  try { fn(); checks++; console.log('ok   ' + name); } catch (e) { failures++; console.log('FAIL ' + name + '\n     ' + (e && e.stack || e)); }
}

function loadScript(file) {
  const sandbox = { window: {} };
  vm.runInNewContext(fs.readFileSync(path.join(VIZ, 'data', file), 'utf8'), sandbox, { filename: file });
  return sandbox.window;
}
const RUN = loadScript('sample_run.js').LOB_RUN;
const LAT = loadScript('latency.js').LOB_LATENCY;

// ----------------------------------------------------------------------- mock canvas
function mockCtx() {
  const calls = [];
  const bad = [];
  const ctx = { calls, bad };
  const check = (name, args) => args.forEach((a) => { if (typeof a === 'number' && !Number.isFinite(a)) bad.push(name + '(' + args.join(',') + ')'); });
  for (const m of ['clearRect', 'fillRect', 'beginPath', 'moveTo', 'lineTo', 'closePath', 'stroke', 'fill', 'fillText', 'setLineDash', 'setTransform', 'strokeRect', 'save', 'restore']) {
    ctx[m] = (...args) => { calls.push(m); check(m, args); };
  }
  ctx.measureText = (s) => ({ width: String(s).length * 6 });
  return ctx;
}
function mockCanvas() {
  const ctx = mockCtx();
  return { width: 0, height: 0, ctx, getContext: () => ctx, getBoundingClientRect: () => ({ width: 640, height: 260 }) };
}

// -------------------------------------------------------------------------- data checks
test('sample run format and counts are consistent', () => {
  assert.strictEqual(RUN.format, 'lob-run-v1');
  const m = LobViz.prepare(RUN);
  const c = RUN.meta.counts, p = m.prefix;
  assert.strictEqual(m.N, RUN.meta.ops);
  assert.strictEqual(p.submits[m.N], c.submits);
  assert.strictEqual(p.cancels[m.N], c.cancels);
  assert.strictEqual(p.modifies[m.N], c.modifies);
  assert.strictEqual(p.trades[m.N], c.trades);
  assert.strictEqual(p.rejects[m.N], c.rejects);
  assert.strictEqual(m.tradeList.length, c.trades);
  assert.strictEqual(RUN.events.length, c.events);
  assert.strictEqual(c.submits + c.cancels + c.modifies, m.N);
  assert.strictEqual(m.snapOp[0], 0);
  assert.strictEqual(m.snapOp[m.S - 1], m.N);
});

test('timestamps increase, snapshots are sorted and uncrossed', () => {
  const m = LobViz.prepare(RUN);
  for (let i = 1; i < m.N; i++) assert.ok(m.ops[i][0] > m.ops[i - 1][0], 'time at op ' + i);
  for (let i = 1; i < m.S; i++) assert.ok(m.snapOp[i] > m.snapOp[i - 1]);
  for (const s of m.snaps) {
    if (s[2].length && s[3].length) assert.ok(s[2][0][0] < s[3][0][0], 'crossed book in snapshot at op ' + s[0]);
    for (let i = 1; i < s[2].length; i++) assert.ok(s[2][i - 1][0] > s[2][i][0]);
    for (let i = 1; i < s[3].length; i++) assert.ok(s[3][i - 1][0] < s[3][i][0]);
  }
});

test('trades are sorted by op and priced inside plausible bounds', () => {
  const m = LobViz.prepare(RUN);
  for (let i = 1; i < m.tradeList.length; i++) assert.ok(m.tradeList[i].op >= m.tradeList[i - 1].op);
  for (const t of m.tradeList) { assert.ok(t.qty > 0); assert.ok(t.price >= m.pmin - 100 && t.price <= m.pmax + 100); }
});

test('snapshotIndexAt and tradeCountAt', () => {
  const m = LobViz.prepare(RUN);
  assert.strictEqual(LobViz.snapshotIndexAt(m, 0), 0);
  assert.strictEqual(LobViz.snapshotIndexAt(m, m.N), m.S - 1);
  for (const k of [1, 7, 8, 9, 1234, 7999]) {
    const i = LobViz.snapshotIndexAt(m, k);
    assert.ok(m.snapOp[i] <= k && (i === m.S - 1 || m.snapOp[i + 1] > k));
  }
  assert.strictEqual(LobViz.tradeCountAt(m, 0), 0);
  assert.strictEqual(LobViz.tradeCountAt(m, m.N), m.tradeList.length);
});

test('windowStats matches a direct count', () => {
  const m = LobViz.prepare(RUN);
  const b = 3000, win = 500;
  const s = LobViz.windowStats(m, b, win);
  let sub = 0, can = 0, mod = 0;
  for (let i = b - win; i < b; i++) { const k = m.ops[i][1]; if (k === 0) sub++; else if (k === 1) can++; else mod++; }
  assert.strictEqual(s.submits, sub); assert.strictEqual(s.cancels, can); assert.strictEqual(s.modifies, mod);
  assert.strictEqual(s.ops, win);
});

test('latency data: histograms add up to the sample count, percentiles ordered', () => {
  assert.strictEqual(LAT.format, 'lob-latency-v1');
  assert.ok(LAT.sources.length >= 20);
  for (const s of LAT.sources) {
    assert.strictEqual(s.histogram_log2_ns.reduce((a, b) => a + b, 0), s.samples, s.file);
    assert.ok(s.p50_ns <= s.p99_ns && s.p99_ns <= s.p99_9_ns && s.p99_9_ns <= s.max_ns, s.file);
    assert.ok(/^bench\/results\//.test(s.file));
  }
  const studies = new Set(LAT.sources.map((s) => s.study));
  assert.ok(studies.has('stage3') && studies.has('stage4'));
});

// -------------------------------------------------------------------------- drawing
test('drawDepth/Series/Flow produce finite geometry at many cursor positions', () => {
  const m = LobViz.prepare(RUN);
  const st = { yMax: 0 };
  for (const frac of [0, 0.001, 0.05, 0.25, 0.5, 0.75, 0.999, 1]) {
    const applied = Math.floor(frac * m.N);
    const cv = mockCanvas();
    LobViz.drawDepth(cv.ctx, 640, 260, m.snaps[LobViz.snapshotIndexAt(m, applied)], st, null);
    LobViz.drawSeries(cv.ctx, 640, 280, m, applied, null);
    LobViz.drawFlow(cv.ctx, 640, 180, m, applied, null);
    assert.ok(cv.ctx.calls.length > 40, 'few draw calls at ' + frac);
    assert.deepStrictEqual(cv.ctx.bad, [], 'non-finite arguments at ' + frac);
  }
});

test('depth chart handles one-sided and empty books', () => {
  const st = { yMax: 0 };
  for (const snap of [[0, 0, [], []], [1, 5, [[100, 5, 1], [99, 7, 2]], []], [2, 9, [], [[101, 3, 1]]]]) {
    const cv = mockCanvas();
    LobViz.drawDepth(cv.ctx, 400, 200, snap, st, null);
    assert.ok(cv.ctx.calls.length > 0);
    assert.deepStrictEqual(cv.ctx.bad, []);
  }
});

test('drawLatency renders every source, with finite geometry', () => {
  for (const s of LAT.sources) {
    const cv = mockCanvas();
    LobViz.drawLatency(cv.ctx, 800, 240, s, null);
    assert.ok(cv.ctx.calls.filter((c) => c === 'fillRect').length >= 3, s.file);
    assert.deepStrictEqual(cv.ctx.bad, [], s.file);
  }
  const cv = mockCanvas();
  LobViz.drawLatency(cv.ctx, 800, 240, null, null);
  assert.ok(cv.ctx.calls.includes('fillText'));
});

// -------------------------------------------------------------------------- playback
test('Player: play, tick, speed, step, seek, end-of-run', () => {
  const m = LobViz.prepare(RUN);
  const p = new LobViz.Player(m);
  p.setSpeed(1000);
  p.tick(100);
  assert.strictEqual(p.cursor, 0, 'does not advance while paused');
  p.play(); p.tick(100);
  assert.ok(Math.abs(p.cursor - 100) < 1e-9);
  p.step(1);
  assert.strictEqual(p.playing, false);
  assert.strictEqual(p.applied, m.snapOp[LobViz.snapshotIndexAt(m, 100) + 1]);
  const here = p.applied; p.step(-1);
  assert.ok(p.applied < here);
  p.seek(-5); assert.strictEqual(p.cursor, 0);
  p.seek(1e9); assert.strictEqual(p.cursor, m.N); assert.strictEqual(p.playing, false);
  p.play(); assert.strictEqual(p.cursor, 0, 'play at the end restarts');
  p.setSpeed(0); assert.strictEqual(p.speed, 1);
});

// ------------------------------------------------------------------------ fake DOM
function fakeEnvironment() {
  const listeners = {};
  const els = {};
  function makeEl(id) {
    const e = { id, textContent: '', value: '', style: {}, listeners: {}, children: [], options: [], files: [] };
    e.addEventListener = (ev, fn) => { (e.listeners[ev] = e.listeners[ev] || []).push(fn); };
    e.fire = (ev, extra) => (e.listeners[ev] || []).forEach((fn) => fn(Object.assign({ target: e }, extra)));
    let html = '';
    Object.defineProperty(e, 'innerHTML', {
      get: () => html,
      set: (v) => {
        html = v;
        if (/^lat-/.test(id)) {
          e.options = Array.from(v.matchAll(/<option value="([^"]*)"/g)).map((m) => ({ value: m[1] }));
          if (!e.options.some((o) => o.value === e.value)) e.value = e.options.length ? e.options[0].value : '';
        }
      },
    });
    if (/^cv-/.test(id)) Object.assign(e, mockCanvas());
    return e;
  }
  const doc = {
    documentElement: {},
    getElementById: (id) => (els[id] = els[id] || makeEl(id)),
    addEventListener: (ev, fn) => { (listeners[ev] = listeners[ev] || []).push(fn); },
  };
  let raf = null;
  const win = {
    LOB_RUN: RUN, LOB_LATENCY: LAT, devicePixelRatio: 2, location: { hash: '#at=0.5&play=0' },
    getComputedStyle: () => ({ getPropertyValue: () => '' }),
    requestAnimationFrame: (fn) => { raf = fn; },
    addEventListener: () => {},
  };
  return { doc, win, els, listeners, frame: (ts) => { const f = raf; raf = null; if (f) f(ts); }, hasFrame: () => raf !== null };
}

test('init wires the page: initial render, play/pause, step, scrub, speed, keyboard, selectors', () => {
  const env = fakeEnvironment();
  const app = LobViz.init(env.doc, env.win);
  assert.ok(app.model && app.player);
  assert.ok(Math.abs(app.player.cursor - 0.5 * app.model.N) < 1, 'hash #at=0.5 applied');
  assert.strictEqual(app.player.playing, false, 'hash #play=0 applied');
  env.frame(0);
  for (const id of ['cv-depth', 'cv-series', 'cv-flow', 'cv-latency']) {
    const cv = env.els[id];
    assert.ok(cv.ctx.calls.length > 20, id + ' was drawn');
    assert.deepStrictEqual(cv.ctx.bad, [], id);
    assert.strictEqual(cv.width, 1280, 'canvas scaled by devicePixelRatio');
  }
  assert.ok(/op 4000 \/ 8000/.test(env.els['readout'].textContent), env.els['readout'].textContent);
  assert.ok(/<tr/.test(env.els['tape-body'].innerHTML));
  assert.ok(/aggressive share/.test(env.els['stats-body'].innerHTML));
  assert.ok(/seed 7/.test(env.els['run-info'].textContent));
  assert.ok(/05-linear-scan-levels_balanced/.test(env.els['lat-info'].textContent), 'defaults to the final book step');

  env.els['btn-play'].fire('click');
  assert.strictEqual(app.player.playing, true);
  const c0 = app.player.cursor;
  env.frame(100); env.frame(200); env.frame(300);
  assert.ok(app.player.cursor > c0, 'cursor advances while playing');
  env.els['btn-play'].fire('click');
  assert.strictEqual(app.player.playing, false);

  env.els['scrub'].value = '123'; env.els['scrub'].fire('input');
  assert.strictEqual(app.player.applied, 123);
  env.els['btn-next'].fire('click');
  assert.ok(app.player.applied > 123);
  env.els['btn-prev'].fire('click');
  env.els['btn-end'].fire('click');
  assert.strictEqual(app.player.applied, app.model.N);
  env.els['btn-start'].fire('click');
  assert.strictEqual(app.player.applied, 0);
  env.els['speed'].value = '2000'; env.els['speed'].fire('change');
  assert.strictEqual(app.player.speed, 2000);

  env.listeners['keydown'].forEach((fn) => fn({ key: ' ', target: {}, preventDefault() {} }));
  assert.strictEqual(app.player.playing, true);
  env.listeners['keydown'].forEach((fn) => fn({ key: 'ArrowRight', target: {}, preventDefault() {} }));
  assert.strictEqual(app.player.playing, false);

  // latency selectors
  env.els['lat-study'].value = 'stage4'; env.els['lat-study'].fire('change');
  assert.ok(env.els['lat-variant'].options.length >= 5, 'stage 4 variants listed');
  env.frame(400);
  assert.ok(/Stage 4/.test(env.els['lat-info'].textContent) && /intended send time/.test(env.els['lat-info'].textContent));
  assert.ok(/stage4\/spsc_balanced/.test(env.els['lat-info'].textContent), 'defaults to the spsc variant');
  env.els['lat-variant'].value = '0'; env.els['lat-variant'].fire('change');
  assert.ok(!/spsc_balanced/.test(env.els['lat-info'].textContent), 'variant change picks another file');
  env.els['lat-workload'].value = 'passive'; env.els['lat-workload'].fire('change');
  assert.ok(/passive/.test(env.els['lat-info'].textContent) || /bench\/results\/stage4\/.*passive/.test(env.els['lat-info'].textContent));
});

test('init reports a bad file instead of throwing', () => {
  const env = fakeEnvironment();
  env.win.LOB_RUN = undefined;
  const app = LobViz.init(env.doc, env.win);
  assert.strictEqual(app.model, null);
  assert.ok(/No sample run/.test(env.els['status'].textContent));
  assert.throws(() => LobViz.prepare({ format: 'nope' }), /lob-run-v1/);
});

// ------------------------------------------------------------------- static page checks
test('index.html references only local files that exist and ids that app.js uses', () => {
  const html = fs.readFileSync(path.join(VIZ, 'index.html'), 'utf8');
  const refs = [...html.matchAll(/(?:src|href)="([^"]+)"/g)].map((m) => m[1]);
  assert.ok(refs.length >= 4);
  for (const r of refs) {
    assert.ok(!/^[a-z]+:\/\//i.test(r) && !r.startsWith('//'), 'external reference: ' + r);
    assert.ok(fs.existsSync(path.join(VIZ, r)), 'missing file ' + r);
  }
  const ids = new Set([...html.matchAll(/id="([^"]+)"/g)].map((m) => m[1]));
  const app = fs.readFileSync(path.join(VIZ, 'app.js'), 'utf8');
  for (const m of app.matchAll(/\$\('([^']+)'\)/g)) assert.ok(ids.has(m[1]), 'app.js uses id missing from html: ' + m[1]);
});

test('no network URLs or CDN references in page, script or stylesheet', () => {
  for (const f of ['index.html', 'app.js', 'style.css']) {
    const s = fs.readFileSync(path.join(VIZ, f), 'utf8');
    assert.ok(!/https?:\/\//i.test(s), f + ' contains an http(s) URL');
    assert.ok(!/@import|fetch\(|XMLHttpRequest|WebSocket/.test(s), f + ' performs network access');
  }
});

console.log('\n' + checks + ' passed, ' + failures + ' failed');
process.exit(failures ? 1 : 0);
