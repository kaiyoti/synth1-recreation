/* Synth1 v1.13 panel compositor (M4 B3). Pure JS, no DOM: renders the 466x340 editor bitmap
 * into an RGBA buffer, pixel-for-pixel as the original editor draws it under the reference host
 * (tasks/m4-gui-host, Wine 9.0). Shared by the browser panel and the Node pixel gate.
 *
 *   const core = Synth1PanelCore.create(data, baseRGBA, atlasRGBA)   // data = panel_data.json
 *   core.render(state, outRGBA)   // state = {values: {param: GUI int}, lcd: {program, bank},
 *                                 //          pressed: '<widget id>' | '<id>u' | '<id>d' (held),
 *                                 //          damage: [released widget ids, oldest first]}
 *
 * Controls: every parameter's widget pixels are a masked tile captured per value (base.png holds
 * every pixel no control ever changes). LCD text: FUN_10008ba0 (0x10008ba0) replayed exactly:
 * the background under the text box is pixel-replicated to 2x, the Wine "Lucida Console"
 * (weight 600, 2x height h2) glyphs are blended with Wine's subpixel blend_color
 * ((src*a + dst*(255-a) + 127) / 255 per channel, gamma 1000), and the 2x box is averaged back
 * 2x2 with >> 2. The box is the DrawText DT_CALCRECT width rounded up to even, clipped to the
 * LCD, and glyph pixels are clipped to the calc rect.
 */
