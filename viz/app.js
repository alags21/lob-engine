/* lob-engine visualizer: plain JS + canvas, no dependencies, no network access.
 *
 * Loads a run exported by lob_sim / lob_replay ("lob-run-v1", see docs/EVENT_FORMAT.md) from
 * window.LOB_RUN (viz/data/sample_run.js) or from a file the user picks, and a set of latency
 * histograms from window.LOB_LATENCY (viz/data/latency.js, copied from bench/results/*.json).
 *
 * The file works in two environments: in a browser (`LobViz.init(document, window)` is called
 * from index.html) and in Node (module.exports), where viz/test/viz_test.js drives it with a
 * mock canvas.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.LobViz = api;
})(typeof window !== 'undefined' ? window : globalThis, function () {
  'use strict';

  // Fallback colours (used where CSS variables are unavailable, e.g. under Node).
  const DEFAULT_THEME = {
    text: '#1d2733', muted: '#5b6b7c', grid: 'rgba(120,135,150,0.25)', bg: '#ffffff',
    bid: '#1b8a7e', ask: '#d9552f', accent: '#3b6fd4', passive: '#7aa7e8', aggressive: '#f0a23c',
    cancel: '#9aa5b1', modify: '#a98bd6', cursor: '#111111',
  };

  // ---------------------------------------------------------------- data preparation
  function prepare(run) {
    if (!run || run.format !== 'lob-run-v1') throw new Error('not a lob-run-v1 file');
    const ops = run.ops, events = run.events, snaps = run.snapshots;
    const N = ops.length;
    if (!N || !snaps.length) throw new Error('run has no ops or no snapshots');

    // Prefix counts so any window [a, b) of ops can be summarised in O(1).
    const mk = () => new Float64Array(N + 1);
    const submits = mk(), aggressive = mk(), cancels = mk(), modifies = mk(), trades = mk(), rejects = mk(), tradedQty = mk();
    for (let i = 0; i < N; i++) {
      const o = ops[i]; // [t, kind, id, side, type, tif, price, qty]
      submits[i + 1] = submits[i] + (o[1] === 0 ? 1 : 0);
      aggressive[i + 1] = aggressive[i] + (o[1] === 0 && (o[4] === 1 || o[5] !== 0) ? 1 : 0);
      cancels[i + 1] = cancels[i] + (o[1] === 1 ? 1 : 0);
      modifies[i + 1] = modifies[i] + (o[1] === 2 ? 1 : 0);
    }
    const tradeCnt = new Float64Array(N), rejCnt = new Float64Array(N), qtyCnt = new Float64Array(N);
    const tradeList = [];
    for (const e of events) { // [op_index, type, order_id, maker_id, side, price, qty, aux]
      if (e[1] === 2) {
        tradeCnt[e[0]]++; qtyCnt[e[0]] += e[6];
        tradeList.push({ op: e[0], t: ops[e[0]][0], price: e[5], qty: e[6], side: e[4], taker: e[2], maker: e[3] });
      } else if (e[1] === 1) rejCnt[e[0]]++;
    }
    for (let i = 0; i < N; i++) {
      trades[i + 1] = trades[i] + tradeCnt[i];
      rejects[i + 1] = rejects[i] + rejCnt[i];
      tradedQty[i + 1] = tradedQty[i] + qtyCnt[i];
    }

    const S = snaps.length;
    const snapOp = new Int32Array(S), snapT = new Float64Array(S);
    const bestBid = new Array(S), bestAsk = new Array(S), mid = new Array(S), spread = new Array(S);
    let pmin = Infinity, pmax = -Infinity, spreadMax = 0;
    for (let i = 0; i < S; i++) {
      const s = snaps[i]; // [op_index, t_ns, bids, asks]
      snapOp[i] = s[0]; snapT[i] = s[1];
      const bb = s[2].length ? s[2][0][0] : null, ba = s[3].length ? s[3][0][0] : null;
      bestBid[i] = bb; bestAsk[i] = ba;
      mid[i] = bb !== null && ba !== null ? (bb + ba) / 2 : null;
      spread[i] = bb !== null && ba !== null ? ba - bb : null;
      if (bb !== null) pmin = Math.min(pmin, bb);
      if (ba !== null) pmax = Math.max(pmax, ba);
      if (spread[i] !== null) spreadMax = Math.max(spreadMax, spread[i]);
    }
    const duration = Math.max(1, ops[N - 1][0]);

    // Order-flow timeline: fixed number of buckets over op index.
    const B = Math.min(80, N);
    const flow = [];
    for (let b = 0; b < B; b++) {
      const a = Math.floor((b * N) / B), z = Math.floor(((b + 1) * N) / B);
      flow.push({
        passive: (submits[z] - submits[a]) - (aggressive[z] - aggressive[a]), aggressive: aggressive[z] - aggressive[a],
        cancels: cancels[z] - cancels[a], modifies: modifies[z] - modifies[a], trades: trades[z] - trades[a],
      });
    }
    return { run, meta: run.meta, N, ops, snaps, S, snapOp, snapT, bestBid, bestAsk, mid, spread, pmin, pmax, spreadMax,
      duration, prefix: { submits, aggressive, cancels, modifies, trades, rejects, tradedQty }, tradeList, flow };
  }

  // Largest snapshot index whose op_index <= applied (snapshot 0 is op_index 0).
  function snapshotIndexAt(model, applied) {
    let lo = 0, hi = model.S - 1;
    while (lo < hi) {
      const m = (lo + hi + 1) >> 1;
      if (model.snapOp[m] <= applied) lo = m; else hi = m - 1;
    }
    return lo;
  }

  // Number of trades with op index < applied (trades are sorted by op).
  function tradeCountAt(model, applied) {
    const tl = model.tradeList;
    let lo = 0, hi = tl.length;
    while (lo < hi) {
      const m = (lo + hi) >> 1;
      if (tl[m].op < applied) lo = m + 1; else hi = m;
    }
    return lo;
  }

  function windowStats(model, applied, window) {
    const b = Math.max(0, Math.min(model.N, Math.floor(applied)));
    const a = Math.max(0, b - window);
    const p = model.prefix;
    const d = (arr) => arr[b] - arr[a];
    const submits = d(p.submits), agg = d(p.aggressive);
    const dt = a < b ? (model.ops[Math.max(0, b - 1)][0] - model.ops[a][0]) : 0;
    return {
      ops: b - a, submits, aggressive: agg, cancels: d(p.cancels), modifies: d(p.modifies), trades: d(p.trades),
      rejects: d(p.rejects), volume: d(p.tradedQty), aggressiveShare: submits ? agg / submits : 0,
      cancelToSubmit: submits ? d(p.cancels) / submits : 0, opsPerSec: dt > 0 ? ((b - a - 1) * 1e9) / dt : 0,
    };
  }

  // ---------------------------------------------------------------------- formatting
  function formatNs(ns) {
    if (!isFinite(ns)) return '-';
    if (ns >= 1e9) return (ns / 1e9).toFixed(3) + ' s';
    if (ns >= 1e6) return (ns / 1e6).toFixed(3) + ' ms';
    if (ns >= 1e3) return (ns / 1e3).toFixed(1) + ' us';
    return Math.round(ns) + ' ns';
  }
  function formatNum(x, d) { return isFinite(x) ? x.toFixed(d === undefined ? 0 : d) : '-'; }
  const sideName = (s) => (s === 0 ? 'buy' : 'sell');

  // ------------------------------------------------------------------------- drawing
  function clear(ctx, w, h, th) {
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = th.bg; ctx.fillRect(0, 0, w, h);
  }
  function text(ctx, th, s, x, y, align, color, font) {
    ctx.fillStyle = color || th.muted; ctx.font = font || '11px system-ui, sans-serif';
    ctx.textAlign = align || 'left'; ctx.textBaseline = 'alphabetic'; ctx.fillText(s, x, y);
  }
  function niceTicks(lo, hi, n) {
    const span = hi - lo || 1, raw = span / n, p = Math.pow(10, Math.floor(Math.log10(raw)));
    const step = [1, 2, 5, 10].map((m) => m * p).find((s) => s >= raw) || p * 10;
    const out = [];
    for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(v);
    return out;
  }

  // Depth chart: cumulative resting quantity vs price for the snapshot in `snap` ([op, t, bids, asks]).
  // `st` carries the smoothed y-axis maximum between frames.
  function drawDepth(ctx, w, h, snap, st, th) {
    th = th || DEFAULT_THEME;
    clear(ctx, w, h, th);
    const padL = 44, padR = 10, padT = 10, padB = 24, pw = w - padL - padR, ph = h - padT - padB;
    const bids = snap[2], asks = snap[3];
    if (!bids.length && !asks.length) { text(ctx, th, 'empty book', w / 2, h / 2, 'center'); return; }
    const bb = bids.length ? bids[0][0] : asks[0][0], ba = asks.length ? asks[0][0] : bids[0][0];
    const mid = (bb + ba) / 2;
    let half = 8;
    if (bids.length) half = Math.max(half, mid - bids[bids.length - 1][0]);
    if (asks.length) half = Math.max(half, asks[asks.length - 1][0] - mid);
    half *= 1.05;
    const x0 = mid - half, x1 = mid + half;
    const cum = (lv) => { let c = 0; return lv.map((l) => (c += l[1])); };
    const cb = cum(bids), ca = cum(asks);
    const top = Math.max(cb.length ? cb[cb.length - 1] : 0, ca.length ? ca[ca.length - 1] : 0, 1);
    st.yMax = Math.max(top, (st.yMax || 0) * 0.97);
    const X = (p) => padL + ((p - x0) / (x1 - x0)) * pw;
    const Y = (q) => padT + ph - (q / st.yMax) * ph;

    ctx.strokeStyle = th.grid; ctx.lineWidth = 1;
    for (const v of niceTicks(0, st.yMax, 3)) {
      ctx.beginPath(); ctx.moveTo(padL, Y(v)); ctx.lineTo(w - padR, Y(v)); ctx.stroke();
      text(ctx, th, formatNum(v), padL - 4, Y(v) + 3, 'right');
    }
    for (const v of niceTicks(x0, x1, 6)) text(ctx, th, formatNum(v), X(v), h - 8, 'center');

    const area = (levels, cumul, color, dir) => {
      if (!levels.length) return;
      const edge = dir < 0 ? X(x0) : X(x1);
      ctx.beginPath();
      ctx.moveTo(X(levels[0][0]), Y(0));
      ctx.lineTo(X(levels[0][0]), Y(cumul[0]));
      for (let i = 1; i < levels.length; i++) {
        ctx.lineTo(X(levels[i][0]), Y(cumul[i - 1]));
        ctx.lineTo(X(levels[i][0]), Y(cumul[i]));
      }
      ctx.lineTo(edge, Y(cumul[cumul.length - 1]));
      ctx.lineTo(edge, Y(0));
      ctx.closePath();
      ctx.globalAlpha = 0.28; ctx.fillStyle = color; ctx.fill();
      ctx.globalAlpha = 1; ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.stroke();
    };
    area(bids, cb, th.bid, -1);
    area(asks, ca, th.ask, 1);
    ctx.setLineDash([4, 4]); ctx.strokeStyle = th.muted; ctx.beginPath(); ctx.moveTo(X(mid), padT); ctx.lineTo(X(mid), padT + ph); ctx.stroke(); ctx.setLineDash([]);
    text(ctx, th, 'bids', padL + 6, padT + 12, 'left', th.bid, 'bold 11px system-ui, sans-serif');
    text(ctx, th, 'asks', w - padR - 6, padT + 12, 'right', th.ask, 'bold 11px system-ui, sans-serif');
    text(ctx, th, 'cumulative qty vs price (ticks)', w / 2, padT + 12, 'center');
  }

  // Mid price (with trade dots) and spread over time, with a cursor at `applied` ops.
  function drawSeries(ctx, w, h, model, applied, th) {
    th = th || DEFAULT_THEME;
    clear(ctx, w, h, th);
    const padL = 44, padR = 10, padT = 8, padB = 20, pw = w - padL - padR;
    const gap = 16, midH = Math.floor((h - padT - padB - gap) * 0.66), spH = h - padT - padB - gap - midH;
    const X = (t) => padL + (t / model.duration) * pw;
    const lo = model.pmin, hi = model.pmax, span = Math.max(1, hi - lo);
    const YM = (p) => padT + midH - ((p - lo) / span) * midH;
    const spY0 = padT + midH + gap, YS = (s) => spY0 + spH - (s / Math.max(1, model.spreadMax)) * spH;

    ctx.strokeStyle = th.grid; ctx.lineWidth = 1;
    for (const v of niceTicks(lo, hi, 3)) { ctx.beginPath(); ctx.moveTo(padL, YM(v)); ctx.lineTo(w - padR, YM(v)); ctx.stroke(); text(ctx, th, formatNum(v), padL - 4, YM(v) + 3, 'right'); }
    text(ctx, th, 'mid price', padL + 4, padT + 11, 'left', th.text);
    ctx.strokeStyle = th.accent; ctx.lineWidth = 1.5; ctx.beginPath();
    let started = false;
    for (let i = 0; i < model.S; i++) {
      if (model.mid[i] === null) continue;
      const x = X(model.snapT[i]), y = YM(model.mid[i]);
      if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
    }
    ctx.stroke();
    const tl = model.tradeList, stride = Math.max(1, Math.floor(tl.length / 500));
    for (let i = 0; i < tl.length; i += stride) {
      ctx.fillStyle = tl[i].side === 0 ? th.bid : th.ask;
      ctx.fillRect(X(tl[i].t) - 1, YM(tl[i].price) - 1, 2, 2);
    }
    // spread
    ctx.strokeStyle = th.grid; ctx.lineWidth = 1;
    for (const v of niceTicks(0, model.spreadMax, 2)) { ctx.beginPath(); ctx.moveTo(padL, YS(v)); ctx.lineTo(w - padR, YS(v)); ctx.stroke(); text(ctx, th, formatNum(v), padL - 4, YS(v) + 3, 'right'); }
    text(ctx, th, 'spread (ticks)', padL + 4, spY0 + 10, 'left', th.text);
    ctx.strokeStyle = th.ask; ctx.lineWidth = 1.2; ctx.beginPath(); started = false;
    let prevY = 0;
    for (let i = 0; i < model.S; i++) {
      if (model.spread[i] === null) continue;
      const x = X(model.snapT[i]), y = YS(model.spread[i]);
      if (!started) { ctx.moveTo(x, y); started = true; } else { ctx.lineTo(x, prevY); ctx.lineTo(x, y); } // step line
      prevY = y;
    }
    ctx.stroke();
    // time axis
    for (const v of niceTicks(0, model.duration / 1e6, 5)) text(ctx, th, formatNum(v, v < 10 ? 1 : 0) + ' ms', X(v * 1e6), h - 5, 'center');
    // cursor
    const t = model.ops[Math.max(0, Math.min(model.N - 1, Math.floor(applied) - 1))][0];
    ctx.strokeStyle = th.cursor; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(X(t), padT); ctx.lineTo(X(t), h - padB); ctx.stroke();
  }

  // Order-flow mix per slice of the run, with a cursor.
  function drawFlow(ctx, w, h, model, applied, th) {
    th = th || DEFAULT_THEME;
    clear(ctx, w, h, th);
    const padL = 36, padR = 10, padT = 18, padB = 16, pw = w - padL - padR, ph = h - padT - padB;
    const B = model.flow.length, bw = pw / B;
    let max = 1;
    for (const f of model.flow) max = Math.max(max, f.passive + f.aggressive + f.cancels + f.modifies);
    const Y = (v) => padT + ph - (v / max) * ph;
    const series = [['passive', th.passive, 'passive submits'], ['aggressive', th.aggressive, 'aggressive submits'], ['cancels', th.cancel, 'cancels'], ['modifies', th.modify, 'modifies']];
    for (let b = 0; b < B; b++) {
      let acc = 0;
      for (const [k, color] of series) {
        const v = model.flow[b][k];
        if (v <= 0) continue;
        ctx.fillStyle = color; ctx.fillRect(padL + b * bw, Y(acc + v), Math.max(1, bw - 1), Y(acc) - Y(acc + v));
        acc += v;
      }
    }
    ctx.strokeStyle = th.text; ctx.lineWidth = 1.2; ctx.beginPath();
    for (let b = 0; b < B; b++) { const x = padL + (b + 0.5) * bw, y = Y(model.flow[b].trades); if (b) ctx.lineTo(x, y); else ctx.moveTo(x, y); }
    ctx.stroke();
    let lx = padL;
    for (const [, color, label] of series.concat([['', th.text, 'trades (line)']])) {
      ctx.fillStyle = color; ctx.fillRect(lx, 5, 8, 8); text(ctx, th, label, lx + 11, 12); lx += 14 + label.length * 6;
    }
    for (const v of niceTicks(0, max, 2)) text(ctx, th, formatNum(v), padL - 4, Y(v) + 3, 'right');
    const cx = padL + (Math.max(0, Math.min(model.N, applied)) / model.N) * pw;
    ctx.strokeStyle = th.cursor; ctx.beginPath(); ctx.moveTo(cx, padT); ctx.lineTo(cx, padT + ph); ctx.stroke();
    text(ctx, th, 'ops per slice of the run (' + formatNum(model.N / B, 0) + ' ops each)', padL, h - 3);
  }

  // Latency histogram from one entry of LOB_LATENCY.sources (log2 buckets of ns).
  function drawLatency(ctx, w, h, src, th) {
    th = th || DEFAULT_THEME;
    clear(ctx, w, h, th);
    if (!src) { text(ctx, th, 'no latency data', w / 2, h / 2, 'center'); return; }
    const counts = src.histogram_log2_ns, total = counts.reduce((a, b) => a + b, 0) || 1;
    let first = counts.findIndex((c) => c > 0), last = counts.length - 1;
    while (last > 0 && counts[last] === 0) last--;
    if (first < 0) first = 0;
    first = Math.max(0, first - 1); last = Math.min(counts.length - 1, last + 1);
    const padL = 44, padR = 10, padT = 52, padB = 28, pw = w - padL - padR, ph = h - padT - padB;
    const n = last - first + 1, bw = pw / n;
    const frac = counts.map((c) => c / total);
    let fmax = 0.01;
    for (let b = first; b <= last; b++) fmax = Math.max(fmax, frac[b]);
    const Y = (f) => padT + ph - (f / fmax) * ph;
    const XB = (b) => padL + (b - first) * bw;
    for (const v of niceTicks(0, fmax * 100, 3)) { ctx.strokeStyle = th.grid; ctx.beginPath(); ctx.moveTo(padL, Y(v / 100)); ctx.lineTo(w - padR, Y(v / 100)); ctx.stroke(); text(ctx, th, formatNum(v) + '%', padL - 4, Y(v / 100) + 3, 'right'); }
    for (let b = first; b <= last; b++) {
      ctx.fillStyle = th.accent; ctx.globalAlpha = 0.8;
      ctx.fillRect(XB(b) + 1, Y(frac[b]), Math.max(1, bw - 2), Y(0) - Y(frac[b]));
      ctx.globalAlpha = 1;
      if ((b - first) % 2 === 0) text(ctx, th, formatNs(b === 0 ? 0 : Math.pow(2, b - 1)), XB(b), h - 14, 'left');
    }
    const mark = (ns, label, color, row) => {
      if (!(ns > 0)) return;
      const pos = Math.log2(ns) + 1; // bucket b covers [2^(b-1), 2^b)
      if (pos < first || pos > last + 1) return;
      const x = padL + (pos - first) * bw;
      ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.beginPath(); ctx.moveTo(x, padT); ctx.lineTo(x, padT + ph); ctx.stroke();
      text(ctx, th, label + ' ' + formatNs(ns), x + 3, 24 + row * 12, 'left', color);
      ctx.beginPath(); ctx.moveTo(x, 28 + row * 12 - 14); ctx.lineTo(x, padT); ctx.stroke();
    };
    mark(src.p50_ns, 'p50', th.bid, 0);
    mark(src.p99_ns, 'p99', th.aggressive, 1);
    mark(src.p99_9_ns, 'p99.9', th.ask, 2);
    text(ctx, th, 'share of operations by latency bucket (log2 ns axis)', padL, 12, 'left', th.text);
  }

  // ------------------------------------------------------------------------- playback
  class Player {
    constructor(model) {
      this.model = model; this.cursor = 0; this.playing = false; this.speed = 300; // ops per second
    }
    get applied() { return Math.floor(this.cursor); }
    seek(x) { this.cursor = Math.max(0, Math.min(this.model.N, x)); if (this.cursor >= this.model.N) this.playing = false; }
    play() { if (this.cursor >= this.model.N) this.cursor = 0; this.playing = true; }
    pause() { this.playing = false; }
    toggle() { if (this.playing) this.pause(); else this.play(); }
    setSpeed(opsPerSec) { this.speed = Math.max(1, opsPerSec); }
    tick(dtMs) { if (this.playing) this.seek(this.cursor + (this.speed * dtMs) / 1000); }
    // Step to the previous/next recorded snapshot (the finest granularity the export has).
    step(dir) {
      this.pause();
      const i = snapshotIndexAt(this.model, this.applied);
      let j = i;
      if (dir > 0) j = Math.min(this.model.S - 1, i + 1);
      else j = this.model.snapOp[i] === this.applied ? Math.max(0, i - 1) : i;
      this.seek(this.model.snapOp[j]);
    }
  }

  // ----------------------------------------------------------------------------- app
  function themeFrom(win, doc) {
    const th = Object.assign({}, DEFAULT_THEME);
    try {
      const cs = win.getComputedStyle(doc.documentElement);
      const map = { text: '--text', muted: '--muted', grid: '--grid', bg: '--panel', bid: '--bid', ask: '--ask', accent: '--accent', passive: '--passive', aggressive: '--aggressive', cancel: '--cancel', modify: '--modify', cursor: '--cursor' };
      for (const k of Object.keys(map)) { const v = cs.getPropertyValue(map[k]); if (v && v.trim()) th[k] = v.trim(); }
    } catch (e) { /* keep defaults */ }
    return th;
  }

  function sizeCanvas(cv, win) {
    const r = cv.getBoundingClientRect();
    const dpr = (win && win.devicePixelRatio) || 1;
    const w = Math.max(50, Math.floor(r.width)), h = Math.max(50, Math.floor(r.height));
    if (cv.width !== Math.floor(w * dpr) || cv.height !== Math.floor(h * dpr)) { cv.width = Math.floor(w * dpr); cv.height = Math.floor(h * dpr); }
    const ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx, w, h };
  }

  function init(doc, win) {
    const $ = (id) => doc.getElementById(id);
    const el = {
      depth: $('cv-depth'), series: $('cv-series'), flow: $('cv-flow'), latency: $('cv-latency'),
      play: $('btn-play'), prev: $('btn-prev'), next: $('btn-next'), start: $('btn-start'), end: $('btn-end'),
      speed: $('speed'), scrub: $('scrub'), readout: $('readout'), tape: $('tape-body'), stats: $('stats-body'),
      runInfo: $('run-info'), file: $('file-input'), status: $('status'),
      latStudy: $('lat-study'), latWl: $('lat-workload'), latVar: $('lat-variant'), latInfo: $('lat-info'),
    };
    let model = null, player = null, th = themeFrom(win, doc), depthState = { yMax: 0 }, lastTapeKey = '', dirty = true, last = null;
    let latSrc = null;

    function setStatus(msg) { if (el.status) el.status.textContent = msg || ''; }

    function load(run) {
      model = prepare(run);
      player = new Player(model);
      player.setSpeed(Number(el.speed.value) || 300);
      depthState = { yMax: 0 };
      el.scrub.min = 0; el.scrub.max = model.N; el.scrub.step = 1; el.scrub.value = 0;
      const m = model.meta;
      el.runInfo.textContent = [
        'source ' + (m.source || 'simulated'), 'seed ' + m.seed, m.ops + ' ops', formatNs(m.duration_ns) + ' of market time',
        m.arrival_rate_per_sec + ' arrivals/s', 'cancel ' + m.cancel_fraction, 'modify ' + m.modify_fraction, 'aggressiveness ' + m.aggressiveness,
        'depth ' + m.depth_dist + ' (mean ' + m.depth_mean_ticks + ' ticks)', 'snapshot every ' + m.snapshot_every + ' ops', 'checksum ' + m.checksum,
      ].join(' · ');
      lastTapeKey = ''; dirty = true;
      setStatus('');
    }

    function drawAll() {
      if (!model) return;
      const applied = player.applied;
      const si = snapshotIndexAt(model, applied);
      let c = sizeCanvas(el.depth, win); drawDepth(c.ctx, c.w, c.h, model.snaps[si], depthState, th);
      c = sizeCanvas(el.series, win); drawSeries(c.ctx, c.w, c.h, model, applied, th);
      c = sizeCanvas(el.flow, win); drawFlow(c.ctx, c.w, c.h, model, applied, th);
      c = sizeCanvas(el.latency, win); drawLatency(c.ctx, c.w, c.h, latSrc, th);

      const bb = model.bestBid[si], ba = model.bestAsk[si];
      const t = applied > 0 ? model.ops[applied - 1][0] : 0;
      el.readout.textContent = 'op ' + applied + ' / ' + model.N + '   t = ' + formatNs(t) + '   bid ' + (bb === null ? '-' : bb) + '   ask ' + (ba === null ? '-' : ba) +
        '   spread ' + (bb !== null && ba !== null ? ba - bb : '-') + '   mid ' + (bb !== null && ba !== null ? (bb + ba) / 2 : '-');
      el.scrub.value = applied;
      el.play.textContent = player.playing ? 'Pause' : 'Play';

      const n = tradeCountAt(model, applied), key = String(n);
      if (key !== lastTapeKey) {
        lastTapeKey = key;
        const rows = [];
        for (let i = n - 1; i >= Math.max(0, n - 16); i--) {
          const tr = model.tradeList[i];
          rows.push('<tr class="' + (tr.side === 0 ? 'buy' : 'sell') + '"><td>' + formatNs(tr.t) + '</td><td>' + tr.price + '</td><td>' + tr.qty + '</td><td>' + sideName(tr.side) + '</td></tr>');
        }
        el.tape.innerHTML = rows.join('') || '<tr><td colspan="4">no trades yet</td></tr>';
      }
      const s = windowStats(model, applied, 500);
      el.stats.innerHTML = [
        ['window', 'last ' + s.ops + ' ops'], ['arrivals / s (market time)', formatNum(s.opsPerSec, 0)],
        ['submits', s.submits], ['aggressive share', formatNum(100 * s.aggressiveShare, 1) + '%'], ['cancels', s.cancels],
        ['cancels per submit', formatNum(s.cancelToSubmit, 2)], ['modifies', s.modifies], ['trades', s.trades], ['traded volume', s.volume], ['rejects', s.rejects],
      ].map((r) => '<tr><th>' + r[0] + '</th><td>' + r[1] + '</td></tr>').join('');
    }

    function currentOpts() {
      const all = (win.LOB_LATENCY && win.LOB_LATENCY.sources) || [];
      return all.filter((s) => s.study === el.latStudy.value && s.workload === el.latWl.value);
    }

    // Uses the variant currently selected in the dropdown.
    function pickLatency() {
      const pick = currentOpts()[Number(el.latVar.value) || 0] || null;
      latSrc = pick;
      el.latInfo.textContent = pick
        ? pick.kind + '. ' + pick.samples + ' samples. Source ' + pick.file + ' (commit ' + pick.commit + '; ' + pick.environment.cpu + ', ' + pick.environment.os + '). Measured on a non-isolated desktop; these numbers are independent of the simulated run above.'
        : 'no latency data for this selection';
      dirty = true;
    }

    // Rebuilds the variant list for the current study/workload and selects a sensible default:
    // the final (most optimised) book step, or the lock-free SPSC queue.
    function chooseLatency() {
      const opts = currentOpts();
      el.latVar.innerHTML = opts.map((s, i) => '<option value="' + i + '">' + s.variant_title + '</option>').join('');
      let idx = opts.length - 1;
      if (el.latStudy.value === 'stage4') idx = Math.max(0, opts.findIndex((s) => s.variant === 'spsc'));
      el.latVar.value = String(Math.max(0, idx));
      pickLatency();
    }

    function setupLatency() {
      const all = (win.LOB_LATENCY && win.LOB_LATENCY.sources) || [];
      const studies = Array.from(new Set(all.map((s) => s.study)));
      const names = { stage3: 'Stage 3: book optimisation steps', stage4: 'Stage 4: queue designs' };
      el.latStudy.innerHTML = studies.map((s) => '<option value="' + s + '">' + (names[s] || s) + '</option>').join('');
      const wls = Array.from(new Set(all.map((s) => s.workload)));
      el.latWl.innerHTML = wls.map((s) => '<option value="' + s + '">' + s + '</option>').join('');
      if (wls.indexOf('balanced') >= 0) el.latWl.value = 'balanced';
      chooseLatency();
    }

    function frame(ts) {
      if (model) {
        const dt = last === null ? 0 : Math.min(100, ts - last);
        last = ts;
        const before = player.cursor;
        player.tick(dt);
        if (player.cursor !== before || dirty) { drawAll(); dirty = false; }
      }
      win.requestAnimationFrame(frame);
    }

    // controls
    el.play.addEventListener('click', () => { if (player) { player.toggle(); dirty = true; } });
    el.prev.addEventListener('click', () => { if (player) { player.step(-1); dirty = true; } });
    el.next.addEventListener('click', () => { if (player) { player.step(1); dirty = true; } });
    el.start.addEventListener('click', () => { if (player) { player.pause(); player.seek(0); dirty = true; } });
    el.end.addEventListener('click', () => { if (player) { player.pause(); player.seek(model.N); dirty = true; } });
    el.speed.addEventListener('change', () => { if (player) player.setSpeed(Number(el.speed.value)); });
    el.scrub.addEventListener('input', () => { if (player) { player.seek(Number(el.scrub.value)); dirty = true; } });
    el.latStudy.addEventListener('change', () => { chooseLatency(); });
    el.latWl.addEventListener('change', () => { chooseLatency(); });
    el.latVar.addEventListener('change', () => { pickLatency(); });
    doc.addEventListener('keydown', (e) => {
      if (!player || (e.target && /INPUT|SELECT/.test(e.target.tagName || '') && e.key !== ' ')) return;
      if (e.key === ' ') { e.preventDefault(); player.toggle(); }
      else if (e.key === 'ArrowRight') player.step(1);
      else if (e.key === 'ArrowLeft') player.step(-1);
      else if (e.key === 'Home') { player.pause(); player.seek(0); }
      else if (e.key === 'End') { player.pause(); player.seek(model.N); }
      dirty = true;
    });
    win.addEventListener('resize', () => { dirty = true; });
    el.file.addEventListener('change', () => {
      const f = el.file.files && el.file.files[0];
      if (!f) return;
      const reader = new win.FileReader();
      reader.onload = () => {
        try {
          let s = String(reader.result).trim();
          const m = /^window\.LOB_RUN\s*=\s*/.exec(s);
          if (m) s = s.slice(m[0].length).replace(/;\s*$/, '');
          load(JSON.parse(s));
        } catch (err) { setStatus('Could not load ' + f.name + ': ' + err.message); }
      };
      reader.readAsText(f);
    });

    setupLatency();
    if (win.LOB_RUN) {
      try { load(win.LOB_RUN); } catch (err) { setStatus('Sample run failed to load: ' + err.message); }
    } else setStatus('No sample run found. Use "Load run" to open a .json or .js export.');

    // optional URL hash: #at=0.5 (start position as a fraction), #play=0, #speed=1000
    const params = {};
    String((win.location && win.location.hash) || '').replace(/^#/, '').split('&').forEach((kv) => { const [k, v] = kv.split('='); if (k) params[k] = v; });
    if (model) {
      if (params.speed) { el.speed.value = params.speed; player.setSpeed(Number(params.speed)); }
      if (params.at) player.seek(Number(params.at) * model.N);
      if (params.play !== '0') player.play();
    }
    win.requestAnimationFrame(frame);
    return { get model() { return model; }, get player() { return player; }, drawAll, load, chooseLatency };
  }

  return { prepare, snapshotIndexAt, tradeCountAt, windowStats, drawDepth, drawSeries, drawFlow, drawLatency, Player, init, formatNs, DEFAULT_THEME };
});
