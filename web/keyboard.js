/* Synth1 on-screen keyboard (M4 B4, + m4-wheels): playable piano with pitch-bend + modulation
 * wheels under the panel.
 *
 * The original Synth1 editor has no keyboard (tasks/m4-panel/web/assets/widgets.json has no key
 * widget), so this is a functional deliverable, not a pixel port. Two layers, like
 * panel_widgets.js (logic) / panel_app.js (DOM), same UMD shape.
 *
 *   Synth1Keyboard.create(opts)    -> pure logic model, no DOM (what gate.js drives)
 *   Synth1Keyboard.mount(el, opts) -> <canvas> + pointer/keyboard events wired to that model
 *
 * opts: {low:24, high:108, height:120, baseNote:60, octaveRange:4, wheels:true,
 *        onNoteOn(note,vel), onNoteOff(note), onPitchBend(v), onModWheel(v)}
 *
 * Model, coordinates keyboard-local CSS pixels (0,0 = canvas top-left, y grows down):
 *   k.layout(width, height)                    geometry (width = container width)
 *   k.noteAt(x, y)                             MIDI note under the point, null outside the keys
 *   k.velocityFromY(y)                         30 at the top .. 127 at the bottom, integer
 *   k.press(id,x,y) / k.move(id,x,y) / k.release(id)   pointer streams; move glissando
 *   k.keyDown(code, repeat) / k.keyUp(code)    computer keyboard (Z..M / Q..U rows, - / = octave)
 *   k.allNotesOff()                            window blur / tab hide
 * Each returns the array of {type:'on'|'off', note, vel} it produced and forwards them to the
 * callbacks. Notes are reference-counted per source (pointer id, "key:<code>"): a note-off fires
 * only when the last source lets go, and a pointer that drags off the keys releases what it held
 * (so a glissando can never leave a stuck note).
 *
 * Wheels (opts.wheels !== false): a strip WHEEL_W px wide at the left edge, pitch column left,
 * mod column right; the keys keep the remaining width. Same press/move/release stream — a pointer
 * that starts on a wheel drags it (pitch springs back to 0 on release, mod stays where left);
 * k.nudgeWheel(wheel, dir) is the mouse-wheel step (pitch 512, mod 7). k.pitchBend / k.modWheel
 * are the current values; k.setPitchBend(v) / k.setModWheel(v) are the external-MIDI setters:
 * they clamp and move the wheel visually (the model value changes, the view repaints) without
 * emitting the callback. Wheel events emit on change only: onPitchBend(-8192..8191, 0 = centre)
 * and onModWheel(0..127). No computer-keyboard binding (the tracker layout keeps every key).
 */
