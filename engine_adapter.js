// engine_adapter.js — page-side adapter for the m4 panel.
//   Synth1Engine.create(audioContext) ->
//     {setParam(id,v), noteOn(n,v), noteOff(n), loadSy1(text), setVolume(v),
//      cc(cc,v), pitchBend(v), aftertouch(v)}   // perf: tasks/m4-wasm-perf exports
// Loads synth1_worklet.js (ES module) next to this script and connects a
// stereo AudioWorkletNode to audioContext.destination.
(function () {
  function scriptBase() {
    if (typeof document !== 'undefined' && document.currentScript &&
        document.currentScript.src) {
      return document.currentScript.src.replace(/[^/]*$/, '');
    }
    return '';
  }

  const BASE = scriptBase() || './';   // document.currentScript is only set while loading

  async function createMainThread(audioContext, base) {
    const mod = await (await import(base + 'synth1_engine.mjs?v=5')).default();
    const h = mod._s1_create(audioContext.sampleRate);
    // main-thread fallback (plain http: no AudioWorklet): smallest buffer that stays glitch-free;
    // 256 frames = 5.8 ms @ 44.1 kHz (was 1024 = 23 ms); ?buf=512|1024 in the page URL overrides
    const qb = +(new URLSearchParams((typeof location !== 'undefined' && location.search) || '').get('buf') || 0);
    const N = [256, 512, 1024, 2048].includes(qb) ? qb : 256, pL = mod._malloc(N * 4), pR = mod._malloc(N * 4);
    const node = audioContext.createScriptProcessor(N, 0, 2);
    node.onaudioprocess = (e) => {
      const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1), n = L.length;
      mod._s1_render(h, pL, pR, n);
      const heap = mod.HEAPF32;
      L.set(heap.subarray(pL >> 2, (pL >> 2) + n)); R.set(heap.subarray(pR >> 2, (pR >> 2) + n));
    };
    node.connect(audioContext.destination);
    return {
      node: node, mainThread: true,
      setParam: (id, v) => mod._s1_set_param(h, id | 0, v | 0),
      noteOn: (n, v) => mod._s1_note_on(h, n | 0, v | 0),
      noteOff: (n) => mod._s1_note_off(h, n | 0),
      loadSy1: (text) => {
        const bytes = new TextEncoder().encode(String(text)), ptr = mod._malloc(bytes.length + 1);
        mod.HEAPU8.set(bytes, ptr); mod.HEAPU8[ptr + bytes.length] = 0;
        mod._s1_load_sy1(h, ptr); mod._free(ptr);
      },
      setVolume: (v) => mod._s1_set_volume(h, v | 0),
      cc: (c, v) => mod._s1_cc(h, c | 0, v | 0),
      pitchBend: (v) => mod._s1_pitch_bend(h, v | 0),
      aftertouch: (v) => mod._s1_aftertouch(h, v | 0),
      // Master Tuning -100..+100 cents (m4-mastertune-land; process-global, next note-on)
      setMasterTune: (c) => mod._s1_set_mastertune(h, c | 0)
    };
  }

  globalThis.Synth1Engine = {
    create: async function (audioContext, opts) {
      const base = (opts && opts.base) || BASE;
      // AudioWorklet exists only in secure contexts (https / localhost). Elsewhere (e.g. plain http
      // on a LAN address) run the same wasm engine on the main thread in a ScriptProcessorNode.
      if (!audioContext.audioWorklet) return createMainThread(audioContext, base);
      await audioContext.audioWorklet.addModule(
        base + 'synth1_worklet.js?v=5', { type: 'module' });
      const node = new AudioWorkletNode(audioContext, 'synth1',
        { outputChannelCount: [2] });
      node.connect(audioContext.destination);
      const port = node.port;
      port.onmessage = (e) => { if (e.data && e.data.type === 'status') console.log('synth1 worklet', JSON.stringify(e.data)); };
      node.onprocessorerror = (e) => console.log('synth1 worklet processorerror', e && e.message);
      return {
        node: node,
        setParam: function (id, v) { port.postMessage({ type: 'param', id: id | 0, value: v | 0 }); },
        noteOn: function (n, v) { port.postMessage({ type: 'note', note: n | 0, vel: v | 0 }); },
        noteOff: function (n) { port.postMessage({ type: 'note', note: n | 0, vel: 0 }); },
        loadSy1: function (text) { port.postMessage({ type: 'sy1', text: String(text) }); },
        setVolume: function (v) { port.postMessage({ type: 'volume', value: v | 0 }); },
        cc: function (c, v) { port.postMessage({ type: 'cc', cc: c | 0, value: v | 0 }); },
        pitchBend: function (v) { port.postMessage({ type: 'bend', value: v | 0 }); },
        aftertouch: function (v) { port.postMessage({ type: 'at', value: v | 0 }); },
        setMasterTune: function (c) { port.postMessage({ type: 'mastertune', cents: c | 0 }); }
      };
    }
  };
})();
