// Synthetic packages only. Run with node --test tests/test_web_local_files.cjs.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const local = require('../tools/web/local-files.js');

function packageBytes(name, payload) {
  const bytes = new Uint8Array(276 + payload.length);
  bytes.set(Buffer.from('DATA'));
  const view = new DataView(bytes.buffer);
  view.setUint32(4, 0x100, true); view.setUint32(8, 1, true);
  bytes.set(Buffer.from(name), 12); view.setUint32(272, payload.length, true);
  bytes.set(payload, 276);
  return bytes;
}
function file(path, bytes = new Uint8Array()) {
  const f = new File([bytes], path.split('/').at(-1));
  Object.defineProperty(f, 'webkitRelativePath', { value: path });
  return f;
}
const selection = () => [file('Spill/FGP.exe'), file('Spill/DATA/MENU.BIN'), file('Spill/Data/common.bin'), file('Spill/Language/common.bin')];

test('selects one installation, normalizes Windows casing, ignores unrelated/private files', () => {
  const selected = local.select([...selection(), file('Spill/Profiles/owner.pfl'), file('Spill/secrets.txt')]);
  assert.deepEqual([...selected.packages.keys()], ['Data/menu.bin', 'Data/common.bin', 'Language/common.bin']);
  assert.equal(selected.executable.name, 'FGP.exe');
});
test('rejects missing, ambiguous, colliding and unsafe paths', () => {
  assert.throws(() => local.select([]), /FGP.exe/);
  assert.throws(() => local.select([file('FGP.exe')]), /Data\/menu/);
  assert.throws(() => local.select([...selection(), file('Spill/backup/FGP.exe')]), /én/);
  assert.throws(() => local.select([...selection(), file('Spill/Data/menu.bin')]), /samme navn/);
  for (const path of ['Spill/../FGP.exe', '/Spill/FGP.exe', 'Spill\\FGP.exe']) assert.throws(() => local.select([file(path)]), /filsti/);
});
test('rejects unsupported executable before indexing', async () => {
  await assert.rejects(local.validateExecutable(file('FGP.exe')), /oppdatering 1.2/);
  await assert.rejects(local.validateExecutable(file('FGP.exe', new Uint8Array(1153024))), /støttes ikke/);
});
test('indexes payload offsets and preserves every name byte', () => {
  const bytes = packageBytes('bitmap\\title.bmp', [0, 128, 255]);
  const entries = local.packageIndex(bytes.length, (o, n) => bytes.slice(o, o + n));
  assert.equal(entries.length, 1);
  assert.deepEqual(entries[0], { name: new Uint8Array(Buffer.from('bitmap\\title.bmp')), offset: 276, size: 3 });
  const index = local.encodeIndex(entries);
  assert.equal(Buffer.from(index.slice(0, 8)).toString(), 'FGPIDX1\n');
  assert.equal(new DataView(index.buffer).getUint32(8, true), 1);
  assert.equal(new DataView(index.buffer).getUint32(index.length - 8, true), 276);
});
test('refuses bad package headers, counts, truncated payloads and trailing bytes', () => {
  const good = packageBytes('x', [1, 2]);
  const badMagic = good.slice(); badMagic[0] = 0;
  const badCount = good.slice(); new DataView(badCount.buffer).setUint32(8, 0xffffffff, true);
  const badVersion = good.slice(); badVersion[4] = 1;
  for (const bytes of [good.slice(0, 10), good.slice(0, -1), new Uint8Array([...good, 0]), badMagic, badCount, badVersion]) {
    assert.throws(() => local.packageIndex(bytes.length, (o, n) => bytes.slice(o, o + n)));
  }
});
test('missing or out-of-bounds local reads fail before any URL or network operation', () => {
  const files = new Map([['Data/menu.bin', new Blob(['abc'])]]);
  assert.throws(() => local.readRange(files, 'missing.bin', 0, 1));
  for (const [offset, size] of [[-1, 1], [0, -1], [0, 4], [0.5, 1], [0, NaN]]) {
    assert.throws(() => local.readRange(files, 'Data/menu.bin', offset, size));
  }
  assert.deepEqual(local.readRange(files, 'Data/menu.bin', 3, 0), new Uint8Array());
});
test('file-backed ISO/raw ranges and merged ranges preserve bytes across every boundary', async () => {
  const bytes = Uint8Array.from({ length: 6144 }, (_, i) => i % 251);
  const raw = new Uint8Array(3 * 2352).fill(0xff);
  for (let i = 0; i < 3; i++) raw.set(bytes.subarray(i * 2048, (i + 1) * 2048), i * 2352 + 16);
  for (const [file, sectorSize] of [[new Blob([bytes]), 2048], [new Blob([raw]), 2352]]) {
    const source = { kind: 'disc', file, sectorSize, offset: 2000, size: 3000 };
    const joined = structuredClone({ kind: 'joined', size: 3004, parts: [
      { source: new Blob(['HEAD']), start: 0, end: 4 }, { source, start: 0, end: 3000 },
    ] });
    const expected = new Uint8Array(3004); expected.set(Buffer.from('HEAD')); expected.set(bytes.subarray(2000, 5000), 4);
    for (const [start, end] of [[0, 3004], [0, 0], [2, 100], [48, 2052], [3004, 3004]]) {
      assert.deepEqual(new Uint8Array(await local.sliceSource(joined, start, end).arrayBuffer()), expected.slice(start, end));
    }
    assert.throws(() => local.sliceSource(joined, -1, 2));
    assert.throws(() => local.sliceSource(joined, 0, 3005));
  }
});
