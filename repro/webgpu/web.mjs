import http from "node:http";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn, execFileSync } from "node:child_process";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const OUT = process.env.OUT ?? path.resolve(HERE, "../../.build/repro-webgpu");
const rendered = (r) => Array.isArray(r.result?.pixel) && JSON.stringify(r.result.pixel) === JSON.stringify(r.result.expected);
const uncaptured = (r) => r.console.some((l) => /\[uncaptured /.test(l));
const browserStalled = (after, lostAt, r) => after?.frame <= lostAt && r.result.rafPending > 0;
const reportsLoss = (s) => s?.renderer === "Noop" && s.fatalCode === 4 && s.fatalCount === 1;
const TESTS = {
  W1: { expect: { control: "ok", pr: "ok" }, check: rendered },
  W1n: { expect: { control: "bug", pr: "ok" }, check: rendered },
  "W2-": { expect: { control: "bug", pr: "ok" }, check: (r) => r.result?.capsDrawIndirect === 0 },
  "W2+": { expect: { control: "ok", pr: "ok" }, check: (r) => (r.result?.deviceIndirectFirstInstance === false ? null : r.result?.capsDrawIndirect === 1) },
  W3: { expect: { control: "bug", pr: "ok" }, check: (r) => r.verdict === "completed" && r.result?.afterReady?.stalled === false },
  W4: { expect: { control: "bug", pr: "ok" }, check: (r) => (browserStalled(r.result?.after, r.result?.lostAtFrame, r) ? null : reportsLoss(r.result?.after)) },
  W5: { expect: { control: "bug", pr: "ok" }, check: (r) => r.result?.afterDestroyA?.fatalCount === 0 && r.result.afterDestroyA.renderer === "WebGPU"
    && (browserStalled(r.result.afterDestroyB, r.result.lostAtFrameB, r) ? null : reportsLoss(r.result.afterDestroyB)) },
  W6: { expect: { base: "bug", control: "bug", pr: "ok" }, check: (r) => r.verdict === "completed" && r.result?.async?.done === true },
  W6b: { expect: { base: "bug", control: "bug", pr: "ok" }, check: (r) => r.result?.async?.word0 === "0x44332211" },
  W7: { expect: { control: "bug", pr: "ok" }, check: (r) => r.result?.occResult === 1 && !uncaptured(r) },
  W1t: { expect: { pr: "info" } },
};
const TEST_BUDGET_MS = 30000;
const EVAL_TIMEOUT_MS = 4000;
const NOISE = [/requestAdapter returned null/, /warm-up requestAdapter/, /A valid external Instance reference no longer exists/];
const DONE_EXPR = "window.__r && window.__r.done ? JSON.stringify(window.__r) : null";

const args = process.argv.slice(2);
const browserName = (args.find((a) => a.startsWith("--browser=")) ?? "--browser=chrome").slice(10);
const requested = args.filter((a) => !a.startsWith("--"));
for (const t of requested) if (!TESTS[t]) throw new Error(`unknown test ${t}; known: ${Object.keys(TESTS).join(" ")}`);
const tests = requested.length ? requested : Object.keys(TESTS);

function findBinary(envName, candidates) {
  if (process.env[envName]) return process.env[envName];
  for (const c of candidates) {
    try { return execFileSync("which", [c], { stdio: ["ignore", "pipe", "ignore"] }).toString().trim(); } catch {}
  }
  throw new Error(`${candidates[0]} not found; set ${envName}=/path/to/binary`);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const withTimeout = (p, ms) => Promise.race([p.then((v) => ({ ok: true, v }), (e) => ({ ok: false, err: String(e) })), sleep(ms).then(() => ({ ok: false, timeout: true }))]);

class Rpc {
  constructor(ws) {
    this.ws = ws;
    this.id = 0;
    this.pending = new Map();
    this.handlers = new Map();
    ws.addEventListener("message", (e) => {
      const m = JSON.parse(e.data);
      if (m.id !== undefined && this.pending.has(m.id)) {
        const p = this.pending.get(m.id);
        this.pending.delete(m.id);
        if (m.error) p.reject(new Error(typeof m.error === "string" ? `${m.error}: ${m.message}` : m.error.message)); else p.resolve(m.result);
      } else if (m.method) {
        for (const h of this.handlers.get(m.method) ?? []) h(m.params);
      }
    });
  }
  static async connect(url) {
    const ws = new WebSocket(url);
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    return new Rpc(ws);
  }
  send(method, params = {}) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }
  on(method, h) { this.handlers.set(method, [...(this.handlers.get(method) ?? []), h]); }
  once(method) {
    return new Promise((resolve) => {
      const h = (p) => { this.handlers.set(method, this.handlers.get(method).filter((x) => x !== h)); resolve(p); };
      this.on(method, h);
    });
  }
}

const shortName = (n) => { let prev; do { prev = n; n = n.replace(/<[^<>]*>/g, "").replace(/\([^()]*\)/g, ""); } while (n !== prev); return n.replace(/^\$/, "").replace(/\[abi:[^\]]*\]/g, ""); };

