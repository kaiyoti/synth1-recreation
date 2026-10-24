// AudioWorkletGlobalScope lacks atob (the SINGLE_FILE wasm loader decodes base64 with it) and
// TextEncoder (used for .sy1 text). Imported by synth1_worklet.js before the engine module.
if (typeof globalThis.atob !== 'function') {
  const T = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/', R = {};
  for (let i = 0; i < 64; i++) R[T[i]] = i;
  globalThis.atob = function (s) {
    s = String(s).replace(/[^A-Za-z0-9+/]/g, '');
    let out = '', buf = 0, bits = 0;
    for (let i = 0; i < s.length; i++) {
      buf = (buf << 6) | R[s[i]]; bits += 6;
      if (bits >= 8) { bits -= 8; out += String.fromCharCode((buf >> bits) & 255); }
    }
    return out;
  };
}
if (typeof globalThis.TextEncoder !== 'function') {
  globalThis.TextEncoder = class { encode(s) {
    const u = unescape(encodeURIComponent(String(s))), b = new Uint8Array(u.length);
    for (let i = 0; i < u.length; i++) b[i] = u.charCodeAt(i);
    return b;
  } };
}
