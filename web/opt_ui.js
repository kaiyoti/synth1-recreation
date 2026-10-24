/* DOM side of the Options dialog (model + pixels: panel_opt.js, assets opt/). Modal: a page
 * backdrop catches every pointer event and forwards it to the dialog (clicks outside the open
 * drop list close it, like the original's ComboLBox capture). Shown at the page's integer zoom.
 *   Synth1OptUI.open(panel, {base: 'opt/', onStatus(msg)})
 */
(function (root) {
  'use strict';
  let lib = null;

  async function loadLib(base) {
    if (lib) return lib;
    const V = '?v=' + Date.now();
    const json = await fetch(base + 'opt.json' + V).then((r) => r.json());
    const blob = await (await fetch(base + 'opt.png' + V)).blob();
    const bmp = await createImageBitmap(blob, { colorSpaceConversion: 'none', premultiplyAlpha: 'none' });
    const c = document.createElement('canvas'); c.width = bmp.width; c.height = bmp.height;
    const g = c.getContext('2d', { colorSpace: 'srgb' }); g.drawImage(bmp, 0, 0);
    lib = { json: json, atlas: { width: bmp.width, height: bmp.height, data: g.getImageData(0, 0, bmp.width, bmp.height).data } };
    return lib;
  }

  async function open(panel, o) {
    o = o || {};
    const L = await loadLib(o.base || 'opt/');
    const s0 = Object.assign({}, panel.settings);
    // the dialog shows the page's current panel size (URL ?scale) when it is one of its entries
    const pageSize = Math.round((panel.scale * panel.native) * (window.devicePixelRatio || 1)) * 100 / (window.devicePixelRatio || 1);
    if (Synth1Opt.SIZES.includes(Math.round(pageSize))) s0.coordinatesscale = Math.round(pageSize);
    const dlg = Synth1Opt.create(L, s0);
    const W = Synth1Opt.W, H = Synth1Opt.H, B = 1;   // 1 px window border (Wine draws no caption)
    // page zoom in whole device pixels, reduced until the dialog fits the viewport
    const dpr = window.devicePixelRatio || 1;
    let zd = Math.max(1, Math.round((panel.scale * panel.native) * dpr));
    while (zd > 1 && ((W + 2 * B) * zd / dpr > window.innerWidth || (H + 2 * B) * zd / dpr > window.innerHeight)) zd--;
    const z = zd / dpr;

    const back = document.createElement('div');
    Object.assign(back.style, { position: 'fixed', inset: '0', zIndex: 40, background: 'rgba(0,0,0,0)',
      touchAction: 'none', userSelect: 'none' });
    const cv = document.createElement('canvas'); cv.width = W + 2 * B; cv.height = H + 2 * B;
    Object.assign(cv.style, { position: 'absolute', width: (W + 2 * B) * z + 'px', height: (H + 2 * B) * z + 'px',
      imageRendering: 'pixelated', boxShadow: '2px 2px 8px rgba(0,0,0,.5)' });
    const lc = document.createElement('canvas');
    Object.assign(lc.style, { position: 'absolute', display: 'none', imageRendering: 'pixelated' });
    back.appendChild(cv); back.appendChild(lc); document.body.appendChild(back);
    // centred on the editor, kept inside the viewport
    const r = panel.canvas.getBoundingClientRect();
    const left = Math.max(0, Math.round(r.left + r.width / 2 - (W + 2 * B) * z / 2));
    const top = Math.max(0, Math.round(r.top + r.height / 2 - (H + 2 * B) * z / 2));
    cv.style.left = left + 'px'; cv.style.top = top + 'px';
    const g = cv.getContext('2d', { colorSpace: 'srgb' }), lg = lc.getContext('2d', { colorSpace: 'srgb' });

    // colour edit boxes: a real <input> over the edit control while it is being edited or holds
    // a value other than the captured one
    const inputs = {};
    for (const w of ['panel', 'text']) {
      const R = Synth1Opt.HIT[w === 'panel' ? 'editPanel' : 'editText'];
      const inp = document.createElement('input');
      Object.assign(inp.style, { position: 'absolute', display: 'none', boxSizing: 'border-box',
        left: left + (B + R[0] + 2) * z + 'px', top: top + (B + R[1] + 2) * z + 'px',
        width: (R[2] - R[0] - 4) * z + 'px', height: (R[3] - R[1] - 4) * z + 'px',
        font: (11 * z) + 'px Tahoma, Verdana, sans-serif', border: '0', padding: '0 ' + z + 'px', outline: 'none' });
      inp.maxLength = 7;
      inp.addEventListener('pointerdown', (e) => e.stopPropagation());
      inp.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Enter' || e.key === 'Escape') inp.blur(); });
      inp.addEventListener('blur', () => { if (editingNow === inp) editingNow = null; dlg.setColor(w, inp.value.trim()) || (inp.value = dlg.colorHex(w)); paint(); });
      back.appendChild(inp); inputs[w] = inp;
    }
    const captured = { panel: '#28283c', text: '#c8c8c8' };
    let editingNow = null;   // the edit being opened by the current click (focus lands after paint)

    function paint() {
      const f = dlg.render(), img = g.createImageData(W + 2 * B, H + 2 * B);
      img.data.fill(255);
      for (let i = 0; i < img.data.length; i += 4) { img.data[i] = img.data[i + 1] = img.data[i + 2] = 100; }
      for (let y = 0; y < H; y++) img.data.set(f.data.subarray(y * W * 4, (y + 1) * W * 4), ((y + B) * (W + 2 * B) + B) * 4);
      g.putImageData(img, 0, 0);
      const l = dlg.listRender();
      if (l) {
        if (lc.width !== l.w || lc.height !== l.h) { lc.width = l.w; lc.height = l.h; }
        lg.putImageData(new ImageData(l.data, l.w, l.h), 0, 0);
        Object.assign(lc.style, { display: 'block', width: l.w * z + 'px', height: l.h * z + 'px',
          left: left + (B + l.x) * z + 'px', top: top + (B + l.y) * z + 'px' });
      } else lc.style.display = 'none';
      for (const w of ['panel', 'text']) {
        const on = dlg.state.tab === 'general' && dlg.state.chk[w];
        const v = dlg.colorHex(w), editing = document.activeElement === inputs[w] || editingNow === inputs[w];
        inputs[w].style.display = on && (editing || v !== captured[w]) ? 'block' : 'none';
        if (!editing) inputs[w].value = v;
      }
    }
    const toClient = (e) => [Math.floor((e.clientX - left) / z) - B, Math.floor((e.clientY - top) / z) - B];
    back.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || e.target.tagName === 'INPUT') return;
      try { back.setPointerCapture(e.pointerId); } catch (err) { /* ignore */ }
      if (document.activeElement && document.activeElement.tagName === 'INPUT') document.activeElement.blur();
      const [x, y] = toClient(e); dlg.pointerDown(x, y); paint(); e.preventDefault();
    });
    back.addEventListener('pointermove', (e) => { const [x, y] = toClient(e); dlg.pointerMove(x, y); if (dlg.state.dragTrack || dlg.state.comboOpen) paint(); });
    back.addEventListener('pointerup', () => { dlg.pointerUp(); paint(); });
    back.addEventListener('contextmenu', (e) => e.preventDefault());
    function onKey(e) {
      if (document.activeElement && document.activeElement.tagName === 'INPUT') return;
      e.stopImmediatePropagation(); e.preventDefault();   // the computer keyboard must not play notes
      if (e.type === 'keydown' && (e.key === 'Escape' || e.key === 'Enter')) { dlg.key(e.key); paint(); }
    }
    window.addEventListener('keydown', onKey, true); window.addEventListener('keyup', onKey, true);

    dlg.onAction = function (name) {
      if (name === 'editPanel' || name === 'editText') {
        const inp = inputs[name === 'editPanel' ? 'panel' : 'text'];
        editingNow = inp; inp.style.display = 'block'; inp.focus(); inp.select();
      } else if (o.onStatus) o.onStatus(name + ': not available in the web version');
    };
    dlg.onClose = function (res) {
      window.removeEventListener('keydown', onKey, true); window.removeEventListener('keyup', onKey, true);
      back.remove();
      if (!res) return;
      const remount = panel.applySettings(res);
      const sizeChanged = res.coordinatesscale !== s0.coordinatesscale;
      if ((remount && /[?&]colors=1/.test(location.search)) || sizeChanged) {
        const q = new URLSearchParams(location.search);
        if (sizeChanged) q.set('scale', String(res.coordinatesscale / 100));
        location.search = q.toString();
      }
      if (o.onStatus && res.mastertune !== (s0.mastertune | 0)) o.onStatus('master tune ' + res.mastertune + ' cents (A4 = ' + (440 * Math.pow(2, res.mastertune / 1200)).toFixed(3) + ' Hz)');
      if (o.onStatus && remount && !/[?&]colors=1/.test(location.search)) o.onStatus('colours saved (panel preview pending verification)');
    };
    paint();
    return dlg;
  }

  root.Synth1OptUI = { open: open };
})(typeof self !== 'undefined' ? self : this);
