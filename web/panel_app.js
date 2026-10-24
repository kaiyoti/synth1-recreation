/* Synth1 web panel (M4 B3): the pixel-exact editor in a <canvas>.
 * panel_core.js draws the 466x340 bitmap; panel_widgets.js runs the original editor's widget
 * logic; this file wires the DOM (pointer events, popups, presets) and an optional engine.
 *
 *   Synth1Panel.mount(rootEl, {assets: 'assets/', scale: 1, engine: {setParam(p, v)}})
 *     .loadSy1(text, programIndex, bankLabel)
 */
(function (root) {
  'use strict';

  async function rgba(url) {
    const blob = await (await fetch(url)).blob();
    const bmp = await createImageBitmap(blob, { colorSpaceConversion: 'none', premultiplyAlpha: 'none' });
    const c = document.createElement('canvas'); c.width = bmp.width; c.height = bmp.height;
    const g = c.getContext('2d', { colorSpace: 'srgb' }); g.drawImage(bmp, 0, 0);
    return { width: bmp.width, height: bmp.height, data: g.getImageData(0, 0, bmp.width, bmp.height).data };
  }



  function parseSy1(text) {             // first line = name, then "color=..", "ver=..", "id,value"
    const lines = text.split(/\r?\n/), values = {};
    for (const l of lines.slice(1)) {
      const m = /^(\d+),(-?\d+)\s*$/.exec(l);
      if (m) values[+m[1]] = +m[2];
    }
    return { name: lines[0].replace(/\s+$/, ''), values: values };
  }

  /* Options (the Opt dialog, web/panel_opt.js + opt_ui.js), saved per browser under the
     original's ini keys. */
  const OPT_KEY = 'synth1.opt';
  function loadSettings() {
    let s = {};
    try { s = JSON.parse(localStorage.getItem(OPT_KEY) || '{}') || {}; } catch (e) { s = {}; }
    return Object.assign(root.Synth1Opt ? Synth1Opt.defaults() : { slidertype: 1, transparencywin: 1 }, s);
  }
  function saveSettings(s) { try { localStorage.setItem(OPT_KEY, JSON.stringify(s)); } catch (e) { /* private mode */ } }

  async function mount(el, opts) {
    opts = opts || {};
    // asset set: the editor captured natively at 100 % (assets/) or 200 % (assets200/, crisp at 2x)
    const native = opts.native || ((opts.scale || 1) >= 2 && opts.have200 ? 2 : 1);
    // skins: web/skins/<name>/assets{,200}/ (same schema; e.g. ?skin=s80 = new artwork)
    const SK = opts.skin ? 'skins/' + opts.skin + '/' : '';
    const A = opts.assets || (SK + (native === 2 ? 'assets200/' : 'assets/'));
    const V = '?v=' + (opts.version || Date.now());   // no stale cached assets
    const [data, W, base, atlas, pops, selJson, banks] = await Promise.all([
      fetch(A + 'panel_data.json' + V).then((r) => r.json()),
      fetch(A + 'widgets.json' + V).then((r) => r.json()),
      rgba(A + 'base.png' + V), rgba(A + 'atlas.png' + V),
      fetch(A + 'popups.json' + V).then((r) => (r.ok ? r.json() : null)).catch(() => null),
      fetch(A + 'select.json' + V).then((r) => (r.ok ? r.json() : null)).catch(() => null),
      fetch(A + 'banks.json' + V).then((r) => (r.ok ? r.json() : null)).catch(() => null)]);
    const initSound = await fetch(A + 'init_sound.json' + V).then((r) => (r.ok ? r.json() : null)).catch(() => null);
    const popAtlas = pops ? await rgba(A + 'popups.png' + V) : null;
    const selChrome = selJson ? await rgba(A + 'select.png' + V) : null;
    const settings = loadSettings();
    const core = Synth1PanelCore.create(data, base.data, atlas.data, atlas.width);   // no colour themes (user 2026-10-23)
    let engine = opts.engine || null;     // set later by api.setEngine (audio needs a user gesture)
    const emu = Synth1Widgets.create(W, { slidertype: settings.slidertype === 0 ? 0 : 1,
      listGeom: (id) => (pops ? Synth1Popups.listGeom(pops, id) : null) });
    // CSS px per panel px, snapped so one panel pixel is a whole number of DEVICE pixels (a
    // fractional DPR such as 1.25/1.5 or browser zoom would otherwise resample and blur)
    const dpr = window.devicePixelRatio || 1;
    // CSS px per ASSET px (native 200 % assets are shown 1:1 at zoom 2)
    const scale = Math.max(1, Math.round(((opts.scale || 1) / native) * dpr)) / dpr;
    // the stage leaves room for popups that open above/beside the editor (CVolSubWin at top-60)
    // The editor bitmap sits at the top-left with no margin; popups (they may open above or beside
    // the editor) are drawn in an overlay canvas, shifted into the page when they would leave it.
    const PAD = 0;
    const cv = document.createElement('canvas');
    cv.width = data.width; cv.height = data.height;
    cv.style.width = data.width * scale + 'px'; cv.style.height = data.height * scale + 'px';
    cv.style.imageRendering = 'pixelated'; cv.style.touchAction = 'none';
    cv.style.display = 'block'; cv.style.position = 'relative'; cv.style.zIndex = 1;
    el.style.position = 'relative'; el.style.width = data.width * scale + 'px';
    // rounded outer corners of the instrument: panel top corners, keyboard bottom corners
    const RAD = Math.round(8 * native * scale) + 'px';
    cv.style.borderTopLeftRadius = RAD; cv.style.borderTopRightRadius = RAD;
    el.appendChild(cv);
    const ov = document.createElement('canvas');          // popup overlay
    Object.assign(ov.style, { position: 'absolute', display: 'none', imageRendering: 'pixelated', zIndex: 5,
      pointerEvents: 'none' });
    el.appendChild(ov);
    const og = ov.getContext('2d', { colorSpace: 'srgb' });
    let popShift = [0, 0], popRect = null;               // displayed-minus-true offset, displayed rect
    /* MIDI keyboard (tasks/m4-keyboard, B4) directly under the editor, exactly its width */
    let kb = null, kbDiv = null;
    if (root.Synth1Keyboard) {
      kbDiv = document.createElement('div');
      Object.assign(kbDiv.style, { width: data.width * scale + 'px', position: 'relative', zIndex: 2 });
      Object.assign(kbDiv.style, { borderBottomLeftRadius: RAD, borderBottomRightRadius: RAD, overflow: 'hidden' });
      if (opts.skin === 's80') {   // its own red frame (separate from the panel's), drawn inside its box
        const bw = Math.max(1, Math.round(4 * native * scale * (window.devicePixelRatio || 1))) / (window.devicePixelRatio || 1);
        // touching the panel; no top border (the panel's bottom border is the shared edge)
        Object.assign(kbDiv.style, { boxSizing: 'border-box', border: bw + 'px solid #ac162a', borderTop: '0', marginTop: '0' });
      }
      el.appendChild(kbDiv);
      kb = Synth1Keyboard.mount(kbDiv, { low: opts.kbLow || 36, high: opts.kbHigh || 96, height: Math.round(56 * native * scale),
        onNoteOn: (n, v) => { if (engine) engine.noteOn(n, v); if (opts.onNote) opts.onNote(n, v); },
        onNoteOff: (n) => { if (engine) engine.noteOff(n); if (opts.onNote) opts.onNote(n, 0); },
        onPitchBend: (v) => { if (engine && engine.pitchBend) engine.pitchBend(v); if (opts.onPitchBend) opts.onPitchBend(v); },
        onModWheel: (v) => { if (engine && engine.cc) engine.cc(1, v); if (opts.onModWheel) opts.onModWheel(v); } });
    }
    const g = cv.getContext('2d', { colorSpace: 'srgb' });
    const img = g.createImageData(data.width, data.height);
    const state = { values: emu.values, lcd: { program: '', bank: '' } };
    let dirty = true;

    function paint() {
      if (!dirty) return; dirty = false;
      const pr = emu.pressed;
      state.pressed = pr ? String(pr.id) + (pr.half === 1 ? 'u' : pr.half === 2 ? 'd' : '') : null;
      state.damage = emu.damage;
      core.render(state, img.data);
      g.putImageData(img, 0, 0);
      const p = emu.popup, c = p && pops && popAtlas ? Synth1Popups.compose(p, pops, popAtlas) : null;
      if (!c) { ov.style.display = 'none'; popShift = [0, 0]; popRect = null; return; }
      // keep the popup inside the page (left / top); the content is the original's
      const r = cv.getBoundingClientRect();
      const minX = Math.ceil(-(r.left + window.scrollX) / scale), minY = Math.ceil(-(r.top + window.scrollY) / scale);
      const dx = Math.max(c.x, minX), dy = Math.max(c.y, minY);
      popShift = [dx - c.x, dy - c.y]; popRect = [dx, dy, c.w, c.h];
      if (ov.width !== c.w || ov.height !== c.h) { ov.width = c.w; ov.height = c.h; }
      ov.style.width = c.w * scale + 'px'; ov.style.height = c.h * scale + 'px';
      ov.style.left = dx * scale + 'px'; ov.style.top = dy * scale + 'px'; ov.style.display = 'block';
      // WS_EX_LAYERED popups: alpha 191 (slider, CVolSubWin) / 204 (list, CLCDSubWin) on Windows
      // when "Transparency Window" is on; the Wine reference (and the pixel gates) are opaque
      ov.style.opacity = settings.transparencywin ? String((p.kind === 'list' ? 204 : 191) / 255) : '1';
      og.putImageData(new ImageData(c.data, c.w, c.h), 0, 0);
    }
    function loop() { paint(); requestAnimationFrame(loop); }
    requestAnimationFrame(loop);

    /* ---- program / bank state and the select window (CSoundSelectWin) ---- */
    const cur = { bank: 0, program: 0 };
    emu.values[254] = 64;                  // master volume: not per program (boot value 64)
    let sel = null, selCv = null, selG = null, selImg = null;
    if (selJson && banks && selChrome) {
      sel = Synth1Select.create(selJson, selChrome.data, banks);
      const [cw, ch] = sel.size;
      selCv = document.createElement('canvas'); selCv.width = cw + 2; selCv.height = ch + 2;
      Object.assign(selCv.style, { position: 'absolute', display: 'none', width: (cw + 2) * scale + 'px',
        height: (ch + 2) * scale + 'px', imageRendering: 'pixelated', zIndex: 10 });
      document.body.appendChild(selCv);
      selG = selCv.getContext('2d', { colorSpace: 'srgb' }); selImg = selG.createImageData(cw, ch);
      sel.onClose = () => { selCv.style.display = 'none'; };
      sel.onLoad = (b, k) => api.loadProgram(b, k);
      const toClient = (ev) => { const r = selCv.getBoundingClientRect();
        return [Math.floor((ev.clientX - r.left) / scale) - 1, Math.floor((ev.clientY - r.top) / scale) - 1]; };
      selCv.addEventListener('pointermove', (ev) => { sel.mouse('move', ...toClient(ev)); paintSel(); });
      selCv.addEventListener('pointerdown', (ev) => { sel.mouse(ev.button === 2 ? 'rdown' : 'down', ...toClient(ev)); paintSel(); ev.preventDefault(); });
      selCv.addEventListener('contextmenu', (ev) => ev.preventDefault());
      window.addEventListener('keydown', (ev) => { if (sel.isOpen) { sel.key(ev.key); paintSel(); ev.preventDefault(); } });
      document.addEventListener('pointerdown', (ev) => { if (sel.isOpen && ev.target !== selCv) sel.deactivate(); }, true);
    }
    function paintSel() {
      if (!sel || !sel.isOpen) return;
      const [cw, ch] = sel.size, f = sel.frame;
      selG.fillStyle = 'rgb(' + f.join(',') + ')'; selG.fillRect(0, 0, cw + 2, ch + 2);   // WS_BORDER
      selImg.data.set(sel.client); selG.putImageData(selImg, 1, 1);
      // bank list + heading from the bank data (names/counts after the reorganisation)
      const T = sel.tree(), FONT = '11px Tahoma, Verdana, "DejaVu Sans", sans-serif';
      selG.save(); selG.translate(1, 1); selG.textBaseline = 'middle';
      selG.fillStyle = '#fff'; selG.fillRect(6, 41, 247, Math.max(0, 16 * banks.length + 4));
      banks.forEach((bk, i) => {
        const r = T.items[i] ? T.items[i].rect : [25, 41 + 16 * i, 140, 56 + 16 * i];
        const label = String(bk.slot).padStart(2, '0') + ':' + bk.folder + ' (' + bk.count + ')';
        selG.font = FONT; const w = Math.ceil(selG.measureText(label).width) + 4;
        if (i === T.item) { selG.fillStyle = '#3399ff'; selG.fillRect(r[0] - 2, r[1], w, r[3] - r[1]); }
        selG.fillStyle = i === T.item ? '#fff' : '#000'; selG.fillText(label, r[0], (r[1] + r[3]) / 2 + 0.5);
      });
      const bk = banks[T.shown];
      if (bk) {
        selG.fillStyle = '#fff'; selG.fillRect(345, 9, 360, 22); selG.fillRect(820, 9, 140, 22);
        selG.fillStyle = '#000'; selG.font = 'bold 12px Tahoma, Verdana, "DejaVu Sans", sans-serif';
        selG.fillText('Bank' + String(bk.slot).padStart(2, '0') + ':' + bk.folder, 351, 20);
        selG.textAlign = 'right'; selG.fillText(bk.count + ' Programs', 951, 20); selG.textAlign = 'left';
      }
      selG.restore();
    }
    function openSelect() {                // centred on the editor window (FUN_1002fa40 case 3/5)
      if (!sel) return;
      sel.open(cur.bank, cur.program);
      const r = cv.getBoundingClientRect(), [cw, ch] = sel.size;
      const cx = r.left + window.scrollX + (PAD + data.width / 2) * scale, cy = r.top + window.scrollY + (PAD + data.height / 2) * scale;
      // centred on the editor like the original, but kept inside the page (no off-screen part)
      selCv.style.left = Math.max(window.scrollX, Math.round(cx - (cw + 2) * scale / 2)) + 'px';
      selCv.style.top = Math.max(window.scrollY, Math.round(cy - (ch + 2) * scale / 2)) + 'px';
      selCv.style.display = 'block'; paintSel();
    }

    emu.onCommand = function (id, value) {
      dirty = true;
      if (id === 3 || id === 5) { openSelect(); return; }
      if (id === 1252) { api.loadProgram(cur.bank, value); return; }
      if (id === 94) { api.writeSy1(); return; }          // 'write': download the patch as .sy1
      if (id === 15) { if (opts.onOpt) opts.onOpt(); return; }   // 'opt': the Options dialog
      if (engine) {
        if (id >= 1000 && id <= 1098) engine.setParam(id - 1000, value);
        else if (id === 1254) engine.setVolume(value);
      }
      if (opts.onCommand) opts.onCommand(id, value);
    };

    /* The original warps the cursor (SetCursorPos) when a popup opens: onto the slider value or
       the current list cell. A page cannot move the pointer, so while a popup is open the panel
       sees a virtual cursor = physical pointer + the warp offset (relative motion preserved). */
    let warp = [0, 0], popOpen = null, lastXY = [0, 0];
    function toPanel(ev) {
      const r = cv.getBoundingClientRect();
      let x = Math.floor((ev.clientX - r.left) / scale), y = Math.floor((ev.clientY - r.top) / scale);
      if (popRect && x >= popRect[0] && x < popRect[0] + popRect[2] && y >= popRect[1] && y < popRect[1] + popRect[3]) {
        x -= popShift[0]; y -= popShift[1];
      }
      lastXY = [x, y];
      return [x + warp[0], y + warp[1]];
    }
    function syncWarp() {
      const p = emu.popup;
      if (p && p !== popOpen) {
        let t = null;
        if (p.kind === 'slider') t = p.warp;
        else if (p.kind === 'list' && p.geom) {
          const k = p.current, c = Math.floor(k / p.rows), r = k % p.rows, g = p.geom;
          t = [g.cl + g.mx + c * g.cellw + (g.cellw >> 1) + 1, g.ct + g.my + r * g.cellh + (g.cellh >> 1) + 1];
        }
        if (t) warp = [t[0] - lastXY[0], t[1] - lastXY[1]];
      } else if (!p && popOpen) warp = [0, 0];
      popOpen = p;
    }
    let lastDown = 0;
    cv.addEventListener('pointerdown', (ev) => {
      const [x, y] = toPanel(ev);
      cv.setPointerCapture(ev.pointerId);
      if (ev.button === 2) emu.mouse('rdown', x, y);
      else if (ev.button === 0) {
        const now = performance.now();
        emu.mouse(now - lastDown < 500 ? 'dblclick' : 'down', x, y); lastDown = now;
      }
      syncWarp(); dirty = true; ev.preventDefault();
    });
    cv.addEventListener('pointermove', (ev) => { const [x, y] = toPanel(ev); emu.mouse('move', x, y); syncWarp(); dirty = true; });
    cv.addEventListener('pointerup', (ev) => { const [x, y] = toPanel(ev); if (ev.button === 0) emu.mouse('up', x, y); syncWarp(); dirty = true; });
    cv.addEventListener('wheel', (ev) => { emu.wheel(-Math.sign(ev.deltaY) * 120); dirty = true; ev.preventDefault(); }, { passive: false });
    cv.addEventListener('contextmenu', (ev) => ev.preventDefault());

    const api = {
      core: core, emu: emu, state: state, canvas: cv, keyboard: kb,
      setValues(v) { Object.assign(emu.values, v); dirty = true; },
      setLcd(program, bank) { state.lcd.program = program; state.lcd.bank = bank; dirty = true; },
      loadSy1(text, index0, bankLabel) {
        const s = parseSy1(text);
        api.setValues(s.values);
        cur.name = s.name;
        api.setLcd(Synth1PanelCore.programText(index0 || 0, s.name), bankLabel || Synth1PanelCore.bankText(0, 'bank0'));
        if (engine) engine.loadSy1(text);
        return s;
      },
      render() { dirty = true; paint(); },
      /* attach the audio engine: load the current program, then the panel's current values */
      setEngine(e) {
        engine = e;
        const p = banks && banks[cur.bank] && banks[cur.bank].programs[cur.program];
        if (p) e.loadSy1(p.sy1);
        for (let id = 0; id <= 98; id++) if (emu.values[id] !== undefined) e.setParam(id, emu.values[id]);
        if (emu.values[254] !== undefined) e.setVolume(emu.values[254]);
        if (e.setMasterTune) e.setMasterTune(settings.mastertune | 0);   // Options > Master Tuning
      },
      /* 'write' (CSoundSaveDlg -> FUN_1001b5b0): name, "color=<tag>", "ver=<n>", then "id,value"
         rows in the program's own row order, Windows text (CRLF); the web downloads the file */
      sy1Text() {
        const p = banks && banks[cur.bank] && banks[cur.bank].programs[cur.program];
        const head = p ? Synth1Select.sy1Head(p.sy1) : { name: cur.name || 'initial sound', color: 'default' };
        const name = cur.name || head.name, order = [];
        if (p) for (const l of p.sy1.split(/\r?\n/).slice(3)) { const m = /^(\d+),/.exec(l); if (m && !order.includes(+m[1])) order.push(+m[1]); }
        for (let id = 0; id <= 98; id++) if (!order.includes(id) && emu.values[id] !== undefined) order.push(id);
        const rows = order.filter((id) => emu.values[id] !== undefined).map((id) => id + ',' + emu.values[id]);
        return [name, 'color=' + head.color, 'ver=113'].concat(rows).join('\r\n') + '\r\n';
      },
      writeSy1() {
        const name = (cur.name || (banks && banks[cur.bank].programs[cur.program] ? Synth1Select.sy1Head(banks[cur.bank].programs[cur.program].sy1).name : 'initial sound'));
        const blob = new Blob([api.sy1Text()], { type: 'application/octet-stream' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = String(cur.program + 1).padStart(3, '0') + '_' + name.replace(/[\\/:*?"<>|]+/g, '_').trim() + '.sy1';
        document.body.appendChild(a); a.click(); a.remove();
        setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      },
      noteOn(n, v) { if (engine) engine.noteOn(n, v); },
      noteOff(n) { if (engine) engine.noteOff(n); },
      /* perf controls (tasks/m4-wasm-perf engine): CC, pitch bend, channel aftertouch */
      cc(c, v) { if (engine && engine.cc) engine.cc(c | 0, v | 0); },
      pitchBend(v) { if (engine && engine.pitchBend) engine.pitchBend(v | 0); },
      aftertouch(v) { if (engine && engine.aftertouch) engine.aftertouch(v | 0); },
      /* move the keyboard's wheels from an external source (Web MIDI) without emitting */
      setPitchBend(v) { return kb ? kb.setPitchBend(v) : 0; },
      setModWheel(v) { return kb ? kb.setModWheel(v) : 0; },
      banks: banks, current: cur, settings: settings, scale: scale, native: native,
      /* apply Options (OK): live ones now; colour / panel size need a re-mount (returns true) */
      applySettings(s) {
        const reload = (s.panelcolor | 0) !== (settings.panelcolor | 0) || (s.textcolor | 0) !== (settings.textcolor | 0);
        Object.assign(settings, s); saveSettings(settings);
        emu.slidertype = settings.slidertype === 0 ? 0 : 1;
        if (engine && engine.setMasterTune) engine.setMasterTune(settings.mastertune | 0);
        dirty = true;
        return reload;
      },
      /* load program k of factory bank b (index into banks.json) as the editor does: the .sy1
         values, the program LCD "%d:%s  " (name per the fgets(33) header rule), the bank LCD
         "%02d:%s  " (bank slot : folder) and the program spin's value. */
      loadProgram(b, k) {
        if (!banks || !banks[b]) return;
        const p = banks[b].programs[k];
        cur.bank = b; cur.program = k; cur.name = null; emu.values[252] = k;
        if (!p && initSound) {             // empty slot: the device loads its initial sound
          Object.assign(emu.values, Object.fromEntries(Object.entries(initSound.init_sound).map(([q, x]) => [+q, x])));
          if (engine) for (const q of Object.keys(initSound.init_sound)) engine.setParam(+q, initSound.init_sound[q]);
          api.setLcd(Synth1PanelCore.programText(k, 'initial sound'), Synth1PanelCore.bankText(banks[b].slot, banks[b].folder));
        }
        if (p) {
          // p.values = the core's loaded state (portable-synth preset load incl. the version-upgrade
          // chain #137, precomputed by tools/bank_values.py); the WASM engine will load p.sy1 itself
          const v = p.values || parseSy1(p.sy1).values, h = Synth1Select.sy1Head(p.sy1);
          Object.assign(emu.values, Object.fromEntries(Object.entries(v).map(([q, x]) => [+q, x])));
          if (engine) engine.loadSy1(p.sy1);   // the engine's own .sy1 loader (upgrade chain)
          api.setLcd(Synth1PanelCore.programText(k, h.name), Synth1PanelCore.bankText(banks[b].slot, banks[b].folder));
        }
        dirty = true;
      }
    };
    return api;
  }

  root.Synth1Panel = { mount: mount, parseSy1: parseSy1 };
})(typeof self !== 'undefined' ? self : this);
