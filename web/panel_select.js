/* Synth1 program / bank select window (CSoundSelectWin, WndProc FUN_100209b0, cell paint
 * FUN_1001e800) for the M4 web panel.
 *
 * The window keeps a persistent client bitmap, painted exactly like the original: a full paint
 * (chrome + cells 0..127 in index order) on open / bank switch, and single-cell repaints on hover
 * changes (so a repainted cell's text descenders land on the cell below, as in the original).
 * Chrome (tree, "List/Info", labels) is the captured client per selected factory bank; the grid
 * is modelled: Rectangle(x, y, x+181, y+19) black pen + fill (colour tag, normal / highlighted),
 * "%d:%s" in GDI+ Arial 18 px (Wine: GDI ExtTextOut subpixel glyphs, GDI advances, layout rect
 * (x+1, y-1, 178, 18) with 3 px margins -> word wrap at 172 px, first line only, no clip).
 *
 *   const sel = Synth1Select.create(selJson, chromeRGBA, banksJson)
 *   sel.open(bankIndex, programIndex)        // bankIndex into banksJson (= tree order)
 *   sel.mouse('move'|'down'|'rdown', x, y)   // client coords (0..984, 0..620)
 *   sel.key('Escape'|'ArrowUp'|'ArrowDown')
 *   sel.client -> RGBA (985 x 621); sel.isOpen; sel.onLoad = (bankIndex, program) => {}
 */
