/* Synth1 Options dialog (COptionDlg, dialog 0xa6 + pages 0xa7 General / 0xa8 MIDI) for the web
 * replica. Model + renderer: the dialog client (502x438) is composed from parts cropped out of
 * captures of the original dialog under the reference harness (tasks/m4-panel/opt/cap, built by
 * tools/build_opt.js). Behaviour follows those captures (Wine 9.0 comctl32/user32):
 *  - checkbox: click toggles; the focus rectangle is shown only on a 2nd consecutive click on the
 *    same checkbox; ticking Panel/Text enables its colour edit and moves the focus there;
 *  - push buttons (- + Reset) and tab headers always show the focus rectangle when focused;
 *    OK shows the thick default border only while it has the focus (initial state);
 *  - Master Tuning trackbar range -100..+100 cents (TBM_SETRANGE 0x64ff9c, decomp 25006),
 *    A4 = 440 * 2^(c/1200) Hz;
 *  - Panel Size combo 80..250 % (step 10).
 * Settings keys = the original's ini keys (slidertype, transparencywin, hostselectprogram,
 * mastertune, panelcolor, textcolor (bit 31 = disabled), coordinatesscale).
 *
 *   const dlg = Synth1Opt.create(lib, settings)   // lib = {json, atlas: {width, data}}
 *   dlg.pointerDown(x, y) / pointerMove / pointerUp / key('Escape'|'Enter')  (client coords)
 *   dlg.render() -> {w, h, data}   (client RGBA)   dlg.listRender() -> combo list or null
 *   dlg.onClose(result|null), dlg.onAction(name)  (browse, update, load, save, edit)
 */
