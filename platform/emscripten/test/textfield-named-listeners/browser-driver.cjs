#!/usr/bin/env node
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
// Resolve an existing installation; this driver never downloads a browser.
const { chromium } = require('playwright-core');
assert.equal(require('playwright-core/package.json').version, '1.63.0');
const root = path.resolve(process.argv[2]);
const inputs = JSON.parse(fs.readFileSync(path.join(root, 'inputs.json')));
const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
assert.match(process.env.CHROMIUM_SHA256 || '', /^[0-9a-f]{64}$/);
assert.equal(sha(fs.readFileSync(process.env.CHROMIUM_PATH)), process.env.CHROMIUM_SHA256);
const flags = ['--no-sandbox', '--enable-webgl', '--use-gl=swiftshader',
  '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'];
const mime = { '.html': 'text/html', '.js': 'application/javascript',
  '.wasm': 'application/wasm', '.bin': 'application/octet-stream' };
const hook = [
  "Module.print = function() { console.log(Array.from(arguments).join(' ')); };",
  'const originalPrintErr = Module.printErr;',
  'Module.printErr = function() {',
  "  console.error('[MODULE-ERROR] ' + Array.from(arguments).join(' '));",
  '  originalPrintErr.apply(Module, arguments);',
  '};',
].join('\n');
const server = http.createServer((request, response) => {
  const url = new URL(request.url, 'http://localhost');
  if (url.pathname === '/favicon.ico') { response.writeHead(204).end(); return; }
  const file = path.resolve(root, '.' + decodeURIComponent(url.pathname));
  if (!file.startsWith(root + path.sep)) { response.writeHead(403).end(); return; }
  fs.readFile(file, (error, bytes) => {
    if (error) { response.writeHead(404).end(); return; }
    if (file.endsWith('.html')) {
      const source = bytes.toString();
      const marker = /Module\.setStatus\('Downloading\.\.\.',\s*0,\s*0\);/;
      if (!marker.test(source)) { response.writeHead(500).end('Missing runtime hook'); return; }
      bytes = Buffer.from(source.replace(marker, match => match + hook));
    }
    response.writeHead(200, { 'Content-Type': mime[path.extname(file)] || 'application/octet-stream',
      'Cache-Control': 'no-store' });
    response.end(bytes);
  });
});