(function (root) {
  'use strict';

  function b64bytes(s) {
    if (typeof Buffer !== 'undefined') return new Uint8Array(Buffer.from(s, 'base64'));
    const bin = atob(s), out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  function create(data, baseRGBA, atlasRGBA, atlasWidth) {
    const W = data.width, H = data.height;
    const controls = [];
    for (const key of Object.keys(data.controls)) {
      const c = data.controls[key];
      const bits = b64bytes(c.mask), idx = [];
      for (let i = 0; i < c.w * c.h; i++) if (bits[i >> 3] & (0x80 >> (i & 7))) idx.push(i);
      const dmap = new Map();
      if (c.domain) c.domain.forEach(function (d, k) { dmap.set(d, k); });
      controls.push({ param: c.param, x: c.x, y: c.y, w: c.w, h: c.h, idx: idx, tiles: c.tiles, dmap: c.domain ? dmap : null });
    }
    const fonts = {};
    for (const fn of Object.keys(data.fonts)) {
      const f = {};
      for (const code of Object.keys(data.fonts[fn])) {
        const g = data.fonts[fn][code];
        f[code] = { adv: g.adv, x: g.x, y: g.y, w: g.w, h: g.h, rgb: g.rgb ? b64bytes(g.rgb) : null };
      }
      fonts[fn] = f;
    }
    const fg = data.lcd_text_rgb;
    /* Push buttons / spins repaint their footprint (rect + 3 px shadow) when pressed and released,
       overwriting what lies there (the original's raster side effect): 'pressed' = the held look,
       'released' = the look a release leaves behind (state.damage = released ids, oldest first). */
    function tiles(src) {
      const o = {};
      for (const key of Object.keys(src || {})) {
        const t = src[key], bits = b64bytes(t.mask), rgb = b64bytes(t.rgb), idx = [];
        for (let i = 0; i < t.w * t.h; i++) if (bits[i >> 3] & (0x80 >> (i & 7))) idx.push(i);
        o[key] = { x: t.x, y: t.y, w: t.w, idx: idx, rgb: rgb };
      }
      return o;
    }
    const pressed = tiles(data.pressed), released = tiles(data.released);
    function blitTile(out, pk) {
      for (const i of pk.idx) {
        const p = ((pk.y + ((i / pk.w) | 0)) * W + pk.x + (i % pk.w)) * 4;
        out[p] = pk.rgb[i * 3]; out[p + 1] = pk.rgb[i * 3 + 1]; out[p + 2] = pk.rgb[i * 3 + 2];
      }
    }

    function textWidth(font, s) {
      let w = 0;
      for (let i = 0; i < s.length; i++) { const g = font[s.charCodeAt(i)]; if (g) w += g.adv; }
      return w;
    }

    function drawLcd(out, L, s) {
      const font = fonts[L.font], h2 = L.h2, left = 1, top = 1;
      if (!s) return;
      const calcW = textWidth(font, s);
      let w2 = calcW + (calcW & 1);
      w2 = Math.min(w2, (L.w - left) * 2);
      if (w2 <= 0) return;
      const ow = w2 >> 1, oh = h2 >> 1, ox = L.x + left, oy = L.y + top;
      const tmp = new Int32Array(w2 * h2 * 3);
      for (let y = 0; y < h2; y++) for (let x = 0; x < w2; x++) {
        const p = ((oy + (y >> 1)) * W + ox + (x >> 1)) * 4, q = (y * w2 + x) * 3;
        tmp[q] = out[p]; tmp[q + 1] = out[p + 1]; tmp[q + 2] = out[p + 2];
      }
      const clipW = Math.min(w2, calcW), clipH = h2;   // DT_CALCRECT rect = (0,0,calcW,tmHeight=h2)
      let pen = 0;
      for (let i = 0; i < s.length; i++) {
        const g = font[s.charCodeAt(i)];
        if (!g) continue;
        if (g.rgb) {
          const x0 = pen + g.x, y0 = g.y;
          for (let yy = 0; yy < g.h; yy++) {
            const ty = y0 + yy; if (ty < 0 || ty >= clipH) continue;
            for (let xx = 0; xx < g.w; xx++) {
              const tx = x0 + xx; if (tx < 0 || tx >= clipW) continue;
              const a = (yy * g.w + xx) * 3, ar = g.rgb[a], ag = g.rgb[a + 1], ab = g.rgb[a + 2];
              if (!(ar | ag | ab)) continue;
              const q = (ty * w2 + tx) * 3;
              tmp[q] = ((fg[0] * ar + tmp[q] * (255 - ar) + 127) / 255) | 0;
              tmp[q + 1] = ((fg[1] * ag + tmp[q + 1] * (255 - ag) + 127) / 255) | 0;
              tmp[q + 2] = ((fg[2] * ab + tmp[q + 2] * (255 - ab) + 127) / 255) | 0;
            }
          }
        }
        pen += g.adv;
      }
      for (let y = 0; y < oh; y++) for (let x = 0; x < ow; x++) {
        const a = ((2 * y) * w2 + 2 * x) * 3, b = a + 3, c = a + w2 * 3, d = c + 3;
        const p = ((oy + y) * W + ox + x) * 4;
        out[p] = (tmp[a] + tmp[b] + tmp[c] + tmp[d]) >> 2;
        out[p + 1] = (tmp[a + 1] + tmp[b + 1] + tmp[c + 1] + tmp[d + 1]) >> 2;
        out[p + 2] = (tmp[a + 2] + tmp[b + 2] + tmp[c + 2] + tmp[d + 2]) >> 2;
      }
    }

    function render(state, out) {
      out = out || new Uint8ClampedArray(W * H * 4);
      out.set(baseRGBA);
      for (const id of state.damage || []) if (released[id]) blitTile(out, released[id]);
      if (state.pressed && pressed[state.pressed]) blitTile(out, pressed[state.pressed]);
      const vals = state.values || {};
      for (const c of controls) {
        let v = vals[c.param];
        if (v === undefined) continue;
        if (c.dmap) { v = c.dmap.get(v | 0); if (v === undefined) continue; }   // GUI int -> tile
        else v = Math.max(0, Math.min(c.tiles.length - 1, v | 0));
        const t = c.tiles[v];
        for (const i of c.idx) {
          const lx = i % c.w, ly = (i / c.w) | 0;
          const p = ((c.y + ly) * W + c.x + lx) * 4, q = ((t[1] + ly) * atlasWidth + t[0] + lx) * 4;
          out[p] = atlasRGBA[q]; out[p + 1] = atlasRGBA[q + 1]; out[p + 2] = atlasRGBA[q + 2]; out[p + 3] = 255;
        }
      }
      const lcd = state.lcd || {};
      for (const L of data.lcds) if (lcd[L.id] !== undefined) drawLcd(out, L, lcd[L.id]);
      return out;
    }

    function domain(param) {
      for (const c of controls) if (c.param === param) return c.dmap ? Array.from(c.dmap.keys()) : null;
      return null;
    }

    return { render: render, width: W, height: H, domain: domain, textWidth: textWidth };
  }

  /* Display strings the editor builds for the two LCD lines (FUN @ 0x1002f... wsprintfA
     "%d:%s  " / "%02d:%s  "). */
  function programText(index0, name) { return (index0 + 1) + ':' + name + '  '; }
  function bankText(bank, name) { return (bank < 10 ? '0' : '') + bank + ':' + name + '  '; }

  const api = { create: create, programText: programText, bankText: bankText };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Synth1PanelCore = api;
})(typeof self !== 'undefined' ? self : this);
