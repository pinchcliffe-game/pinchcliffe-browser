// Optional headless test against user-owned images, with HTTP(S) blocked.
// node tests/test_web_disc_browser.cjs CD1.iso CD2.iso (or raw .bin images)
// Same FGP_PLAYWRIGHT_MODULE/FGP_TEST_BROWSER/FGP_BROWSER_EXECUTABLE overrides
// as test_web_browser.cjs. Images stay local; no web server is needed.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { audit } = require('../tools/web/audit-discs.cjs');
const playwright = require(process.env.FGP_PLAYWRIGHT_MODULE || 'playwright');
(async () => {
  const paths = process.argv.slice(2).map(p => path.resolve(p));
  assert.equal(paths.length, 2, 'Provide the two local disc images');
  const expected = paths.map(audit).map(({ volume, packages, entries, packageBytes }) => ({ volume, packages, entries, packageBytes }));
  const browser = await playwright[process.env.FGP_TEST_BROWSER || 'chromium'].launch({
    headless: true,
    ...(process.env.FGP_BROWSER_EXECUTABLE ? { executablePath: process.env.FGP_BROWSER_EXECUTABLE } : {}),
  });
  try {
    const page = await browser.newPage();
    const requests = [];
    page.on('request', request => { if (/^https?:/.test(request.url())) requests.push(request.url()); });
    await page.route(/^https?:/, route => route.abort());
    await page.setContent('<input type="file" multiple>');
    await page.locator('input').setInputFiles(paths);
    const source = ['disc-image.js', 'local-files.js'].map(name => fs.readFileSync(path.join(__dirname, '../tools/web', name), 'utf8')).join('\n');
    const result = await page.evaluate(source => new Promise((resolve, reject) => {
      const workerSource = source + `
        onmessage = ({data: files}) => {
          try {
            const reader = new FileReaderSync();
            postMessage(files.map(file => {
              const image = FGPDiscImage.openImage(file.size, (offset, size) =>
                new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + size))));
              let packages = 0, entries = 0, packageBytes = 0;
              for (const [name, entry] of image.files) {
                if (!/^(data[1-3]?|lang[1-3]?)\\/[^/]+\\.bin$/i.test(name)) continue;
                entries += FGPLocalFiles.packageIndex(entry.size, (o, n) => image.readFile(entry, o, n)).length;
                packages++; packageBytes += entry.size;
              }
              return {volume: image.volume, packages, entries, packageBytes};
            }));
          } catch (error) { postMessage({error: error.message}); }
        };`;
      const url = URL.createObjectURL(new Blob([workerSource], { type: 'text/javascript' }));
      const worker = new Worker(url);
      const cleanup = () => { worker.terminate(); URL.revokeObjectURL(url); clearTimeout(timer); };
      const timer = setTimeout(() => { cleanup(); reject(new Error('Disc indexing timed out')); }, 60000);
      worker.onerror = event => { cleanup(); reject(new Error(event.message)); };
      worker.onmessage = ({data}) => { cleanup(); data.error ? reject(new Error(data.error)) : resolve(data); };
      worker.postMessage(Array.from(document.querySelector('input').files));
    }), source);
    assert.deepEqual(result, expected);
    assert.deepEqual(requests, []);
    console.log('Local disc indexing passed with HTTP(S) blocked:', JSON.stringify(result));
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
