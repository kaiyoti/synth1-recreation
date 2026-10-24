/* Synth1 v1.13 editor behaviour (M4 B3): an emulation of the original widget tree.
 *
 * The widget list (assets/widgets.json) is the editor's own canvas child array, dumped from the
 * live process (tasks/m4-gui-host `widgets`): class, id, hit rect and every raw field, in
 * hit-test order. The handlers below are ports of the decompiled 32-bit code (addresses cited);
 * state that the original keeps in widget fields is derived from the GUI parameter values.
 *
 *   const emu = Synth1Widgets.create(widgetsJson, {slidertype: 1})
 *   emu.values[p] = GUI int            // p = 0..98, 254 (master volume), 252 (program)
 *   emu.mouse('down'|'up'|'move'|'dblclick'|'rdown', x, y)   // PANEL coords (the 466x340 bitmap)
 *   emu.wheel(delta)                                          // to the slider popup
 * The editor's canvas window (CCWindow0, style 0x52800000) has WS_BORDER: canvas client
 * coords = panel coords - (1, 1). Widget rects and popup positions are canvas coords; popup
 * rects are reported in panel coords (popup.px, popup.py).
 *   emu.onCommand = (id, value) => {}  // the 0x709 commands (FUN_1002fa40): 1000..1098 params,
 *                                      // 1252 program, 1254 volume, 3/5 select window, 15 opt, 94 write
 *   emu.pressed  -> {id, half} of a held push button / spin, or null
 *   emu.popup    -> null | {kind: 'slider', x, y, w, h, min, max, value, ownerId}
 *                         | {kind: 'list', ownerId, ...}
 */