(function (root) {
  'use strict';

  var BLACK_PITCH = { 1: 1, 3: 1, 6: 1, 8: 1, 10: 1 };
  function isBlackNote(n) { return !!BLACK_PITCH[((n % 12) + 12) % 12]; }

  var V_MIN = 30, V_MAX = 127, BLACK_W = 0.62, BLACK_H = 0.62;

  /* Pitch-bend + modulation wheels (opts.wheels !== false): a strip WHEEL_W px wide at the left
     edge of the keyboard, pitch column left, mod column right, both full height. */
  var WHEEL_W = 56, PITCH_MIN = -8192, PITCH_MAX = 8191, PITCH_STEP = 512, MOD_MIN = 0, MOD_MAX = 127, MOD_STEP = 7;

  /* Computer keyboard, the tracker layout (white keys on the letter rows, black keys on the row
     above): bottom row Z..M = C..B from the base C, then , L . ; / = C C# D D# E of the next
     octave; top row Q..U (black keys on 2 3 5 6 7) = the next octave C..B, then I 9 O 0 P [ =
     C C# D D# E F. Octave shift: '-' / '=' (also numpad -/+). */
  var KEY_MAP = {
    KeyZ: 0, KeyS: 1, KeyX: 2, KeyD: 3, KeyC: 4, KeyV: 5, KeyG: 6, KeyB: 7, KeyH: 8, KeyN: 9,
    KeyJ: 10, KeyM: 11, Comma: 12, KeyL: 13, Period: 14, Semicolon: 15, Slash: 16,
    KeyQ: 12, Digit2: 13, KeyW: 14, Digit3: 15, KeyE: 16, KeyR: 17, Digit5: 18, KeyT: 19,
    Digit6: 20, KeyY: 21, Digit7: 22, KeyU: 23, KeyI: 24, Digit9: 25, KeyO: 26, Digit0: 27,
    KeyP: 28, BracketLeft: 29
  };
  var OCTAVE_KEYS = { Minus: -1, NumpadSubtract: -1, Equal: 1, NumpadAdd: 1 };

  function create(opts) {
    opts = opts || {};
    var low = opts.low === undefined ? 24 : (opts.low | 0);
    var high = opts.high === undefined ? 108 : (opts.high | 0);
    if (high < low) high = low;
    var baseNote = opts.baseNote === undefined ? 60 : (opts.baseNote | 0);
    var octRange = opts.octaveRange === undefined ? 4 : (opts.octaveRange | 0);
    var hasWheels = opts.wheels !== false;

    var whites = [], whiteOf = {}, blacks = [], boundaryOf = {}, boundaryBlack = {};
    for (var n = low; n <= high; n++) {
      if (isBlackNote(n)) { blacks.push(n); boundaryOf[n] = whites.length; boundaryBlack[whites.length] = n; }
      else { whiteOf[n] = whites.length; whites.push(n); }
    }

    var k = {
      low: low, high: high, whites: whites, blacks: blacks, blackBoundary: boundaryOf,
      width: 0, height: opts.height === undefined ? 120 : opts.height,
      whiteW: 0, blackW: 0, blackH: 0,
      wheels: hasWheels, wheelWidth: hasWheels ? WHEEL_W : 0,
      pitchBend: 0, modWheel: 0,
      baseNote: baseNote, octave: 0, keyVelocity: 100, log: [],
      onNoteOn: opts.onNoteOn || null, onNoteOff: opts.onNoteOff || null,
      onPitchBend: opts.onPitchBend || null, onModWheel: opts.onModWheel || null
    };

    k.layout = function (width, height) {
      if (width !== undefined) k.width = width;
      if (height !== undefined) k.height = height;
      k.whiteW = (k.width - k.wheelWidth) / whites.length;
      k.blackW = k.whiteW * BLACK_W;
      k.blackH = k.height * BLACK_H;
      return k;
    };

    k.noteAt = function (x, y) {
      if (k.width <= 0 || k.height <= 0) return null;
      if (!(x >= 0) || !(y >= 0) || x >= k.width || y >= k.height) return null;
      var xk = x - k.wheelWidth;
      if (xk < 0) return null;
      var slot = Math.floor(xk / k.whiteW);
      if (slot >= whites.length) slot = whites.length - 1;
      if (y < k.blackH) {
        var b = Math.round(xk / k.whiteW), nb = boundaryBlack[b];
        if (nb !== undefined) {
          var cx = b * k.whiteW, half = k.blackW / 2;
          if (xk >= cx - half && xk <= cx + half) return nb;
        }
      }
      return whites[slot];
    };

    k.velocityFromY = function (y) {
      var t = k.height <= 1 ? 1 : y / (k.height - 1);
      if (!(t > 0)) t = 0; else if (t > 1) t = 1;
      var v = Math.round(V_MIN + t * (V_MAX - V_MIN));
      return v < V_MIN ? V_MIN : (v > V_MAX ? V_MAX : v);
    };

    /* ---- wheels: pitch bend (springs to centre) + modulation (stays where left) ---- */
    var heldWheel = {};

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
    function setPitch(v, emit) {
      v = clamp(Math.round(v), PITCH_MIN, PITCH_MAX);
      if (v === k.pitchBend) return [];
      k.pitchBend = v;
      var ev = { type: 'pitch', value: v };
      k.log.push(ev);
      if (emit !== false && k.onPitchBend) k.onPitchBend(v);
      return [ev];
    }
    function setMod(v, emit) {
      v = clamp(Math.round(v), MOD_MIN, MOD_MAX);
      if (v === k.modWheel) return [];
      k.modWheel = v;
      var ev = { type: 'mod', value: v };
      k.log.push(ev);
      if (emit !== false && k.onModWheel) k.onModWheel(v);
      return [ev];
    }

    k.wheelAt = function (x, y) {
      if (!hasWheels || !(x >= 0) || x >= k.wheelWidth || !(y >= 0) || y >= k.height) return null;
      return x < k.wheelWidth / 2 ? 'pitch' : 'mod';
    };
    /* full travel over the wheel height: top = max, bottom = min, exact 0 at the centre row */
    k.pitchFromY = function (y) {
      var t = k.height <= 1 ? 1 : (k.height - 1 - y) / (k.height - 1);
      return clamp(Math.round(clamp(t, 0, 1) * (PITCH_MAX - PITCH_MIN + 1)) + PITCH_MIN, PITCH_MIN, PITCH_MAX);
    };
    k.modFromY = function (y) {
      var t = k.height <= 1 ? 1 : (k.height - 1 - y) / (k.height - 1);
      return clamp(Math.round(clamp(t, 0, 1) * MOD_MAX), MOD_MIN, MOD_MAX);
    };
    function wheelValue(wheel, y) { return wheel === 'pitch' ? k.pitchFromY(y) : k.modFromY(y); }
    function wheelSet(wheel, v, emit) { return wheel === 'pitch' ? setPitch(v, emit) : setMod(v, emit); }

    /* External MIDI setters: move the wheel (model value; the view repaints from it) without
       re-emitting, clamped to the wire range. */
    k.setPitchBend = function (v) { setPitch(v, false); return k.pitchBend; };
    k.setModWheel = function (v) { setMod(v, false); return k.modWheel; };
    /* Mouse wheel over a wheel column: dir +1 = away from the player (up = value up). */
    k.nudgeWheel = function (wheel, dir) {
      if (wheel !== 'pitch' && wheel !== 'mod') return [];
      return wheelSet(wheel, (wheel === 'pitch' ? k.pitchBend : k.modWheel) + (dir > 0 ? 1 : dir < 0 ? -1 : 0) * (wheel === 'pitch' ? PITCH_STEP : MOD_STEP), true);
    };

    /* ---- note reference counting; a source is a pointer id or "key:<code>" ---- */
    var active = {}, heldPointer = {}, heldKey = {};

    function emit(ev) {
      k.log.push(ev);
      if (ev.type === 'on') { if (k.onNoteOn) k.onNoteOn(ev.note, ev.vel); }
      else if (k.onNoteOff) k.onNoteOff(ev.note);
      return ev;
    }
    function pressNote(src, note, vel) {
      if (note === null || note === undefined) return [];
      var c = active[note] || 0;
      active[note] = c + 1;
      return c === 0 ? [emit({ type: 'on', note: note, vel: vel })] : [];
    }
    function releaseNote(src, note) {
      if (note === null || note === undefined) return [];
      var c = active[note] || 0;
      if (c <= 1) { delete active[note]; return [emit({ type: 'off', note: note, vel: 0 })]; }
      active[note] = c - 1;
      return [];
    }

    /* A pointer stays "down" (an entry in heldPointer) from pointerdown to pointerup even while it
       is over empty space; heldPointer[id] === null means down but off the keys, so a glissando
       that leaves the keyboard and comes back re-triggers, and pointerup always finds the source.
       A pointer that starts on a wheel (heldWheel) stays glued to that wheel until release. */
    k.press = function (id, x, y) {
      var w = k.wheelAt(x, y);
      if (w) {
        var outw = heldWheel.hasOwnProperty(id) ? [] : releaseNote('p' + id, heldPointer[id]);
        if (heldPointer.hasOwnProperty(id)) delete heldPointer[id];
        heldWheel[id] = w;
        return outw.concat(wheelSet(w, wheelValue(w, y), true));
      }
      var out = heldPointer.hasOwnProperty(id) ? releaseNote('p' + id, heldPointer[id]) : [];
      var note = k.noteAt(x, y);
      heldPointer[id] = note;
      return out.concat(note === null ? [] : pressNote('p' + id, note, k.velocityFromY(y)));
    };
    k.move = function (id, x, y) {
      if (heldWheel.hasOwnProperty(id)) return wheelSet(heldWheel[id], wheelValue(heldWheel[id], y), true);
      if (!heldPointer.hasOwnProperty(id)) return [];
      var cur = heldPointer[id], note = k.noteAt(x, y);
      if (note === cur) return [];
      var out = releaseNote('p' + id, cur);
      heldPointer[id] = note;
      return out.concat(note === null ? [] : pressNote('p' + id, note, k.velocityFromY(y)));
    };
    k.release = function (id) {
      if (heldWheel.hasOwnProperty(id)) {
        var w = heldWheel[id];
        delete heldWheel[id];
        return w === 'pitch' ? setPitch(0, true) : [];      // pitch springs back to centre; mod stays
      }
      if (!heldPointer.hasOwnProperty(id)) return [];
      var out = releaseNote('p' + id, heldPointer[id]);
      delete heldPointer[id];
      return out;
    };

    k.setOctave = function (o) { k.octave = Math.max(-octRange, Math.min(octRange, o | 0)); return k.octave; };
    k.keyDown = function (code, repeat) {
      if (OCTAVE_KEYS.hasOwnProperty(code)) return (k.setOctave(k.octave + OCTAVE_KEYS[code]), []);
      var off = KEY_MAP[code];
      if (off === undefined || repeat) return [];
      var note = baseNote + k.octave * 12 + off;
      if (note < low || note > high || heldKey[code] === note) return [];
      heldKey[code] = note;
      return pressNote('key:' + code, note, k.keyVelocity);
    };
    k.keyUp = function (code) {
      if (!heldKey.hasOwnProperty(code)) return [];
      var out = releaseNote('key:' + code, heldKey[code]);
      delete heldKey[code];
      return out;
    };

    k.allNotesOff = function () {
      var out = [], notes = Object.keys(active), i;
      for (i = 0; i < notes.length; i++) out.push(emit({ type: 'off', note: +notes[i], vel: 0 }));
      active = {}; heldPointer = {}; heldKey = {}; heldWheel = {};
      return out.concat(setPitch(0, true));              // a held pitch wheel springs home too
    };
    k.activeNotes = function () {
      return Object.keys(active).map(Number).sort(function (a, b) { return a - b; });
    };
    k.heldPointers = function () { return Object.keys(heldPointer).map(Number); };

    k.layout(0, k.height);
    return k;
  }

  /* ---- DOM layer ---- */
  function mount(el, opts) {
    opts = opts || {};
    var doc = opts.document || (typeof document !== 'undefined' ? document : null);
    var win = opts.window || (typeof window !== 'undefined' ? window : root);
    var k = create(opts);
    var cv = doc.createElement('canvas');
    cv.className = 'synth1-keyboard';
    var g = null, W = 0, H = k.height, dpr = 1, raf = null, dirty = true, lit = {}, activeWheel = {};

    function resize() {
      var w = opts.width || el.clientWidth || el.offsetWidth || 640;
      W = Math.max(1, Math.round(w)); H = Math.max(1, Math.round(k.height));
      dpr = (win && win.devicePixelRatio) || 1;
      cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
      if (cv.style) { cv.style.width = W + 'px'; cv.style.height = H + 'px'; cv.style.touchAction = 'none'; }
      k.layout(W, H);
      if (k.wheelWidth > 0) {
        k.wheelWidth = Math.round(k.wheelWidth / 2) * 2;   // even -> integer column boundaries
        k.layout(W, H);                                   // keys keep the remaining width
      }
      g = cv.getContext ? cv.getContext('2d') : null;
      if (g && g.setTransform) g.setTransform(dpr, 0, 0, dpr, 0, 0);
      dirty = true; paint();
    }

    function fill(x, y, w, h, color) {
      if (!g) return;
      g.fillStyle = color; g.fillRect(x, y, w, h);
    }
    /* half-integer / integer grid alignment keeps every 1px edge crisp at dpr=1 (hairline falls
       between device pixels); rounded up for slab edges, half for groove/cap ridges. */
    function axi(v) { return Math.round(v); }
    function axh(v) { return Math.round(v - 0.5) + 0.5; }
    function roundRectPath(x, y, w, h, r) {
      if (!g) return;
      g.beginPath();
      g.moveTo(x + r, y);
      g.lineTo(x + w - r, y); g.quadraticCurveTo(x + w, y, x + w, y + r);
      g.lineTo(x + w, y + h - r); g.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
      g.lineTo(x + r, y + h); g.quadraticCurveTo(x, y + h, x, y + h - r);
      g.lineTo(x, y + r); g.quadraticCurveTo(x, y, x + r, y);
      g.closePath();
    }
    /* Dark grooved wheel slabs (rocking ridges) with a raised cap per column: pitch left, mod
       right. The pitch slab gets centre notches and its centre ridge is highlighted at rest, the
       cap ridges sit at cx-4/cx/cx+4 so the middle one marks the centre. Cap colours match the
       keys: #f7f7f3 / #b9d7f2 (white), outline #7c7c74, touched cap #3d7ec4. */
    function paintWheels() {
      if (!g || k.wheelWidth <= 0) return;
      var HW = k.wheelWidth, colW = HW / 2, PAD = 6, CAP_W = 20, CAP_H = 22, c, i;
      var travel = Math.max(0, (H - CAP_H) / 2), midY = axi((H - 32) / 2), cyMid = (H - CAP_H) / 2 + CAP_H / 2;
      for (c = 0; c < 2; c++) {
        var isPitch = c === 0;
        var t = isPitch ? (k.pitchBend - (-8192)) / 16383 : k.modWheel / 127;
        var cx = c * colW + colW / 2;
        var slabW = colW - PAD * 2, slabX = axi(cx - slabW / 2), slabY = axi(PAD), slabH = Math.max(6, H - PAD * 2);
        roundRectPath(slabX, slabY, slabW, slabH, 4);
        g.fillStyle = '#141414'; g.fill();
        g.strokeStyle = '#7c7c74'; g.lineWidth = 1; g.stroke();
        var gx1 = axh(cx - slabW / 2 + 3), gx2 = axh(cx + slabW / 2 - 3);
        for (i = 1; i * 6 < slabH - 4; i++) {
          fill(gx1, axi(slabY + 3 + i * 6), gx2 - gx1, 2, (i % 2) ? '#242424' : '#080808');
        }
        if (isPitch) {
          fill(slabX, midY, 1.5, 32, '#8b8b84');                    // centre marks, slab edges
          fill(slabX + slabW - 1.5, midY, 1.5, 32, '#8b8b84');
        }
        var cy = cyMid + (1 - 2 * t) * travel;
        var capX = axh(cx - CAP_W / 2), capY = axh(cy - CAP_H / 2);
        roundRectPath(capX, capY, CAP_W, CAP_H, 3);
        g.fillStyle = activeWheel[isPitch ? 'pitch' : 'mod'] ? '#3d7ec4' : '#f7f7f3'; g.fill();
        g.strokeStyle = '#7c7c74'; g.stroke();
        for (i = -1; i <= 1; i++) {
          fill(cx + i * 4 - 0.5, axi(cy - CAP_H / 2) + 4, 1, CAP_H - 8,
               i === 0 && isPitch ? '#8b8b84' : '#080808');          // centre ridge lit on pitch
        }
      }
    }
    function paint() {                       // key-down state: lit white / lit black key
      if (!g || !dirty) return;
      dirty = false;
      if (g.clearRect) g.clearRect(0, 0, W, H);
      var i, n, x;
      fill(0, 0, W, H, '#101010');
      for (i = 0; i < k.whites.length; i++) {
        n = k.whites[i]; x = k.wheelWidth + i * k.whiteW;
        fill(x, 0, k.whiteW, H, lit[n] ? '#b9d7f2' : '#f7f7f3');
        g.strokeStyle = '#7c7c74'; g.lineWidth = 1;
        g.beginPath(); g.moveTo(x + 0.5, 0.5); g.lineTo(x + 0.5, H - 0.5); g.stroke();
        if (((n % 12) + 12) % 12 === 0) {
          g.fillStyle = '#8b8b84'; g.font = Math.min(11, Math.max(8, k.whiteW * 0.34)) + 'px system-ui, sans-serif';
          if (g.textAlign !== undefined) g.textAlign = 'center';
          g.fillText('C' + (Math.floor(n / 12) - 1), x + k.whiteW / 2, H - 6);
        }
      }
      for (i = 0; i < k.blacks.length; i++) {
        n = k.blacks[i]; x = k.wheelWidth + k.blackBoundary[n] * k.whiteW - k.blackW / 2;
        fill(x, 0, k.blackW, k.blackH, lit[n] ? '#3d7ec4' : '#151515');
      }
      paintWheels();
    }
    function invalidate() {
      lit = {};
      var a = k.activeNotes(), i;
      for (i = 0; i < a.length; i++) lit[a[i]] = true;
      dirty = true;
      if (typeof requestAnimationFrame === 'function') {
        if (raf === null) raf = requestAnimationFrame(function () { raf = null; paint(); });
      } else paint();
    }

    function pos(ev) { var r = cv.getBoundingClientRect(); return [ev.clientX - r.left, ev.clientY - r.top]; }
    function onDown(ev) {
      if (ev.pointerType === 'mouse' && ev.button !== undefined && ev.button !== 0) return;
      if (cv.setPointerCapture) { try { cv.setPointerCapture(ev.pointerId); } catch (e) {} }
      var p = pos(ev), w = k.wheelAt(p[0], p[1]);
      if (w) activeWheel[w] = true;
      k.press(ev.pointerId, p[0], p[1]);
      invalidate();
      if (ev.preventDefault) ev.preventDefault();
    }
    function onMove(ev) {
      var p = pos(ev);
      if (cv.style) cv.style.cursor = k.wheelAt(p[0], p[1]) ? 'ns-resize' : '';
      if (k.move(ev.pointerId, p[0], p[1]).length) invalidate();
    }
    function onUp(ev) {
      k.release(ev.pointerId);
      activeWheel = {};
      if (cv.releasePointerCapture) { try { cv.releasePointerCapture(ev.pointerId); } catch (e) {} }
      invalidate();
    }
    function onCancel(ev) { k.release(ev.pointerId); activeWheel = {}; invalidate(); }
    function onLeave(ev) {                   // touch that left without capture (pointerup outside)
      if (cv.hasPointerCapture && cv.hasPointerCapture(ev.pointerId)) return;
      k.release(ev.pointerId);
      activeWheel = {};
      invalidate();
    }
    /* mouse wheel over a wheel column nudges that wheel (up = value up); over the keys: no-op */
    function onWheel(ev) {
      var p = pos(ev), w = k.wheelAt(p[0], p[1]);
      if (!w) return;
      var d = ev.deltaY > 0 ? -1 : (ev.deltaY < 0 ? 1 : 0);
      if (!d) return;
      if (k.nudgeWheel(w, d).length) invalidate();
      if (ev.preventDefault) ev.preventDefault();
    }
    function onBlur() { if (k.allNotesOff().length) invalidate(); }
    function onVis() { if (doc.hidden && k.allNotesOff().length) invalidate(); }
    function onKeyDown(ev) {
      var oct = OCTAVE_KEYS.hasOwnProperty(ev.code), evs = k.keyDown(ev.code, ev.repeat);
      if (oct || evs.length) { if (ev.preventDefault) ev.preventDefault(); invalidate(); }
    }
    function onKeyUp(ev) { if (k.keyUp(ev.code).length) invalidate(); }

    if (cv.addEventListener) {
      cv.addEventListener('pointerdown', onDown);
      cv.addEventListener('pointermove', onMove);
      cv.addEventListener('pointerup', onUp);
      cv.addEventListener('pointercancel', onCancel);
      cv.addEventListener('pointerleave', onLeave);
      cv.addEventListener('lostpointercapture', onCancel);
      cv.addEventListener('wheel', onWheel);
      cv.addEventListener('contextmenu', function (ev) { if (ev.preventDefault) ev.preventDefault(); });
    }
    if (win && win.addEventListener) {
      win.addEventListener('blur', onBlur);
      win.addEventListener('keydown', onKeyDown);
      win.addEventListener('keyup', onKeyUp);
      win.addEventListener('resize', resize);
    }
    if (doc && doc.addEventListener) doc.addEventListener('visibilitychange', onVis);

    el.appendChild(cv);
    resize();
    k.onNoteOn = opts.onNoteOn ? function (note, vel) { invalidate(); opts.onNoteOn(note, vel); } : null;
    k.onNoteOff = opts.onNoteOff ? function (note) { invalidate(); opts.onNoteOff(note); } : null;
    k.onPitchBend = opts.onPitchBend ? function (v) { invalidate(); opts.onPitchBend(v); } : null;
    k.onModWheel = opts.onModWheel ? function (v) { invalidate(); opts.onModWheel(v); } : null;
    invalidate();

    return {
      canvas: cv, model: k,
      noteAt: function (x, y) { return k.noteAt(x, y); },
      activeNotes: function () { return k.activeNotes(); },
      allNotesOff: function () { var e = k.allNotesOff(); invalidate(); return e; },
      setOctave: function (o) { k.setOctave(o); invalidate(); },
      setPitchBend: function (v) { k.setPitchBend(v); invalidate(); return k.pitchBend; },
      setModWheel: function (v) { k.setModWheel(v); invalidate(); return k.modWheel; },
      setHeight: function (h) { k.height = h; resize(); },
      resize: resize,
      destroy: function () {
        if (cv.removeEventListener) {
          cv.removeEventListener('pointerdown', onDown); cv.removeEventListener('pointermove', onMove);
          cv.removeEventListener('pointerup', onUp); cv.removeEventListener('pointercancel', onCancel);
          cv.removeEventListener('pointerleave', onLeave); cv.removeEventListener('lostpointercapture', onCancel);
          cv.removeEventListener('wheel', onWheel);
        }
        if (win && win.removeEventListener) {
          win.removeEventListener('blur', onBlur); win.removeEventListener('keydown', onKeyDown);
          win.removeEventListener('keyup', onKeyUp); win.removeEventListener('resize', resize);
        }
        if (doc && doc.removeEventListener) doc.removeEventListener('visibilitychange', onVis);
        k.allNotesOff();
        if (el.removeChild) el.removeChild(cv);
      }
    };
  }

  var api = { create: create, mount: mount, isBlackNote: isBlackNote, KEY_MAP: KEY_MAP, OCTAVE_KEYS: OCTAVE_KEYS };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Synth1Keyboard = api;
})(typeof self !== 'undefined' ? self : this);
