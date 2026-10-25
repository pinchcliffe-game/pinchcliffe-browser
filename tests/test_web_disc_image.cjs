// Synthetic media only; no original assets are distributed with these tests.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { openImage } = require('../tools/web/disc-image.js');
const BLOCK = 2048;
function both(bytes, offset, value, width = 4) {
  if (width === 4) { bytes.writeUInt32LE(value, offset); bytes.writeUInt32BE(value, offset + 4); }
  else { bytes.writeUInt16LE(value, offset); bytes.writeUInt16BE(value, offset + 2); }
}
function nameBytes(name, joliet) {
  if (!joliet) return Buffer.from(name);
  const b = Buffer.from(name, 'utf16le'); return b.swap16();
}
function record(name, extent, size, directory, joliet = false) {
  const id = typeof name === 'number' ? Buffer.from([name]) : nameBytes(name, joliet);
  const b = Buffer.alloc(33 + id.length + (id.length % 2 ? 0 : 1));
  b[0] = b.length; both(b, 2, extent); both(b, 10, size);
  b[25] = directory ? 2 : 0; both(b, 28, 1, 2); b[32] = id.length; id.copy(b, 33);
  return b;
}
function iso(joliet = false) {
  const b = Buffer.alloc(26 * BLOCK);
  function descriptor(sector, type) {
    const d = b.subarray(sector * BLOCK, (sector + 1) * BLOCK);
    d[0] = type; d.write('CD001', 1); d[6] = 1;
    if (type !== 255) {
      both(d, 80, 26); both(d, 120, 1, 2); both(d, 124, 1, 2); both(d, 128, BLOCK, 2);
      nameBytes('TEST', type === 2).copy(d, 40);
      record(0, 20, BLOCK, true).copy(d, 156);
      if (type === 2) d.write('%/E', 88);
    }
  }
  descriptor(16, 1);
  if (joliet) descriptor(17, 2);
  descriptor(joliet ? 18 : 17, 255);
  const dir = Buffer.concat([record(0, 20, BLOCK, true), record(1, 20, BLOCK, true),
    record(joliet ? 'Spillø' : 'DATA', 21, BLOCK, true, joliet)]);
  dir.copy(b, 20 * BLOCK);
  Buffer.concat([record(0, 21, BLOCK, true), record(1, 20, BLOCK, true),
    record('menu.bin;1', 22, 3000, false, joliet)]).copy(b, 21 * BLOCK);
  for (let i = 0; i < 3000; i++) b[22 * BLOCK + i] = i % 251;
  return b;
}
function raw(isoBytes) {
  const b = Buffer.alloc(isoBytes.length / BLOCK * 2352, 0xa5);
  for (let i = 0; i < isoBytes.length / BLOCK; i++) {
    b[i * 2352] = b[i * 2352 + 11] = 0; b.fill(255, i * 2352 + 1, i * 2352 + 11);
    b[i * 2352 + 15] = 1; isoBytes.copy(b, i * 2352 + 16, i * BLOCK, (i + 1) * BLOCK);
  }
  return b;
}
const open = b => openImage(b.length, (offset, size) => b.subarray(offset, offset + size));
for (const joliet of [false, true]) for (const mode of ['iso', 'raw']) {
  test(`${mode} ${joliet ? 'Joliet' : 'primary'} preserves nested names and reads across sectors`, () => {
    const original = iso(joliet), b = mode === 'raw' ? raw(original) : original, image = open(b);
    const path = `${joliet ? 'Spillø' : 'DATA'}/menu.bin`;
    assert.deepEqual([...image.files.keys()], [path]);
    const entry = image.files.get(path);
    assert.deepEqual(Buffer.from(image.readFile(entry, 2000, 1000)), original.subarray(22 * BLOCK + 2000, 22 * BLOCK + 3000));
    assert.throws(() => image.readFile(entry, 2999, 2), /outside/);
    assert.throws(() => image.readFile(entry, NaN, 1), /outside/);
  });
}
test('rejects malformed images before exposing files', () => {
  const variants = [];
  let b = iso(); b[16 * BLOCK + 1] = 0; variants.push(b);
  b = iso(); b[16 * BLOCK + 84] = 1; variants.push(b); // mismatched byte orders
  b = iso(); both(b, 16 * BLOCK + 80, 100); variants.push(b); // volume past EOF
  b = iso(); b[21 * BLOCK + 68 + 25] = 0x80; variants.push(b); // multi extent
  b = iso(); both(b, 20 * BLOCK + 68 + 2, 20); variants.push(b); // cycle
  b = iso(); b.write('../x', 20 * BLOCK + 68 + 33); variants.push(b);
  b = raw(iso()); b[20 * 2352 + 15] = 2; variants.push(b);
  variants.push(iso().subarray(0, -1), Buffer.alloc(10));
  for (const bytes of variants) assert.throws(() => open(bytes));
});
test('fails on short byte readers and ambiguous filenames', () => {
  const b = iso();
  assert.throws(() => openImage(b.length, () => new Uint8Array()), /Short/);
  const duplicate = record('MENU.BIN;1', 22, 1, false);
  duplicate.copy(b, 21 * BLOCK + 68 + b[21 * BLOCK + 68]);
  assert.throws(() => open(b), /Duplicate/);
});
