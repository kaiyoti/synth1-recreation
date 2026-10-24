/* Synth1 editor popups (M4 B3): the slider popup (CVolSubWin) and the list popup (CLCDSubWin),
 * drawn from window frames captured from the original editor (tasks/m4-panel/tools/
 * build_popups.py / build_lists.py). Popup windows are separate top-level windows in the
 * original; here they are drawn on the stage canvas at their panel position.
 *
 *   Synth1Popups.compose(popup, pops, atlas) -> {x, y, w, h, data}   (panel coords, RGBA)
 *   Synth1Popups.draw(ctx, pad, popup, pops, atlas, emu)
 */
(function (root) {
  'use strict';

  function blit(dst, dw, src, sw, sx, sy, w, h, dx, dy) {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const s = ((sy + y) * sw + sx + x) * 4, d = ((dy + y) * dw + dx + x) * 4;
      dst[d] = src[s]; dst[d + 1] = src[s + 1]; dst[d + 2] = src[s + 2]; dst[d + 3] = 255;
    }
  }

  function compose(p, pops, atlas) {
    if (p.kind === 'slider') {
      const e = pops.slider[String(p.ownerId)]; if (!e) return null;
      const ti = e.values[String(p.value)]; if (ti === undefined) return null;
      const t = pops.tiles[ti], data = new Uint8ClampedArray(t[2] * t[3] * 4);
      blit(data, t[2], atlas.data, atlas.width, t[0], t[1], t[2], t[3], 0, 0);
      return { x: e.win[0] + 1, y: e.win[1] + 1, w: t[2], h: t[3], data: data };
    }
    if (p.kind === 'list') {
      const e = pops.list[String(p.ownerId)]; if (!e) return null;
      const W = e.win[2], H = e.win[3], data = new Uint8ClampedArray(W * H * 4);
      const b = pops.tiles[e.base];
      blit(data, W, atlas.data, atlas.width, b[0], b[1], W, H, 0, 0);
      const ox = e.client[0] - e.win[0], oy = e.client[1] - e.win[1];
      for (let k = 0; k < e.count; k++) {
        const v = (k === p.current ? 2 : 0) + (k === p.hover ? 1 : 0);   // plain, hover, cur, cur+hover
        const ti = e.cells[k][v], t = pops.tiles[ti];   // tile size carries the edge-cell margin
        const c = Math.floor(k / e.rows), r = k % e.rows;
        const x0 = ox + 2 + c * e.cellw + 1, y0 = oy + 2 + r * e.cellh + 1;
        blit(data, W, atlas.data, atlas.width, t[0], t[1], t[2], t[3], x0, y0);
      }
      return { x: e.win[0] + 1, y: e.win[1] + 1, w: W, h: H, data: data };
    }
    return null;
  }

  function draw(ctx, pad, p, pops, atlas) {
    const c = compose(p, pops, atlas); if (!c) return;
    ctx.putImageData(new ImageData(c.data, c.w, c.h), pad + c.x, pad + c.y);
  }

  /* Geometry the emulator needs for list popups (cell hit-testing in canvas coords). */
  function listGeom(pops, ownerId) {
    const e = pops && pops.list[String(ownerId)]; if (!e) return null;
    return { cl: e.client[0], ct: e.client[1], cw: e.client[2], ch: e.client[3], mx: 2, my: 2,
             cellw: e.cellw, cellh: e.cellh };
  }

  const api = { compose: compose, draw: draw, listGeom: listGeom };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Synth1Popups = api;
})(typeof self !== 'undefined' ? self : this);