(function (root) {
  'use strict';

  const W = 502, H = 438;
  const SIZES = []; for (let s = 80; s <= 250; s += 10) SIZES.push(s);

  // Part rectangles [x0, y0, x1, y1) in client coords (screen - (549, 394)), from the dialog's
  // window dump (opt/cap/gen_winfo.txt) widened to cover focus rectangles; disjoint.
  const PARTS = {
    general: {
      tabs: [0, 0, 130, 28], ok: [144, 402, 234, 429], cancel: [269, 402, 359, 429],
      track: [22, 262, 216, 302], minus: [40, 303, 68, 329], plus: [68, 303, 97, 329],
      reset: [108, 303, 153, 329], cents: [25, 330, 107, 354], hz: [108, 330, 201, 354],
      chkPanel: [327, 264, 384, 287], editPanel: [391, 262, 457, 288],
      chkText: [327, 290, 384, 317], editText: [391, 289, 457, 317],
      chkSlider: [232, 327, 327, 353], chkTransp: [341, 327, 500, 353], chkHost: [232, 353, 411, 379],
      combo: [234, 280, 304, 305],
    },
    midi: {
      tabs: [0, 0, 130, 28], ok: [144, 402, 234, 429], cancel: [269, 402, 359, 429],
      chkBank: [288, 73, 450, 101], chkProg: [288, 101, 470, 129],
    },
  };
  // Hit rectangles (client coords) of the controls.
  const HIT = {
    tabGeneral: [2, 2, 62, 24], tabMidi: [62, 2, 118, 24],
    ok: [145, 403, 233, 428], cancel: [270, 403, 358, 428],
    browseBank: [408, 36, 478, 61], browseZip: [408, 213, 478, 238],
    track: [23, 263, 214, 300], minus: [41, 304, 67, 327], plus: [69, 304, 95, 327],
    reset: [109, 304, 151, 327], combo: [235, 281, 302, 303],
    chkPanel: [328, 265, 381, 284], editPanel: [392, 263, 455, 286],
    chkText: [328, 293, 382, 314], editText: [392, 291, 455, 314],
    chkSlider: [233, 328, 324, 351], chkTransp: [342, 328, 500, 351], chkHost: [233, 354, 408, 377],
    chkBank: [289, 74, 448, 99], chkProg: [289, 102, 468, 127],
    ccList: [21, 60, 259, 282], update: [135, 288, 191, 316], ccEdit: [198, 290, 230, 316],
    load: [319, 204, 368, 237], save: [380, 204, 431, 237],
  };
  const GENERAL_HITS = ['tabGeneral', 'tabMidi', 'ok', 'cancel', 'browseBank', 'browseZip', 'minus',
    'plus', 'reset', 'track', 'combo', 'chkPanel', 'editPanel', 'chkText', 'editText', 'chkSlider',
    'chkTransp', 'chkHost'];
  const MIDI_HITS = ['tabGeneral', 'tabMidi', 'ok', 'cancel', 'chkBank', 'chkProg', 'ccList',
    'update', 'ccEdit', 'load', 'save'];
  const CHK = { chkPanel: 'panel', chkText: 'text', chkSlider: 'slider', chkTransp: 'transp',
    chkHost: 'host', chkBank: 'bank', chkProg: 'prog' };
  // Trackbar geometry (Wine trackbar: channel inset, thumb 11 px): value -> thumb, x -> value.
  const TRACK = { x0: 23 + 13, x1: 23 + 178, vmin: -100, vmax: 100 };
  // Combo drop list (ComboLBox) relative to the combo's top-left: 65x252 client + 1 px border.
  const LIST = { dx: 0, dy: 22, w: 67, h: 254, rowh: 14, top: 1 };

  function inR(r, x, y) { return x >= r[0] && x < r[2] && y >= r[1] && y < r[3]; }

  function defaults() {
    return { slidertype: 1, transparencywin: 1, hostselectprogram: 0, mastertune: 0,
      panelcolor: 0x803c2828 | 0, textcolor: 0x80c8c8c8 | 0, coordinatesscale: 150,
      receivebank: 1, receiveprog: 1 };
  }
  function bgrHex(c) {
    const r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
    return '#' + [r, g, b].map((v) => v.toString(16).padStart(2, '0')).join('');
  }

  function create(lib, settings) {
    const S = Object.assign(defaults(), settings || {});
    const st = {
      tab: 'general', tabFocus: false, focus: 'ok', lastChk: null,
      chk: { panel: (S.panelcolor >>> 31) === 0, text: (S.textcolor >>> 31) === 0,
        slider: S.slidertype === 1, transp: !!S.transparencywin, host: !!S.hostselectprogram,
        bank: !!S.receivebank, prog: !!S.receiveprog },
      tune: Math.max(-100, Math.min(100, S.mastertune | 0)),
      size: SIZES.indexOf(S.coordinatesscale) >= 0 ? S.coordinatesscale : 100,
      panelcolor: S.panelcolor, textcolor: S.textcolor,
      comboOpen: false, comboHover: -1, dragTrack: false,
    };
    const dlg = { state: st, onClose: null, onAction: null, W: W, H: H };

    function result() {
      const flag = (on, c) => on ? (c & 0xffffff) : ((c & 0xffffff) | 0x80000000) | 0;
      return Object.assign({}, S, { slidertype: st.chk.slider ? 1 : 0,
        transparencywin: st.chk.transp ? 1 : 0, hostselectprogram: st.chk.host ? 1 : 0,
        mastertune: st.tune, coordinatesscale: st.size,
        panelcolor: flag(st.chk.panel, st.panelcolor), textcolor: flag(st.chk.text, st.textcolor),
        receivebank: st.chk.bank ? 1 : 0, receiveprog: st.chk.prog ? 1 : 0 });
    }
    function close(ok) { if (dlg.onClose) dlg.onClose(ok ? result() : null); }
    function act(name) { if (dlg.onAction) dlg.onAction(name); }
    function trackValue(x) {
      const v = Math.round(TRACK.vmin + (x - TRACK.x0) * (TRACK.vmax - TRACK.vmin) / (TRACK.x1 - TRACK.x0));
      return Math.max(TRACK.vmin, Math.min(TRACK.vmax, v));
    }
    function listHit(x, y) {
      const c = HIT.combo, lx = c[0] + LIST.dx, ly = c[1] + LIST.dy;
      if (x < lx || x >= lx + LIST.w || y < ly || y >= ly + LIST.h) return -2;
      const k = Math.floor((y - ly - LIST.top) / LIST.rowh);
      return k >= 0 && k < SIZES.length ? k : -1;
    }

    dlg.hitTest = function (x, y) {
      for (const n of (st.tab === 'general' ? GENERAL_HITS : MIDI_HITS)) if (inR(HIT[n], x, y)) return n;
      return null;
    };
    dlg.pointerDown = function (x, y) {
      if (st.comboOpen) {
        const k = listHit(x, y);
        if (k >= 0) st.size = SIZES[k];
        if (k !== -1) st.comboOpen = false;   // outside or on a row closes
        return;
      }
      const n = dlg.hitTest(x, y); if (!n) return;
      if (n === 'tabGeneral' || n === 'tabMidi') {
        st.tab = n === 'tabGeneral' ? 'general' : 'midi'; st.tabFocus = true; st.focus = 'tab';
        st.lastChk = null; return;
      }
      st.tabFocus = false;
      if (CHK[n]) {
        const k = CHK[n]; st.chk[k] = !st.chk[k];
        if ((k === 'panel' || k === 'text') && st.chk[k]) { st.focus = k === 'panel' ? 'editPanel' : 'editText'; st.lastChk = n; return; }
        st.focusVis = st.lastChk === n; st.focus = n; st.lastChk = n; return;
      }
      st.lastChk = null;
      if (n === 'plus') { st.tune = Math.min(100, st.tune + 1); st.focus = n; return; }
      if (n === 'minus') { st.tune = Math.max(-100, st.tune - 1); st.focus = n; return; }
      if (n === 'reset') { st.tune = 0; st.focus = n; return; }
      if (n === 'track') { st.tune = trackValue(x); st.focus = 'track'; st.dragTrack = true; return; }
      if (n === 'combo') { st.comboOpen = true; st.comboHover = SIZES.indexOf(st.size); st.focus = 'combo'; return; }
      if (n === 'ok') { close(true); return; }
      if (n === 'cancel') { close(false); return; }
      if (n === 'editPanel' && st.chk.panel) { st.focus = n; act('editPanel'); return; }
      if (n === 'editText' && st.chk.text) { st.focus = n; act('editText'); return; }
      if (n === 'browseBank' || n === 'browseZip' || n === 'update' || n === 'load' || n === 'save' ||
          n === 'ccList' || n === 'ccEdit') { st.focus = n; act(n); }
    };
    dlg.pointerMove = function (x, y) {
      if (st.dragTrack) { st.tune = trackValue(x); return; }
      if (st.comboOpen) { const k = listHit(x, y); if (k >= 0) st.comboHover = k; }
    };
    dlg.pointerUp = function () { st.dragTrack = false; };
    dlg.key = function (k) {
      if (k === 'Escape') { if (st.comboOpen) st.comboOpen = false; else close(false); }
      else if (k === 'Enter') { if (st.comboOpen) { st.size = SIZES[st.comboHover]; st.comboOpen = false; } else close(true); }
    };
    dlg.setColor = function (which, hex) {   // edit fields (#RRGGBB) -> BGR int
      const m = /^#?([0-9a-f]{6})$/i.exec(hex); if (!m) return false;
      const v = parseInt(m[1], 16), bgr = ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
      if (which === 'panel') st.panelcolor = bgr; else st.textcolor = bgr; return true;
    };
    dlg.colorHex = function (which) { return bgrHex(which === 'panel' ? st.panelcolor : st.textcolor); };

    /* Part state keys (must match tools/build_opt.js). */
    dlg.partKey = function (tab, name) {
      const f = st.focus;
      switch (name) {
        case 'tabs': return st.tab + (st.tabFocus ? ':f' : '');
        case 'ok': return f === 'ok' ? 'f' : '';
        case 'cancel': return '';
        case 'minus': case 'plus': case 'reset': return f === name ? 'f' : '';
        case 'track': case 'cents': case 'hz': return String(st.tune);
        case 'combo': return String(st.size);
        case 'editPanel': return st.chk.panel ? 'on' : 'off';
        case 'editText': return st.chk.text ? 'on' : 'off';
        default:
          if (CHK[name]) return (st.chk[CHK[name]] ? '1' : '0') + (f === name && st.focusVis ? 'f' : '');
      }
      return '';
    };

    dlg.render = function () {
      const J = lib.json, A = lib.atlas, out = new Uint8ClampedArray(W * H * 4);
      const base = J.base[st.tab];
      blit(out, W, A, base, 0, 0);
      const parts = PARTS[st.tab];
      for (const name in parts) {
        const r = parts[name], key = dlg.partKey(st.tab, name);
        const t = J.parts[st.tab + '.' + name + '.' + key];
        if (t) blit(out, W, A, t, r[0], r[1]);
      }
      return { w: W, h: H, data: out };
    };
    /* The open combo list (a separate popup window in the original), in client coords. */
    dlg.listRender = function () {
      if (!st.comboOpen) return null;
      const J = lib.json, A = lib.atlas, w = LIST.w, h = LIST.h, out = new Uint8ClampedArray(w * h * 4);
      blit(out, w, A, J.list.base, 0, 0);
      const cur = st.comboHover;
      const t = J.list.hi[cur]; if (t) blit(out, w, A, t, 1, LIST.top + cur * LIST.rowh);
      return { x: HIT.combo[0] + LIST.dx, y: HIT.combo[1] + LIST.dy, w: w, h: h, data: out };
    };
    return dlg;
  }

  function blit(dst, dw, A, t, dx, dy) {   // t = [ax, ay, w, h]
    for (let y = 0; y < t[3]; y++) for (let x = 0; x < t[2]; x++) {
      const s = ((t[1] + y) * A.width + t[0] + x) * 4, d = ((dy + y) * dw + dx + x) * 4;
      dst[d] = A.data[s]; dst[d + 1] = A.data[s + 1]; dst[d + 2] = A.data[s + 2]; dst[d + 3] = 255;
    }
  }

  const api = { create: create, SIZES: SIZES, PARTS: PARTS, HIT: HIT, LIST: LIST, W: W, H: H, defaults: defaults };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Synth1Opt = api;
})(typeof self !== 'undefined' ? self : this);
