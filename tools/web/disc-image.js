/* Read-only ISO 9660/Joliet reader for locally selected ISO or MODE1/2352
 * images. Inject a synchronous byte reader (FileReaderSync in a worker).
 * Disc executables remain incompatible with the engine's update-1.2 table
 * reader; select the 1.2 executable separately. No files are executed or fetched. */
(function (root) {
  'use strict';
  const BLOCK = 2048;
  function openImage(size, readBytes) {
    function physical(offset, length) {
      if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(length) ||
          offset < 0 || length < 0 || offset + length > size) throw new Error('Disc range outside image');
      const bytes = readBytes(offset, length);
      if (!(bytes instanceof Uint8Array) || bytes.length !== length) throw new Error('Short disc read');
      return bytes;
    }
    const signature = bytes => String.fromCharCode(...bytes.subarray(1, 6)) === 'CD001' && bytes[6] === 1;
    let sectorSize = BLOCK, dataOffset = 0;
    if (size < 17 * BLOCK) throw new Error('Truncated disc image');
    if (!signature(physical(16 * BLOCK, 7))) {
      sectorSize = 2352; dataOffset = 16;
      if (size < 17 * sectorSize || !signature(physical(16 * sectorSize + dataOffset, 7))) {
        throw new Error('Expected ISO or MODE1/2352 data image');
      }
    }
    if (size % sectorSize) throw new Error('Truncated disc sector');
    let logicalSize = size / sectorSize * BLOCK;
    function read(offset, length) {
      if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(length) ||
          offset < 0 || length < 0 || offset + length > logicalSize) throw new Error('Disc extent outside volume');
      const result = new Uint8Array(length);
      for (let at = 0; at < length;) {
        const position = offset + at, sector = Math.floor(position / BLOCK), within = position % BLOCK;
        const count = Math.min(length - at, BLOCK - within);
        if (dataOffset) {
          const header = physical(sector * sectorSize, 16);
          if (header[0] !== 0 || header[11] !== 0 || header[15] !== 1 ||
              !header.subarray(1, 11).every(byte => byte === 255)) throw new Error('Unsupported raw sector mode');
        }
        result.set(physical(sector * sectorSize + dataOffset + within, count), at);
        at += count;
      }
      return result;
    }
    function both(bytes, at, width) {
      const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const little = width === 4 ? v.getUint32(at, true) : v.getUint16(at, true);
      const big = width === 4 ? v.getUint32(at + width) : v.getUint16(at + width);
      if (little !== big) throw new Error('Inconsistent ISO byte orders');
      return little;
    }
    let primary, joliet, terminated = false;
    for (let sector = 16; sector < 80; sector++) {
      const descriptor = read(sector * BLOCK, BLOCK);
      if (!signature(descriptor)) throw new Error('Invalid volume descriptor');
      if (descriptor[0] === 255) { terminated = true; break; }
      if (descriptor[0] === 1) primary = descriptor;
      if (descriptor[0] === 2 && descriptor[88] === 37 && descriptor[89] === 47 &&
          [64, 67, 69].includes(descriptor[90])) joliet = descriptor;
    }
    if (!primary || !terminated) throw new Error('Incomplete volume descriptors');
    const descriptor = joliet || primary;
    if (both(descriptor, 128, 2) !== BLOCK || both(descriptor, 120, 2) !== 1 ||
        both(descriptor, 124, 2) !== 1) throw new Error('Unsupported ISO volume layout');
    const volumeSize = both(descriptor, 80, 4) * BLOCK;
    if (!volumeSize || volumeSize > logicalSize) throw new Error('Truncated ISO volume');
    logicalSize = volumeSize;
    function decode(bytes) {
      if (!joliet) return String.fromCharCode(...bytes);
      if (bytes.length % 2) throw new Error('Invalid Joliet name');
      let name = '';
      for (let i = 0; i < bytes.length; i += 2) name += String.fromCharCode(bytes[i] * 256 + bytes[i + 1]);
      return name;
    }
    function record(bytes) {
      if (bytes.length < 34 || bytes[0] !== bytes.length || bytes[32] + 33 > bytes.length) throw new Error('Truncated directory record');
      if (bytes[1] || bytes[26] || bytes[27] || bytes[25] & 0x80 || both(bytes, 28, 2) !== 1) {
        throw new Error('Unsupported interleaved, extended or multi-extent file');
      }
      const offset = both(bytes, 2, 4) * BLOCK, length = both(bytes, 10, 4);
      if (offset + length > logicalSize) throw new Error('Directory entry outside volume');
      return { offset, size: length, directory: Boolean(bytes[25] & 2) };
    }
    const rootLength = descriptor[156];
    const rootRecord = record(descriptor.slice(156, 156 + rootLength));
    if (!rootRecord.directory) throw new Error('Root is not a directory');
    const files = new Map(), directories = new Set(), paths = new Set();
    let directoryBytes = 0, entryCount = 0;
    function walk(entry, prefix, depth) {
      if (depth > 32 || directories.has(entry.offset)) throw new Error('Cyclic or excessively deep directory');
      directories.add(entry.offset);
      directoryBytes += entry.size;
      if (directoryBytes > 16 * 1024 * 1024) throw new Error('Excessive directory data');
      const bytes = read(entry.offset, entry.size);
      for (let at = 0; at < bytes.length;) {
        const length = bytes[at];
        if (!length) { at += BLOCK - at % BLOCK; continue; }
        if (length > BLOCK - at % BLOCK || at + length > bytes.length) throw new Error('Directory record crosses boundary');
        const raw = bytes.slice(at, at + length), item = record(raw);
        at += length;
        if (++entryCount > 100000) throw new Error('Excessive directory entries');
        const identifier = raw.slice(33, 33 + raw[32]);
        if (identifier.length === 1 && identifier[0] <= 1) continue;
        const name = decode(identifier).replace(/;\d+$/, '');
        if (!name || name === '.' || name === '..' || /[\/\\\x00-\x1f\x7f]/.test(name)) throw new Error('Unsafe disc filename');
        const path = prefix + name, key = path.toLowerCase();
        if (paths.has(key)) throw new Error('Duplicate disc filename');
        paths.add(key);
        if (item.directory) walk(item, path + '/', depth + 1);
        else files.set(path, { offset: item.offset, size: item.size });
      }
    }
    walk(rootRecord, '', 0);
    return {
      volume: decode(descriptor.subarray(40, 72)).replace(/[\x00 ]+$/, ''), sectorSize, files,
      readFile(entry, offset = 0, length = entry.size - offset) {
        if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(length) || offset < 0 || length < 0 || offset + length > entry.size) {
          throw new Error('Read outside disc file');
        }
        return read(entry.offset + offset, length);
      }
    };
  }
  root.FGPDiscImage = { openImage };
  if (typeof module !== 'undefined') module.exports = root.FGPDiscImage;
})(globalThis);