(function (root) {
  'use strict';
  function b64(s) {
    if (typeof Buffer !== 'undefined') return new Uint8Array(Buffer.from(s, 'base64'));
    const t = atob(s), o = new Uint8Array(t.length); for (let i = 0; i < t.length; i++) o[i] = t.charCodeAt(i); return o;
  }
  function create(S, chrome, banks) {
    // one tree row per bank (the captured tree had 11 rows; the reorganised set has more)
    if (banks && banks.length !== S.tree_items.length) {
      const t0 = S.tree_items[0].rect, rh = t0[3] - t0[1];
      S = Object.assign({}, S, { tree_items: banks.map((b, i) => S.tree_items[i] ? S.tree_items[i] :
        { rect: [t0[0], t0[1] + rh * i, t0[2] + 20, t0[3] + rh * i], chrome: S.tree_items[S.tree_items.length - 1].chrome, bank: i }) });
    }
    const CW = S.client[0], CH = S.client[1], g = S.grid;
    const glyph = {};
    for (const k of Object.keys(S.glyphs)) {
      const e = S.glyphs[k]; glyph[k] = { adv: e.adv, x: e.x, y: e.y, w: e.w, h: e.h, rgb: e.rgb ? b64(e.rgb) : null };
    }
    const sel = { isOpen: false, client: new Uint8ClampedArray(CW * CH * 4), onLoad: null, onClose: null };
    let shown = 0, curBank = 0, curProg = 0, hover = -1;
    let item = 0;                          // tree item index (S.tree_items: the banks; roots removed in the assets)
    const itemOfBank = (b) => S.tree_items.findIndex((t) => t.bank === b);
    const hasGrid = () => S.tree_items[item].bank !== null;   // +0x94: a bank with programs

    function adv(c) { const e = glyph[c]; return e ? e.adv : 0; }
    function firstLine(s, maxw) {          // Wine gdip_format_string word wrap (line 1)
      let ext = 0, fit = 0;
      for (let i = 0; i < s.length; i++) { const a = adv(s.charCodeAt(i)); if (ext + a > maxw) break; ext += a; fit++; }
      if (fit === 0) return '';
      const n = s.length;
      for (let i = 0; i < fit; i++) if (s[i] === '\n') return s.slice(0, i);
      if (fit < n) {
        const fitcpy = fit;
        if (s[fit] === ' ') { while (fit < n && s[fit] === ' ') fit++; }
        else {
          while (s[fit - 1] !== ' ') { fit--; if (s[fit] === '\t') break; if (fit === 0) { fit = fitcpy; break; } }
        }
        let end = fit; while (end > 0 && (s[end - 1] === ' ' || s[end - 1] === '\t')) end--;
        return s.slice(0, end);
      }
      return s.slice(0, fit);
    }
    function px(x, y, r, gg, b) {
      if (x < 0 || y < 0 || x >= CW || y >= CH) return;
      const p = (y * CW + x) * 4; sel.client[p] = r; sel.client[p + 1] = gg; sel.client[p + 2] = b; sel.client[p + 3] = 255;
    }
    function text(s, ox, oy, fg) {          // glyph offsets are relative to the cell origin
      let pen = 0;
      for (let i = 0; i < s.length; i++) {
        const e = glyph[s.charCodeAt(i)];
        if (e && e.rgb) {
          for (let yy = 0; yy < e.h; yy++) for (let xx = 0; xx < e.w; xx++) {
            const q = (yy * e.w + xx) * 3, ar = e.rgb[q], ag = e.rgb[q + 1], ab = e.rgb[q + 2];
            if (!(ar | ag | ab)) continue;
            const X = ox + pen + e.x + xx, Y = oy + e.y + yy;
            if (X < 0 || Y < 0 || X >= CW || Y >= CH) continue;
            const p = (Y * CW + X) * 4, d = sel.client;
            d[p] = ((fg[0] * ar + d[p] * (255 - ar) + 127) / 255) | 0;
            d[p + 1] = ((fg[1] * ag + d[p + 1] * (255 - ag) + 127) / 255) | 0;
            d[p + 2] = ((fg[2] * ab + d[p + 2] * (255 - ab) + 127) / 255) | 0;
          }
        }
        pen += adv(s.charCodeAt(i));
      }
    }
    function cellRect(k) { return [g.x + Math.floor(k / g.rows) * g.cw, g.y + (k % g.rows) * g.ch]; }
    function paintCell(k, high) {
      const [x, y] = cellRect(k), p = banks[shown].programs[k];
      const color = p ? p.color : 'default';
      const fill = (high ? S.fills.high : S.fills.normal)[color] || (high ? S.fills.high['default'] : S.fills.normal['default']);
      for (let yy = 0; yy < g.ch + 1; yy++) for (let xx = 0; xx < g.cw + 1; xx++) {
        const edge = yy === 0 || yy === g.ch || xx === 0 || xx === g.cw;
        if (edge) px(x + xx, y + yy, 0, 0, 0); else px(x + xx, y + yy, fill[0], fill[1], fill[2]);
      }
      let fg = [0, 0, 0];
      if (shown === curBank && k === curProg) fg = (fill[0] === 0 && fill[1] === 0 && fill[2] === 255) ? [255, 255, 255] : [0, 0, 255];
      const name = p ? p.name : 'initial sound';
      text(firstLine((k + 1) + ':' + name, g.wrap), x, y, fg);
    }
    function fullPaint() {
      const c = S.tree_items[item].chrome, off = c * CW * CH * 4;
      sel.client.set(chrome.subarray(off, off + CW * CH * 4));
      if (hasGrid()) for (let k = 0; k < g.cols * g.rows; k++) paintCell(k, k === hover);
    }
    function selectItem(i) {               // TVN_SELCHANGED
      if (i < 0 || i >= S.tree_items.length || i === item) return;
      item = i; if (hasGrid()) shown = S.tree_items[i].bank;
      fullPaint();
    }
    function cellAt(x, y) {                // FUN_1001ebb0
      const lx = x - g.x, ly = y - g.y;
      if (lx < 0 || lx >= g.cw * g.cols || ly < 0 || ly >= g.ch * g.rows) return -1;
      return Math.floor(ly / g.ch) + Math.floor(lx / g.cw) * g.rows;
    }
    function close() { sel.isOpen = false; if (sel.onClose) sel.onClose(); }
    // for the page to draw the bank list + heading from banks.json (the captured chrome shows the
    // original bank names; the banks were reorganised 2026-10-24)
    sel.tree = function () { return { item: item, shown: shown, items: S.tree_items }; };
    sel.open = function (bank, prog) {
      curBank = bank; curProg = prog; shown = bank; hover = prog;   // +0x23c = current program
      item = itemOfBank(bank);
      sel.isOpen = true; fullPaint();
    };
    sel.mouse = function (type, x, y) {
      if (!sel.isOpen) return;
      if (type === 'move') {               // WM_MOUSEMOVE: repaint the old and the new hover cell
        if (!hasGrid()) return;
        const k = cellAt(x, y);
        if (k !== hover) {
          if (hover !== -1) paintCell(hover, false);
          hover = k;
          if (k !== -1) paintCell(k, true);
        }
        return;
      }
      if (type === 'down') {
        for (let i = 0; i < S.tree_items.length; i++) {   // the TreeView child
          const r = S.tree_items[i].rect;
          if (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) { selectItem(i); return; }
        }
        if (x >= 6 && x < 257 && y >= 41 && y < 617) return;   // other clicks inside the tree
        if (hasGrid() && hover !== -1) {   // empty slots load too ('initial sound')
           const b = shown, k = hover; close(); if (sel.onLoad) sel.onLoad(b, k); return; }
        close(); return;
      }
      if (type === 'rdown') close();
    };
    sel.key = function (k) {
      if (!sel.isOpen) return;
      if (k === 'Escape') close();
      else if (k === 'ArrowDown') selectItem(item + 1);
      else if (k === 'ArrowUp') selectItem(item - 1);
    };
    sel.deactivate = function () { if (sel.isOpen) close(); };   // WM_ACTIVATE(0) -> close, no load
    sel.size = [CW, CH]; sel.frame = S.frame;
    return sel;
  }
  /* Synth1's .sy1 header read (inferred): fgets(33) for the name, colour only from the 2nd read. */
  function sy1Head(text) {
    function fgets(pos) { let e = pos; while (e < text.length && e - pos < 32) { e++; if (text[e - 1] === '\n') break; } return [text.slice(pos, e), e]; }
    const [l1, p] = fgets(0), [l2] = fgets(p);
    const c = l2.trim();
    return { name: l1.replace(/[\r\n]+$/, ''), color: c.startsWith('color=') ? c.slice(6) : 'default' };
  }
  const api = { create: create, sy1Head: sy1Head };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Synth1Select = api;
})(typeof self !== 'undefined' ? self : this);