(function (root) {
  'use strict';

  function s32(v) { return v >= 0x80000000 ? v - 0x100000000 : v; }
  function cdiv(a, b) { return (a / b) < 0 ? Math.ceil(a / b) : Math.floor(a / b); }   // C int division
  function inRect(l, t, r, b, x, y) { return x >= l && x < r && y >= t && y < b; }    // PtInRect

  function create(W, opts) {
    opts = opts || {};
    const emu = {
      values: {}, pressed: null, popup: null, hover: -1, damage: [],
      slidertype: opts.slidertype === undefined ? 1 : opts.slidertype,
      onCommand: null, log: [],
      popupFrame: opts.popupFrame || { slider: [1, 1, 1, 1] }   // window border (l, t, r, b)
    };
    const byId = new Map();
    for (const w of W) { if (!byId.has(w.id)) byId.set(w.id, []); byId.get(w.id).push(w); }
    const F = (w, off) => { const v = w.f[String(off)]; return v && typeof v === 'object' ? v.ref : (v || 0); };
    const ref = (w, off) => { const v = w.f[String(off)]; return v && typeof v === 'object' ? W[v.ref] : null; };
    const R = (w) => w.rect;   // [left, top, right, bottom]
    const paramOf = (w) => (w.id >= 1000 ? w.id - 1000 : null);

    function send(id, value) {            // SendMessage(hwnd, 0x709, id, value) -> FUN_1002fa40
      emu.log.push([id, value]);
      if (id >= 1000 && id <= 1098) emu.values[id - 1000] = value;
      else if (id === 1254) emu.values[254] = value;
      else if (id === 1252) emu.values[252] = value;
      if (emu.onCommand) emu.onCommand(id, value);
    }

    /* ---- derived widget state (what the original keeps in +0x74/+0x78/+0x110/+0xe8) ---- */
    function ledLit(l) {                  // FUN_10012170: lit = (LED +0x80 == group value)
      const owner = ref(l, 0x8c);
      if (!owner) return F(l, 0x74) !== 0;
      const p = paramOf(owner);
      return p !== null && emu.values[p] === s32(F(l, 0x80));
    }
    function lcdIndex(w) {                // FUN_10012380 (list types): index of the value
      const p = paramOf(w), v = emu.values[p];
      const k = (w.items || []).findIndex((it) => it.value === v);
      return k >= 0 ? k : F(w, 0xe8);
    }
    function knobValue(w) {
      const p = w.id === 1254 ? 254 : paramOf(w);
      const v = emu.values[p];
      return v === undefined ? s32(F(w, 0x78)) : v;
    }

    /* ---- hit testing (canvas FUN_10011bd0: first child whose vt+8 hits and whose id != 0) ---- */
    function hit(w, x, y) {
      const r = R(w);
      switch (w.cls) {
        case 'CLED': {                     // FUN_1000dd10
          let l = r[0], ri = r[2];
          const ext = s32(F(w, 0x88));
          if (ext < 1) l += ext; else ri += ext;
          return inRect(l, r[1], ri, s32(F(w, 0x28)), x, y);
        }
        case 'CLEDMatrix': return false;   // FUN_10016c70: return 0
        default: return inRect(r[0], r[1], r[2], r[3], x, y);   // FUN_1000dcf0
      }
    }
    function find(x, y) {
      for (let i = 0; i < W.length; i++) { const w = W[i]; if (hit(w, x, y) && w.id !== 0) return i; }
      return -1;
    }

    /* ---- per-class handlers ---- */
    function pushDown(w) {                // FUN_10010500
      emu.pressed = { id: w.id, i: w.i };
      const mode = F(w, 0x78);
      if (mode === 1) {
        const lo = F(w, 0x7c), hi = F(w, 0x80);
        // FUN_1000fe00: first lit LED in id order lo..hi -> next id (wrap)
        let cur = hi;
        for (let id = lo; id <= hi; id++) {
          const l = (byId.get(id) || []).find((q) => q.cls === 'CLED');
          if (l) { if (ledLit(l)) { cur = id; break; } }
        }
        let next = cur + 1; if (next > hi) next = lo;
        const nl = (byId.get(next) || []).find((q) => q.cls === 'CLED');
        const value = nl ? s32(F(nl, 0x80)) : 0;   // FUN_1000fbb0: value of the lit LED
        send(w.id, value);
      } else if (mode === 2 || mode === 3) {
        const l = ref(w, 0x84); if (l) (mode === 2 ? lcdNext : lcdPrev)(l);
      } else send(w.id, 0);
    }
    function pushLed(owner, ledId) {      // FUN_10010fd0 (push button slot 0x30, from an LED click)
      if (F(owner, 0x78) !== 1) return;
      let l = (byId.get(ledId) || []).find((q) => q.cls === 'CLED');
      if (!l) return;
      if (F(owner, 0x88) !== 0 && ledLit(l)) {
        let alt = F(owner, 0x7c); if (ledId === alt) alt = F(owner, 0x80);
        l = (byId.get(alt) || []).find((q) => q.cls === 'CLED'); if (!l) return;
      }
      send(owner.id, s32(F(l, 0x80)));
    }
    function lcdNext(w) {                 // FUN_1000fd40
      const t = F(w, 0x98);
      if (t === 1) {
        const v = knobOrLcd(w), lo = s32(F(w, 0x118)), hi = s32(F(w, 0x11c));
        const n = Math.max(lo, Math.min(hi, v + 1)); if (n !== v) send(w.id, n);
      } else if (t === 2 || t === 3) {
        let k = lcdIndex(w) + 1; if (k >= w.items.length) k = 0; send(w.id, w.items[k].value);
      }
    }
    function lcdPrev(w) {                 // FUN_1000df40
      const t = F(w, 0x98);
      if (t === 1) {
        const v = knobOrLcd(w), lo = s32(F(w, 0x118)), hi = s32(F(w, 0x11c));
        const n = Math.max(lo, Math.min(hi, v - 1)); if (n !== v) send(w.id, n);
      } else if (t === 2 || t === 3) {
        let k = lcdIndex(w) - 1; if (k < 0) k = w.items.length - 1; send(w.id, w.items[k].value);
      }
    }
    function knobOrLcd(w) { const v = emu.values[paramOf(w)]; return v === undefined ? s32(F(w, 0x110)) : v; }
    function spinDown(w, x, y) {          // FUN_100106b0
      const r = R(w), link = ref(w, 0x88);
      let up;
      if (F(w, 0x84) === 0) up = ((r[2] + r[0]) / 2 | 0) < x;
      else up = y < ((r[3] + r[1]) / 2 | 0);
      emu.pressed = { id: w.id, i: w.i, half: up ? 1 : 2 };
      if (link) { (up ? lcdNext : lcdPrev)(link); return; }
      const p = w.id === 1252 ? 252 : paramOf(w);
      const old = emu.values[p] === undefined ? s32(F(w, 0x78)) : emu.values[p];
      const lo = s32(F(w, 0x80)), hi = s32(F(w, 0x7c));
      const v = Math.max(lo, Math.min(hi, old + (up ? 1 : -1)));
      if (v !== old) send(w.id, v);
    }
    function toggleDown(w) {              // FUN_10010790
      const p = paramOf(w);
      const cur = emu.values[p] === undefined ? F(w, 0x74) : (emu.values[p] ? 1 : 0);
      send(w.id, cur ? 0 : 1);
    }
    function ledDown(w) {                 // FUN_100104e0
      const owner = ref(w, 0x8c);
      if (owner && owner.cls === 'CPushButton') pushLed(owner, w.id);
    }
    function knobDown(w, x, y) {          // FUN_10010580
      const r = R(w), max = s32(F(w, 0x74));
      if (emu.slidertype === 1) {
        const px = F(w, 0x8c) === 0 ? r[2] : r[0] - 0x12;
        openSlider(w, px, r[1] - 0x3c, 0, max, knobValue(w));
      } else {
        emu.drag = { w: w, y0: y, min: 0, max: max, start: knobValue(w) };   // FUN_10012520
      }
    }
    function lcdDown(w, x, y) {           // FUN_10010150
      const kind = F(w, 0x94);
      if (kind === 0) { send(w.id, (x & 0xffff) | (y << 16)); return; }
      if (kind === 1) {
        if (F(w, 0x98) !== 1) return;
        const lo = s32(F(w, 0x118)), hi = s32(F(w, 0x11c)), v = knobOrLcd(w);
        if (emu.slidertype !== 1) { emu.drag = { w: w, y0: y, min: lo, max: hi, start: v }; return; }
        openSlider(w, R(w)[2], R(w)[1] - 0x3c, lo, hi, v);
        return;
      }
      if (kind === 2) openList(w);
    }

    /* ---- CVolSubWin popup (FUN_10010ba0 / paint FUN_10011800) ---- */
    function openSlider(owner, x, y, min, max, value) {
      const fr = emu.popupFrame.slider;
      const pop = { kind: 'slider', owner: owner, ownerId: owner.id, x: x, y: y, w: 0x12, h: 0x96,
                    min: min, max: max, value: value, last: value, dragging: true };
      pop.cl = x + fr[0]; pop.ct = y + fr[1]; pop.cw = pop.w - fr[0] - fr[2]; pop.ch = pop.h - fr[1] - fr[3];
      pop.px = x + 1; pop.py = y + 1;    // window rect in panel coords
      // WM_CREATE: cursor warped to (client.right - 2, top + (v-max)*(top-bottom)/(max-min))
      pop.warp = [pop.cl + pop.cw - 2 + 1, pop.ct + cdiv((value - max) * (0 - pop.ch), (max - min)) + 1];   // panel
      emu.popup = pop;
    }
    function sliderMove(pop, y) {
      const top = 0, bottom = pop.ch, ly = y - pop.ct;
      let v = cdiv((bottom - ly - top) * (pop.max - pop.min), (bottom - top)) + pop.min;
      v = Math.max(pop.min, Math.min(pop.max, v));
      pop.value = v;
      if (v !== pop.last) { ownerSet(pop.owner, v); pop.last = v; }
    }
    function ownerSet(w, v) {             // owner vt+0x2c: knob FUN_100124f0 / LCD FUN_100124a0
      if (w.cls === 'CRoteryV') send(w.id, v);
      else if (w.cls === 'CLCD') {
        if (F(w, 0x98) === 1) send(w.id, Math.max(s32(F(w, 0x118)), Math.min(s32(F(w, 0x11c)), v)));
        else send(w.id, v);
      }
    }
    function sliderMouse(type, x, y) {
      const pop = emu.popup;
      const inside = inRect(pop.cl, pop.ct, pop.cl + pop.cw, pop.ct + pop.ch, x, y);
      if (type === 'move') { if (pop.dragging) sliderMove(pop, y); return true; }
      if (type === 'down' || type === 'dblclick') {
        if (!inside) { emu.popup = null; return false; }   // ReleaseCapture + forward the click
        pop.dragging = true; sliderMove(pop, y); return true;
      }
      if (type === 'up') { pop.dragging = false; return true; }
      if (type === 'rdown') { emu.popup = null; return true; }
      return true;
    }

    /* ---- CLCDSubWin list popup (FUN_10010840 / paint FUN_1000e0f0) ---- */
    function openList(w) {
      const rows = F(w, 0xa4), n = w.items.length, cols = Math.floor(n / rows) + (n % rows ? 1 : 0);
      emu.popup = { kind: 'list', owner: w, ownerId: w.id, rows: rows, cols: cols, count: n,
                    current: lcdIndex(w), hover: lcdIndex(w),
                    geom: opts.listGeom ? opts.listGeom(w.id) : null };
    }
    function listCell(pop, x, y) {        // FUN_1000fac0 (cell under the cursor), needs geometry
      const g = pop.geom; if (!g) return -1;
      const lx = x - g.cl - g.mx, ly = y - g.ct - g.my;
      if (lx < 0 || ly < 0) return -1;
      const c = Math.floor(lx / g.cellw), r = Math.floor(ly / g.cellh);
      if (c >= pop.cols || r >= pop.rows) return -1;
      return c * pop.rows + r;
    }
    function listMouse(type, x, y) {
      const pop = emu.popup, g = pop.geom;
      const inside = g && inRect(g.cl, g.ct, g.cl + g.cw, g.ct + g.ch, x, y);
      if (type === 'move') { const k = listCell(pop, x, y); pop.hover = k < pop.count ? k : -1; return true; }
      if (type === 'down' || type === 'dblclick') {
        if (!inside) { emu.popup = null; return false; }
        if (pop.hover !== -1) { const it = pop.owner.items[pop.hover]; pop.current = pop.hover; send(pop.owner.id, it.value); }
        emu.popup = null; return true;
      }
      if (type === 'rdown') { emu.popup = null; return true; }
      return true;
    }

    /* ---- canvas mouse entry ---- */
    emu.mouse = function (type, px, py) {
      const x = px - 1, y = py - 1;      // panel -> canvas client coords
      if (emu.drag) {                     // slidertype 0 drag (FUN_10011a80)
        const d = emu.drag;
        if (type === 'up') { emu.drag = null; return; }
        if (type === 'move') {
          const iv = (y - d.y0) * (d.max - d.min);
          let v = d.start - cdiv(iv, 128);
          v = Math.max(d.min, Math.min(d.max, v));
          ownerSet(d.w, v);
        }
        return;
      }
      if (emu.popup) {
        const handled = emu.popup.kind === 'slider' ? sliderMouse(type, x, y) : listMouse(type, x, y);
        if (handled) return;
      }
      const i = find(x, y);
      const prev = emu.hover;
      if (prev !== -1 && prev !== i) leave(W[prev]);
      emu.hover = i;
      if (i < 0) return;
      const w = W[i];
      if (type === 'move') return;                       // slot 0x10: no-op for every class
      if (type === 'up') { up(w); return; }              // slot 0x18
      if (type === 'down' || type === 'dblclick') down(w, x, y);   // slot 0x14
    };
    emu.wheel = function (delta) {        // only the slider popup reacts (case 0x20a)
      const pop = emu.popup; if (!pop || pop.kind !== 'slider') return;
      let v = cdiv(delta, 0x78) + pop.value;
      v = Math.max(pop.min, Math.min(pop.max, v));
      pop.value = v; ownerSet(pop.owner, v); pop.last = v;
    };
    function leave(w) {                   // slot 0xc: push button / spin release (FUN_100107e0)
      if ((w.cls === 'CPushButton' || w.cls === 'CSpin') && emu.pressed && emu.pressed.i === w.i) {
        emu.pressed = null;
        const id = String(w.id), k = emu.damage.indexOf(id);   // its repaint now lies on top
        if (k >= 0) emu.damage.splice(k, 1);
        emu.damage.push(id);
      }
    }
    function up(w) {                      // slot 0x18: FUN_10010810 -> slot 0xc
      if (w.cls === 'CPushButton' || w.cls === 'CSpin') leave(w);
    }
    function down(w, x, y) {
      switch (w.cls) {
        case 'CPushButton': pushDown(w); break;
        case 'CToggleButton': toggleDown(w); break;
        case 'CLED': ledDown(w); break;
        case 'CSpin': spinDown(w, x, y); break;
        case 'CRoteryV': knobDown(w, x, y); break;
        case 'CLCD': lcdDown(w, x, y); break;
      }
    }
    emu.widgets = W;
    emu.find = find;
    emu.ledLit = ledLit;
    return emu;
  }

  const api = { create: create };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Synth1Widgets = api;
})(typeof self !== 'undefined' ? self : this);