const chrome = {
  bin: () => findBinary("CHROME", ["google-chrome-stable", "google-chrome", "chromium", "chromium-browser"]),
  spawn: (bin, profile) => spawn(bin, ["--headless=new", "--enable-unsafe-webgpu", "--enable-features=Vulkan", "--use-angle=vulkan", "--ignore-gpu-blocklist",
    "--no-first-run", "--no-default-browser-check", ...(process.env.CHROME_FLAGS ?? "").split(" ").filter(Boolean),
    "--remote-debugging-port=0", `--user-data-dir=${profile}`, "about:blank"], { stdio: "ignore", detached: true }),
  async connect(proc, profile, log) {
    const activePort = path.join(profile, "DevToolsActivePort");
    for (let i = 0; i < 80 && !fs.existsSync(activePort); ++i) await sleep(250);
    const dp = fs.readFileSync(activePort, "utf8").split("\n")[0];
    let cdp;
    for (let i = 0; i < 40 && !cdp; ++i) {
      const targets = await fetch(`http://127.0.0.1:${dp}/json/list`).then((r) => r.json()).catch(() => []);
      const page = targets.find((t) => t.type === "page");
      if (page) cdp = await Rpc.connect(page.webSocketDebuggerUrl); else await sleep(250);
    }
    if (!cdp) throw new Error("no page target");
    cdp.on("Runtime.consoleAPICalled", (p) => log(`${p.type}: ${p.args.map((a) => a.value ?? a.description ?? a.type).join(" ")}`));
    cdp.on("Log.entryAdded", (p) => log(`${p.entry.level}: ${p.entry.text}`));
    cdp.on("Runtime.exceptionThrown", (p) => log(`pageerror: ${p.exceptionDetails.exception?.description ?? p.exceptionDetails.text}`));
    cdp.on("Inspector.targetCrashed", () => log("page crashed"));
    await Promise.all(["Runtime.enable", "Log.enable", "Debugger.enable", "Inspector.enable"].map((m) => cdp.send(m)));
    return {
      navigate: (url) => cdp.send("Page.navigate", { url }),
      evaluate: async (expression) => (await cdp.send("Runtime.evaluate", { expression, returnByValue: true })).result.value ?? null,
      async hangStack() {
        const paused = cdp.once("Debugger.paused");
        cdp.send("Debugger.pause").catch(() => {});
        const p = await withTimeout(paused, 5000);
        return p.ok ? p.v.callFrames.slice(0, 16).map((f) => shortName(f.functionName || "(anonymous)")) : ["Debugger.pause got no response"];
      },
    };
  },
};

const firefox = {
  bin: () => findBinary("FIREFOX", ["firefox", "firefox-nightly"]),
  spawn(bin, profile) {
    fs.writeFileSync(path.join(profile, "user.js"), 'user_pref("dom.webgpu.enabled", true);\nuser_pref("gfx.webgpu.ignore-blocklist", true);\n');
    return spawn(bin, ["--headless", "--no-remote", "--profile", profile, "--remote-debugging-port=0", "about:blank"], { stdio: ["ignore", "ignore", "pipe"], detached: true });
  },
  async connect(proc, profile, log) {
    const url = await new Promise((resolve, reject) => {
      let buf = "";
      const timer = setTimeout(() => reject(new Error("Firefox did not start WebDriver BiDi")), 30000);
      proc.stderr.on("data", (d) => {
        buf += d;
        const m = buf.match(/WebDriver BiDi listening on (ws:\/\/\S+)/);
        if (m) { clearTimeout(timer); resolve(m[1]); }
      });
    });
    const bidi = await Rpc.connect(`${url}/session`);
    await bidi.send("session.new", { capabilities: {} });
    const { contexts } = await bidi.send("browsingContext.getTree", {});
    const context = contexts[0].context;
    bidi.on("log.entryAdded", (p) => log(p.type === "javascript" ? `pageerror: ${p.text}` : `${p.method ?? p.level}: ${p.text}`));
    await bidi.send("session.subscribe", { events: ["log.entryAdded"] });
    return {
      navigate: (u) => bidi.send("browsingContext.navigate", { context, url: u, wait: "none" }),
      async evaluate(expression) {
        const r = await bidi.send("script.evaluate", { expression, target: { context }, awaitPromise: false });
        return r.type === "success" && r.result.type === "string" ? r.result.value : null;
      },
      hangStack: async () => null,
    };
  },
};

const browser = { chrome, firefox }[browserName];
if (!browser) throw new Error(`unknown browser ${browserName}; use --browser=chrome or --browser=firefox`);
const BIN = browser.bin();

