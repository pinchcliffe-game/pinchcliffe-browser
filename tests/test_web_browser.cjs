/* Headless browser integration test (Playwright is an optional test dependency).
 * node tests/test_web_browser.cjs http://127.0.0.1:8765
 * FGP_TEST_GAME_DIR=/absolute/installed/game additionally starts the real game.
 * Or set FGP_TEST_DISC1, FGP_TEST_DISC2 and FGP_TEST_EXE to local image/exe paths.
 * FGP_PLAYWRIGHT_MODULE and FGP_BROWSER_EXECUTABLE may point at existing tooling;
 * FGP_TEST_BROWSER defaults to chromium. No original data is served or committed.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const playwright = require(process.env.FGP_PLAYWRIGHT_MODULE || 'playwright');

(async () => {
  const browser = await playwright[process.env.FGP_TEST_BROWSER || 'chromium'].launch({
    headless: true,
    ...(process.env.FGP_BROWSER_EXECUTABLE ? { executablePath: process.env.FGP_BROWSER_EXECUTABLE } : {}),
    ...(process.env.FGP_TEST_BROWSER === 'webkit' ? {} : { args: ['--mute-audio'] }),
  });
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'fgp-byo-'));
  try {
    const page = await browser.newPage({ viewport: { width: 1100, height: 800 } });
    const requests = [], errors = [], logs = [];
    page.on('request', request => {
      if (/^https?:/.test(request.url())) requests.push({ url: request.url(), method: request.method() });
    });
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => logs.push(message.text()));
    const base = process.argv[2] || 'http://127.0.0.1:8765';
    const scene = process.env.FGP_TEST_SCENE || 'yard';
    await page.goto(base + '/FGPNativePOC.html?dev=1&view=' + encodeURIComponent(scene));
    await page.waitForFunction(() => !document.getElementById('choose').disabled);
    assert(await page.locator('#start').isDisabled());

    // Actual worker, Blob composition, index offsets, and synchronous binary
    // reads, including high bytes and a language-only package.
    const merged = await page.evaluate(async () => {
      const make = (name, payload) => {
        const bytes = new Uint8Array(276 + payload.length);
        bytes.set(new TextEncoder().encode('DATA'));
        const view = new DataView(bytes.buffer);
        view.setUint32(4, 0x100, true); view.setUint32(8, 1, true);
        bytes.set(new TextEncoder().encode(name), 12);
        view.setUint32(272, payload.length, true); bytes.set(payload, 276);
        return new Blob([bytes]);
      };
      const original = make('base', [0, 128, 255]);
      const packages = await indexInstallation(new Map([
        ['Data/menu.bin', original],
        ['Language/menu.bin', make('title', [1, 254])],
        ['Language/help.bin', make('help', [7])],
      ]));
      const again = await indexInstallation(new Map(packages.map(item => [item.path, item.file])));
      const againMenu = again.find(item => item.path === 'Data/menu.bin');
      if (new DataView(againMenu.index.buffer).getUint32(8, true) !== 2) throw new Error('Already merged data was appended twice');
      const files = new Map(packages.map(item => [item.path, item.file]));
      const menu = packages.find(item => item.path === 'Data/menu.bin');
      const entries = FGPLocalFiles.packageIndex(menu.file.size, (offset, size) => FGPLocalFiles.readRange(files, menu.path, offset, size));
      const bytes = entries.map(entry => Array.from(FGPLocalFiles.readRange(files, menu.path, entry.offset, entry.size)));
      return { bytes, originalSize: original.size, paths: [...files.keys()], count: new DataView(menu.index.buffer).getUint32(8, true) };
    });
    assert.deepEqual(merged.bytes, [[0, 128, 255], [1, 254]]);
    assert.equal(merged.originalSize, 279);
    assert.equal(merged.count, 2);
    assert(merged.paths.includes('Data/help.bin'));

    // Recover from a rejected folder without ever enabling Start.
    await fs.mkdir(path.join(temporary, 'Data'));
    await fs.mkdir(path.join(temporary, 'Language'));
    for (const name of ['FGP.exe', 'Data/menu.bin', 'Data/common.bin', 'Language/common.bin']) {
      await fs.writeFile(path.join(temporary, name), 'invalid');
    }
    await page.locator('#game-folder').setInputFiles(temporary);
    await page.waitForFunction(() => document.getElementById('status').textContent.includes('oppdatering 1.2'));
    assert(await page.locator('#start').isDisabled());
    assert(!(await page.locator('#choose').isDisabled()));

    await page.evaluate(() => new Promise((resolve, reject) => {
      FS.writeFile('/src/data/game/Profiles/byo-test.txt', 'profile survives reload');
      FS.syncfs(false, error => error ? reject(error) : resolve());
    }));
    await page.reload();
    await page.waitForFunction(() => !document.getElementById('choose').disabled);
    assert(await page.locator('#start').isDisabled());
    assert.equal(await page.evaluate(() => FS.readFile('/src/data/game/Profiles/byo-test.txt', { encoding: 'utf8' })), 'profile survives reload');

    if (process.env.FGP_TEST_GAME_DIR || process.env.FGP_TEST_DISC1) {
      if (process.env.FGP_TEST_DISC1) {
        const discs = [process.env.FGP_TEST_DISC1, process.env.FGP_TEST_DISC2].map(p => path.resolve(p));
        const exe = path.resolve(process.env.FGP_TEST_EXE);
        await page.locator('#disc-images').setInputFiles(discs.slice(0, 1));
        assert(await page.locator('#choose-exe').isDisabled());
        assert(await page.locator('#start').isDisabled());
        await page.locator('#disc-images').setInputFiles([discs[0], discs[0]]);
        await page.locator('#game-executable').setInputFiles(exe);
        await page.waitForFunction(() => document.getElementById('status').textContent.includes('ett CD-bilde'));
        assert(await page.locator('#start').isDisabled());
        await page.locator('#disc-images').setInputFiles(discs);
        await page.locator('#game-executable').setInputFiles(path.join(temporary, 'FGP.exe'));
        await page.waitForFunction(() => document.getElementById('status').textContent.includes('oppdatering 1.2'));
        assert(await page.locator('#start').isDisabled());
        await page.locator('#game-executable').setInputFiles(exe);
      } else {
        await page.locator('#game-folder').setInputFiles(path.resolve(process.env.FGP_TEST_GAME_DIR));
      }
      await page.waitForFunction(() => !document.getElementById('start').disabled, null, { timeout: 180000 });
      if (process.env.FGP_TEST_DISC1) {
        assert.equal(await page.evaluate(() => localPackages.size), 198);
        assert.equal(await page.evaluate(() => FS.readdir('/src/data/game/Data/fence/tunes').filter(name => name.endsWith('.tun')).length), 10);
      }
      // Once the engine is loaded, even blocking all HTTP requests cannot
      // stop asset reads from the user's selected files.
      await page.route(/^https?:/, route => route.abort());
      await page.locator('#start').click();
      await page.waitForFunction(() => !document.getElementById('overlay'));
      await page.waitForTimeout(6000);
      assert(logs.some(line => line.includes('Loaded native') && line.includes('/' + scene + '.bin')), logs.slice(-20).join('\n'));
      assert.equal(await page.evaluate(() => FS.readFile('/src/data/game/Profiles/byo-test.txt', { encoding: 'utf8' })), 'profile survives reload');
      console.log('Real local import and ' + scene + ' startup passed with HTTP blocked.');
    }
    assert.deepEqual(errors, []);
    const allowed = new Set(['FGPNativePOC.html', 'FGPNativePOC.js', 'FGPNativePOC.wasm', 'local-files.js', 'local-files-worker.js', 'disc-image.js', 'api/scores']);
    for (const request of requests) {
      assert(allowed.has(new URL(request.url).pathname.slice(1)), request.url);
      assert.equal(request.method, 'GET', 'unexpected upload: ' + request.url);
    }
    console.log('Browser import, local binary reads, language merge, invalid-folder recovery, profile persistence and network boundary passed.');
  } finally {
    await browser.close();
    await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
