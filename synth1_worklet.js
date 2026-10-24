// synth1_worklet.js — AudioWorkletProcessor hosting the Synth1 wasm engine.
// Register with: audioContext.audioWorklet.addModule(url, {type: 'module'}).
// Port messages: {type:'param',id,value} {type:'note',note,vel}
// {type:'sy1',text} {type:'volume',value} {type:'cc',cc,value}
// {type:'bend',value} {type:'at',value}. Stereo output.
import './worklet_polyfills.js';
import createSynth1Module from './synth1_engine.mjs?v=5';

const MAX_QUANTUM = 4096;

class Synth1Processor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.mod = null;
    this.h = 0;
    this.pL = 0;
    this.pR = 0;
    this.queue = [];
    this.port.onmessage = (e) => this.handle(e.data);
    createSynth1Module().catch((e) => { this.port.postMessage({ type: 'status', error: String(e && e.stack || e) }); }).then((mod) => {
      if (!mod) return;
      this.mod = mod;
      this.h = mod._s1_create(sampleRate);
      this.pL = mod._malloc(MAX_QUANTUM * 4);
      this.pR = mod._malloc(MAX_QUANTUM * 4);
      const q = this.queue;
      this.queue = [];
      try { for (const m of q) this.handle(m); this.port.postMessage({ type: 'status', ready: true }); }
      catch (e) { this.port.postMessage({ type: 'status', error: String(e && e.stack || e) }); }
    });
  }

  handle(m) {
    if (!m || typeof m !== 'object') return;
    if (!this.h) {
      this.queue.push(m);
      return;
    }
    const mod = this.mod;
    switch (m.type) {
      case 'param':
        mod._s1_set_param(this.h, m.id | 0, m.value | 0);
        break;
      case 'note':
        if ((m.vel | 0) > 0) mod._s1_note_on(this.h, m.note | 0, m.vel | 0);
        else mod._s1_note_off(this.h, m.note | 0);
        break;
      case 'volume':
        mod._s1_set_volume(this.h, m.value | 0);
        break;
      case 'cc':
        mod._s1_cc(this.h, m.cc | 0, m.value | 0);
        break;
      case 'bend':
        mod._s1_pitch_bend(this.h, m.value | 0);
        break;
      case 'at':
        mod._s1_aftertouch(this.h, m.value | 0);
        break;
      case 'mastertune':   // Options > Master Tuning (m4-mastertune-land)
        mod._s1_set_mastertune(this.h, m.cents | 0);
        break;
      case 'sy1': {
        const bytes = new TextEncoder().encode(String(m.text));
        const ptr = mod._malloc(bytes.length + 1);
        mod.HEAPU8.set(bytes, ptr);
        mod.HEAPU8[ptr + bytes.length] = 0;
        mod._s1_load_sy1(this.h, ptr);
        mod._free(ptr);
        break;
      }
      default:
        break;
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const L = out[0];
    const R = out.length > 1 ? out[1] : null;
    if (!this.h) {
      L.fill(0);
      if (R) R.fill(0);
      return true;
    }
    const n = L.length;
    if (n > MAX_QUANTUM) return true;
    this.mod._s1_render(this.h, this.pL, this.pR, n);
    const heap = this.mod.HEAPF32;  // re-read: memory may have grown
    const oL = this.pL >> 2, oR = this.pR >> 2;
    L.set(heap.subarray(oL, oL + n));
    if (R) R.set(heap.subarray(oR, oR + n));
    return true;
  }
}

registerProcessor('synth1', Synth1Processor);
