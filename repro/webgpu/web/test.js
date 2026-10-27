const q = new URLSearchParams(location.search);
const build = q.get('build');
const test = q.get('test');
const R = window.__r = { build, test, done: false };
const log = (o) => console.log('[t] ' + (typeof o === 'string' ? o : JSON.stringify(o)));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const hex = (v) => '0x' + (v >>> 0).toString(16).padStart(8, '0');
const lostAtFrame = {};
const raf = { scheduled: 0, fired: 0 };
{
  const original = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = (cb) => { ++raf.scheduled; return original((t) => { ++raf.fired; cb(t); }); };
}

async function makeDevice(label, features) {
  let adapter = null;
  for (let i = 0; i < 40 && !adapter; ++i) {
    adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) { log(`requestAdapter returned null (attempt ${i + 1}), retrying`); await sleep(250); }
  }
  const requiredFeatures = features.filter((f) => adapter.features.has(f));
  const device = await adapter.requestDevice({ label, requiredFeatures });
  device.onuncapturederror = (e) => console.error(`[uncaptured ${label}] ${e.error.constructor.name}: ${e.error.message}`);
  device.lost.then((info) => {
    lostAtFrame[label] = window.Module?._h_frame?.();
    log({ lost: label, reason: info.reason, message: info.message });
  });
  log({ device: label, adapter: `${adapter.info?.vendor}/${adapter.info?.architecture}`, deviceAdapterInfo: 'adapterInfo' in device, features: [...device.features].sort() });
  return device;
}

function loadModule(variant, device) {
  return new Promise((resolve, reject) => {
    window.Module = {
      preinitializedWebGPUDevice: device,
      canvas: document.getElementById('canvas'),
      print: (s) => console.log('[stdout] ' + s),
      printErr: (s) => console.warn('[stderr] ' + s),
      onRuntimeInitialized: () => resolve(window.Module),
      onAbort: (what) => console.error('[abort] ' + what),
    };
    const s = document.createElement('script');
    s.src = `out/${build}/${variant}.js`;
    s.onerror = reject;
    document.body.appendChild(s);
  });
}

function state(M) {
  return {
    frame: M._h_frame(),
    renderer: M.UTF8ToString(M._h_renderer_name()),
    fatalCount: M._h_fatal_count(),
    fatalCode: M._h_fatal_code(),
    fatalFrame: M._h_fatal_frame(),
    fatalMsg: M.UTF8ToString(M._h_fatal_msg()),
  };
}

async function frames(M, n, budgetMs = 5000) {
  const start = M._h_frame();
  const t0 = performance.now();
  while (M._h_frame() < start + n) {
    if (performance.now() - t0 > budgetMs) return { stalled: true, from: start, at: M._h_frame() };
    await sleep(10);
  }
  return { stalled: false, from: start, at: M._h_frame() };
}

// Headless Chrome reads a WebGPU canvas back as zeros via drawImage, so pixels are copied on the GPU.
let canvasDevice = null, canvasFormat = null, canvasTexture = null;
{
  const cfg = GPUCanvasContext.prototype.configure;
  GPUCanvasContext.prototype.configure = function (d) {
    canvasDevice = d.device;
    canvasFormat = d.format;
    return cfg.call(this, Object.assign({}, d, { usage: (d.usage ?? GPUTextureUsage.RENDER_ATTACHMENT) | GPUTextureUsage.COPY_SRC }));
  };
  const gct = GPUCanvasContext.prototype.getCurrentTexture;
  GPUCanvasContext.prototype.getCurrentTexture = function () { canvasTexture = gct.call(this); return canvasTexture; };
}

if (test === 'W6b') {
  const rd = GPUAdapter.prototype.requestDevice;
  GPUAdapter.prototype.requestDevice = async function (d) {
    if (d && d.requiredFeatures) {
      d = Object.assign({}, d, { requiredFeatures: [...d.requiredFeatures].filter((f) => f !== 'timestamp-query') });
    }
    const dev = await rd.call(this, d);
    log({ hook: 'requestDevice', requested: d && d.requiredFeatures && [...d.requiredFeatures].sort(), granted: [...dev.features].sort() });
    return dev;
  };
}

let pendingSample = null;
globalThis.__afterFrame = () => samplePending();
function samplePending() {
  if (!pendingSample) return;
  const { x, y, resolve } = pendingSample;
  pendingSample = null;
  try {
    const buf = canvasDevice.createBuffer({ size: 256, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = canvasDevice.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: canvasTexture, origin: [x, y] }, { buffer: buf, bytesPerRow: 256 }, [1, 1]);
    canvasDevice.queue.submit([enc.finish()]);
    buf.mapAsync(GPUMapMode.READ).then(() => {
      const p = [...new Uint8Array(buf.getMappedRange(), 0, 4)];
      resolve(canvasFormat === 'bgra8unorm' ? [p[2], p[1], p[0], p[3]] : p);
    }, (e) => resolve('mapAsync failed: ' + e));
  } catch (e) {
    resolve('sample failed: ' + e);
  }
}
function canvasPixel(x = 128, y = 128) {
  return new Promise((resolve) => { pendingSample = { x, y, resolve }; });
}