const mime = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm" };
const server = http.createServer((req, res) => {
  const url = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (url === "/favicon.ico") { res.statusCode = 204; return res.end(); }
  const [root, rel] = url.startsWith("/out/") ? [path.join(OUT, "web"), url.slice(5)] : [path.join(HERE, "web"), url];
  const p = path.join(root, rel);
  if (!p.startsWith(root) || !fs.existsSync(p) || fs.statSync(p).isDirectory()) { res.statusCode = 404; return res.end(); }
  res.setHeader("content-type", mime[path.extname(p)] ?? "application/octet-stream");
  res.setHeader("cache-control", "no-store");
  fs.createReadStream(p).pipe(res);
}).listen(0, "127.0.0.1");
await new Promise((r) => server.once("listening", r));
const port = server.address().port;

async function runOne(build, test) {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "bgfx-webgpu-repro-"));
  const proc = browser.spawn(BIN, profile);
  const out = { browser: browserName, build, test, verdict: null, result: null, console: [] };
  const t0 = Date.now();
  const stamp = () => ((Date.now() - t0) / 1000).toFixed(2);
  try {
    const page = await browser.connect(proc, profile, (line) => out.console.push(`${stamp()} ${line}`));
    await page.navigate(`http://127.0.0.1:${port}/index.html?build=${build}&test=${encodeURIComponent(test)}`);
    for (;;) {
      await sleep(200);
      const r = await withTimeout(page.evaluate(DONE_EXPR), EVAL_TIMEOUT_MS);
      if (r.ok && r.v) {
        const v = JSON.parse(r.v);
        out.result = v.result ?? null;
        out.error = v.error;
        out.verdict = v.error ? "threw" : "completed";
        break;
      }
      if (!r.ok && r.timeout) {
        out.verdict = `HANG: main thread did not answer within ${EVAL_TIMEOUT_MS / 1000}s (at t=${stamp()}s)`;
        out.hangStack = await page.hangStack();
        break;
      }
      if (!r.ok) { out.verdict = `evaluate failed: ${r.err}`; break; }
      if (Date.now() - t0 > TEST_BUDGET_MS) { out.verdict = `TIMEOUT: not done after ${TEST_BUDGET_MS / 1000}s`; break; }
    }
  } catch (e) {
    out.verdict = `driver error: ${e}`;
  } finally {
    try { process.kill(-proc.pid, "SIGKILL"); } catch {}
    await sleep(300);
    fs.rmSync(profile, { recursive: true, force: true });
  }
  return out;
}

function report(r) {
  console.log(`\n=== ${r.test} ${r.build.toUpperCase()}: ${r.outcome} (${r.mark}) — ${r.verdict}`);
  if (r.result) console.log("result: " + JSON.stringify(r.result));
  if (r.hangStack) console.log("hang stack: " + r.hangStack.join(" <- "));
  if (r.error) console.log("error: " + r.error.split("\n")[0]);
  const counts = new Map();
  for (const l of r.console) {
    const [, t, text] = l.match(/^([0-9.]+) (.*)$/s);
    const body = text.split("\n")[0].replace(/0x[0-9a-f]{5,}/g, "0x…").slice(0, 240);
    if (NOISE.some((n) => n.test(body))) continue;
    const c = counts.get(body) ?? { t, n: 0 };
    ++c.n;
    counts.set(body, c);
  }
  for (const [body, c] of counts) console.log(`  t=${c.t}s ${c.n > 1 ? `[x${c.n}] ` : ""}${body}`);
}

console.log(`${browserName}: ${execFileSync(BIN, ["--version"]).toString().trim()}`);
const results = [];
for (const test of tests) {
  const { expect, check } = TESTS[test];
  for (const [build, want] of Object.entries(expect)) {
    if (!fs.existsSync(path.join(OUT, "web", build, "harness.js"))) continue;
    const r = await runOne(build, test);
    const ok = check ? check(r) : null;
    r.outcome = !check ? "INFO" : ok === null ? "SKIP" : ok ? "OK" : "BUG";
    r.mark = r.outcome === "INFO" ? "info" : r.outcome === "SKIP" ? "skipped" : r.outcome.toLowerCase() === want ? "as expected" : "UNEXPECTED";
    results.push(r);
    report(r);
  }
}
const file = path.join(OUT, "web", `results-${browserName}-${new Date().toISOString().replace(/[:.]/g, "-")}.json`);
fs.writeFileSync(file, JSON.stringify(results, null, 1));
server.close();

console.log("\n-- summary");
for (const r of results) console.log(`${r.build.padEnd(7)} ${r.test.padEnd(4)} ${r.outcome.padEnd(4)} ${r.mark}`);
console.log(`log: ${file}`);
const unexpected = results.filter((r) => r.mark === "UNEXPECTED").length;
console.log(unexpected ? `${unexpected} UNEXPECTED result(s)` : "all results as expected");
process.exitCode = unexpected ? 1 : 0;