async function runArm(browser, address, arm) {
  const context = await browser.newContext({ viewport: { width: 640, height: 960 },
    deviceScaleFactor: 1, serviceWorkers: 'block' });
  const page = await context.newPage();
  const result = { arm, records: [], console: [], errors: [], wasm: [], network: [] };
  const logPath = path.join(root, arm, 'browser.json');
  // Hash the bytes passed into the actual WebAssembly API, including blob URLs
  // unpacked from coronaHtml5App.bin. Never infer execution from a version string.
  await page.addInitScript(() => {
    window.__engineWasm = [];
    const hash = async buffer => Array.from(new Uint8Array(
      await crypto.subtle.digest('SHA-256', buffer))).map(b => b.toString(16).padStart(2, '0')).join('');
    const instantiate = WebAssembly.instantiate;
    WebAssembly.instantiate = async function(bytes, imports) {
      if (!(bytes instanceof WebAssembly.Module)) {
        window.__engineWasm.push({ sha256: await hash(bytes), bytes: bytes.byteLength });
      }
      return instantiate.call(WebAssembly, bytes, imports);
    };
    const streaming = WebAssembly.instantiateStreaming;
    if (streaming) WebAssembly.instantiateStreaming = async function(source, imports) {
      const response = await source;
      const bytes = await response.clone().arrayBuffer();
      window.__engineWasm.push({ sha256: await hash(bytes), bytes: bytes.byteLength, url: response.url });
      return streaming.call(WebAssembly, response, imports);
    };
  });
  page.on('console', message => {
    const text = message.text();
    result.console.push({ type: message.type(), text });
    if (message.type() === 'error') result.errors.push(text);
    const marker = '[ENGINE-TEST] ';
    if (text.includes(marker)) {
      try { result.records.push(JSON.parse(text.slice(text.indexOf(marker) + marker.length))); }
      catch (error) { result.errors.push(String(error)); }
    }
  });
  page.on('pageerror', error => result.errors.push(String(error)));
  page.on('dialog', async dialog => { result.errors.push(dialog.message()); await dialog.dismiss(); });
  page.on('response', response => {
    if (response.url().endsWith('.bin')) result.network.push({ url: response.url(), status: response.status() });
  });
  const waitRecord = async (kind, after = 0) => {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
      const record = result.records.slice(after).find(row => row.kind === kind);
      if (record) return record;
      if (page.isClosed()) throw new Error('Page closed before ' + kind);
      await page.waitForTimeout(50);
    }
    throw new Error(arm + ': timed out waiting for ' + kind);
  };
  const advance = async () => {
    const box = await page.locator('canvas').boundingBox();
    assert(box, 'Canvas must exist');
    await page.mouse.click(box.x + box.width / 2, box.y + box.height * 450 / 480);
  };
  let resizes = 0;
  const resize = async (width, height) => {
    const after = result.records.length;
    await page.setViewportSize({ width, height });
    const record = await waitRecord('resize', after);
    assert.equal(record.index, ++resizes, 'Every intended resize must be processed once');
    assert.notEqual(record.scale, record.oldScale, 'Unchanged scale is inconclusive');
  };
  const typeControl = async placeholder => {
    const control = page.getByPlaceholder(placeholder);
    await control.focus();
    await control.pressSequentially('abc', { delay: 80 });
    await page.waitForTimeout(100);
    const editing = result.records.filter(row => row.kind === 'input'
      && row.control === placeholder && row.phase === 'editing').at(-1);
    if (placeholder !== 'lifetime-control') {
      assert(editing, 'Editing event must be observed');
      assert.equal(editing.text, 'abc');
      assert.equal(editing.oldText, 'ab');
      assert.equal(editing.startPosition, 3, 'Preserve DOM selectionStart indexing');
    }
    await control.press('Enter');
    // The lifetime callback removes its own DOM node on submitted.
    if (await control.count()) await control.evaluate(element => element.blur());
    await page.waitForTimeout(150);
  };
  try {
    await page.goto(address + '/' + arm + '/output/TextFieldRegression.html5/index.html',
      { waitUntil: 'load' });
    const ready = await waitRecord('ready');
    resizes = ready.resizes;
    assert.match(ready.version, /(?:^|\D)2026\.3736(?:\D|$)/);
    result.wasm = await page.evaluate(() => window.__engineWasm);
    assert(result.wasm.some(row => row.sha256 === inputs[arm].wasm_sha256
      && row.bytes === inputs[arm].wasm_bytes), 'Executed WASM must match the verified template');
    assert(result.network.some(row => row.status === 200), 'App bundle must be requested');
    await resize(800, 1200);
    await advance();
    assert.equal((await waitRecord('removed')).sizeType, 'nil');
    for (const [width, height] of [[960, 1440], [800, 1200], [960, 1440], [800, 1200]]) {
      await resize(width, height);
    }
    // Reach the arithmetic failure before the independent finalize assertions.
    await advance();
    await waitRecord('named-done');
    await resize(960, 1440);
    if (arm === 'candidate') {
      await advance(); await waitRecord('input-ready');
      await typeControl('function-control');
      const after = result.records.length;
      await advance(); await waitRecord('input-ready', after);
      await typeControl('table-control');
      await advance(); await waitRecord('lifetime-ready');
      await typeControl('lifetime-control');
      assert(result.records.some(row => row.kind === 'lifetime-input' && row.phase === 'editing'));
      await resize(800, 1200);
      await advance();
      const complete = await waitRecord('complete');
      assert.equal(complete.resizes, resizes);
      assert.equal(complete.failures, 0);
      assert.equal(complete.errors, 0);
      assert.equal(result.errors.length, 0, 'All runtime error channels must stay clear');
      result.status = 'PASS';
    } else {
      const text = result.console.map(row => row.text).join('\n');
      assert.match(text, /attempt to perform arithmetic on field ['"]size['"]/,
        'Official 3736 must reach the real removed-object size arithmetic failure');
      result.status = 'FAIL';
      result.expected = true;
    }
  } catch (error) {
    result.status = 'ERROR';
    result.errors.push(String(error.stack));
    throw error;
  } finally {
    try { await page.goto('about:blank'); await context.close(); }
    catch (error) { result.errors.push(String(error)); result.status = 'ERROR'; }
    result.disposed = true;
    fs.writeFileSync(logPath, JSON.stringify(result, null, 2));
    if (result.status === 'ERROR') throw new Error(arm + ': see browser.json');
  }
  return result;
}

(async () => {
  let browser;
  try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, args: flags });
    const address = 'http://127.0.0.1:' + server.address().port;
    const baseline = await runArm(browser, address, 'baseline');
    const candidate = await runArm(browser, address, 'candidate');
    fs.writeFileSync(path.join(root, 'result.json'), JSON.stringify({
      status: 'PASS', baseline: baseline.status, candidate: candidate.status,
      browser: browser.version(), browser_sha256: process.env.CHROMIUM_SHA256,
      playwright: '1.63.0', flags, inputs,
    }, null, 2));
  } finally {
    if (browser) await browser.close();
    server.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