async function runAsync() {
  // Fresh headless Chrome can return null for the first requestAdapter; absorb it before bgfx asks.
  for (let i = 0; i < 40 && !(await navigator.gpu.requestAdapter()); ++i) { log(`warm-up requestAdapter null (attempt ${i + 1})`); await sleep(250); }
  await loadModule('harness_async', undefined);
  const t0 = performance.now();
  while (!(globalThis.__async && globalThis.__async.done) && performance.now() - t0 < 15000) { await sleep(50); }
  const a = Object.assign({}, globalThis.__async);
  if (a.word0 !== undefined) { a.word0 = hex(a.word0); a.wordLast = hex(a.wordLast); }
  return { async: a, timedOutInJs: !a.done };
}

const BLUE = [0x33, 0x66, 0xcc, 255];

const tests = {
  async W1() {
    const dev = await makeDevice('A', []);
    const M = await loadModule('harness', dev);
    const ok = M._h_init(0);
    const f = await frames(M, 10);
    return { init: ok, frames: f, state: state(M), pixel: await canvasPixel(), expected: BLUE };
  },

  async W1n() {
    Object.defineProperty(GPUDevice.prototype, 'adapterInfo', { get: () => undefined, configurable: true });
    return tests.W1();
  },

  async W1t() {
    const dev = await makeDevice('A', ['timestamp-query']);
    const M = await loadModule('harness', dev);
    const ok = M._h_init(0);
    const f = await frames(M, 10);
    return { init: ok, timestampQuery: dev.features.has('timestamp-query'), frames: f, state: state(M), pixel: await canvasPixel(), expected: BLUE };
  },

  async 'W2-'() {
    const dev = await makeDevice('A', []);
    const M = await loadModule('harness', dev);
    const ok = M._h_init(0);
    return { init: ok, deviceIndirectFirstInstance: dev.features.has('indirect-first-instance'), capsDrawIndirect: M._h_draw_indirect(), caps: hex(M._h_caps_hi()) + hex(M._h_caps_lo()).slice(2) };
  },

  async 'W2+'() {
    const dev = await makeDevice('A', ['indirect-first-instance']);
    const M = await loadModule('harness', dev);
    const ok = M._h_init(0);
    return { init: ok, deviceIndirectFirstInstance: dev.features.has('indirect-first-instance'), capsDrawIndirect: M._h_draw_indirect(), caps: hex(M._h_caps_hi()) + hex(M._h_caps_lo()).slice(2) };
  },

  async W3() {
    const dev = await makeDevice('A', []);
    const M = await loadModule('harness', dev);
    const ok = M._h_init(0);
    await frames(M, 5);
    const before = state(M);
    M._h_readback_request();
    await frames(M, 1);
    const ready = M._h_readback_ready();
    while (M._h_frame() < ready) { await sleep(10); }
    const after = await frames(M, 30);
    return {
      init: ok, before, readyFrame: ready, afterReady: after,
      word0: hex(M._h_readback_word(0)), sentinel: '0xdeadbeef',
      state: state(M), pixel: await canvasPixel(),
    };
  },

  async W4() {
    const dev = await makeDevice('A', []);
    const M = await loadModule('harness', dev);
    const ok = M._h_init(0);
    await frames(M, 10);
    const before = state(M);
    log({ step: 'device.destroy()', frame: before.frame });
    dev.destroy();
    await sleep(1000);
    const f = await frames(M, 20);
    return { init: ok, before, framesAfterDestroy: f, after: state(M), lostAtFrame: lostAtFrame.A, rafPending: raf.scheduled - raf.fired };
  },

  async W5() {
    const devA = await makeDevice('A', []);
    const M = await loadModule('harness', devA);
    const okA = M._h_init(0);
    await frames(M, 10);
    const pixelA = await canvasPixel();
    M._h_shutdown();
    await sleep(100);

    const devB = await makeDevice('B', []);
    M.preinitializedWebGPUDevice = devB;
    const okB = M._h_init(0);
    await frames(M, 10);
    const pixelB = await canvasPixel();

    log({ step: 'destroy A' });
    devA.destroy();
    await sleep(500);
    await frames(M, 20);
    const afterDestroyA = state(M);

    log({ step: 'destroy B' });
    devB.destroy();
    await sleep(500);
    await frames(M, 20);
    const afterDestroyB = state(M);

    return { okA, pixelA, okB, pixelB, afterDestroyA, afterDestroyB, lostAtFrameB: lostAtFrame.B, rafPending: raf.scheduled - raf.fired };
  },

  async W6() { return Object.assign(await runAsync(), { expected: '0x44332211' }); },

  async W6b() { return Object.assign(await runAsync(), { expected: '0x44332211' }); },

  async W7() {
    const dev = await makeDevice('A', []);
    const M = await loadModule('harness', dev);
    const ok = M._h_init(0);
    M._h_occlusion(1);
    const f = await frames(M, 60);
    M._h_occlusion(0);
    await frames(M, 5);
    return { init: ok, frames: f, occResult: M._h_occ_result(), occCount: M._h_occ_count(), occVisibleFrames: M._h_occ_visible_frames(), state: state(M) };
  },
};

(async () => {
  try {
    R.result = await tests[test]();
  } catch (e) {
    R.error = String(e && e.stack || e);
    console.error('[t] test threw: ' + R.error);
  }
  R.done = true;
})();
