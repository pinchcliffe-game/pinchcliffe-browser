/* Local installation reader. Shared by the page, its indexing worker and tests.
 * Only user-selected Files/Blobs enter this module; there is no HTTP fallback. */
(function (root) {
  'use strict';
  const MOUNT = '/src/data/game/';
  const EXE_HASH = '324b3e91f7b7d3c43b3422e7380196a5195260fff29187cb89f82df567e13daf';

  // Structured-cloneable, file-backed sources. Raw CD sectors and merged
  // packages are sliced only when an asset is read; never copy whole discs
  // into memory or create hundreds of thousands of Blob parts at import.
  function sliceSource(source, start = 0, end = source.size) {
    if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start < 0 || end < start || end > source.size) {
      throw new Error('Ugyldig lokalt filområde');
    }
    if (source instanceof Blob) return source.slice(start, end);
    const parts = [];
    if (source.kind === 'disc') {
      if (source.sectorSize === 2048) return source.file.slice(source.offset + start, source.offset + end);
      if (source.sectorSize !== 2352) throw new Error('Ukjent CD-format');
      for (let at = source.offset + start; at < source.offset + end;) {
        const within = at % 2048, count = Math.min(2048 - within, source.offset + end - at);
        const physical = Math.floor(at / 2048) * 2352 + 16 + within;
        parts.push(source.file.slice(physical, physical + count));
        at += count;
      }
    } else if (source.kind === 'joined') {
      let at = 0;
      for (const part of source.parts) {
        const length = part.end - part.start;
        const from = Math.max(start, at), to = Math.min(end, at + length);
        if (from < to) parts.push(sliceSource(part.source, part.start + from - at, part.start + to - at));
        at += length;
      }
    } else throw new Error('Ukjent lokal filtype');
    const blob = new Blob(parts);
    if (blob.size !== end - start) throw new Error('Avkortet lokal fil');
    return blob;
  }

  function select(files) {
    const selected = Array.from(files, file => {
      const path = file.webkitRelativePath || file.name;
      const parts = path.split('/');
      if (parts.some(part => !part || part === '.' || part === '..' || /[\\\x00]/.test(part))) {
        throw new Error('Ugyldig filsti. Velg spillmappen på nytt.');
      }
      return { path, file };
    });
    const executables = selected.filter(item => /(^|\/)fgp\.exe$/i.test(item.path));
    if (executables.length !== 1) {
      throw new Error('Velg én installert spillmappe som inneholder FGP.exe, Data og Language.');
    }
    const exe = executables[0];
    const prefix = exe.path.slice(0, -exe.file.name.length);
    const packages = new Map();
    for (const item of selected) {
      if (!item.path.startsWith(prefix)) continue;
      const relative = item.path.slice(prefix.length);
      const match = /^(data[1-3]?|language[1-3]?)\/([^/]+\.bin)$/i.exec(relative);
      if (!match) continue; // Never import profiles, DLLs or unrelated files.
      const layer = match[1].toLowerCase().replace(/^(data|language)/, word => word[0].toUpperCase() + word.slice(1));
      const path = layer + '/' + match[2].toLowerCase();
      if (packages.has(path)) throw new Error('Flere filer har samme navn: ' + path);
      packages.set(path, item.file);
    }
    for (const path of ['Data/menu.bin', 'Data/common.bin', 'Language/common.bin']) {
      if (!packages.has(path)) throw new Error('Spillmappen mangler ' + path + '. Velg hele installasjonen.');
    }
    return { executable: exe.file, packages };
  }

  async function validateExecutable(file) {
    // All engine RVAs target update 1.2; see REVERSE_ENGINEERING.md provenance.
    if (file.size !== 1153024) throw new Error('Denne versjonen krever FGP.exe fra oppdatering 1.2. Oppdater din egen installasjon først.');
    if (!globalThis.crypto || !crypto.subtle) throw new Error('Åpne spillet via HTTPS (eller localhost) for å kontrollere spillfilene lokalt.');
    const bytes = new Uint8Array(await file.arrayBuffer());
    const hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), byte => byte.toString(16).padStart(2, '0')).join('');
    if (hash !== EXE_HASH) throw new Error('FGP.exe støttes ikke. Bruk den originale oppdatering 1.2 fra din egen installasjon.');
    return bytes;
  }

  function packageIndex(size, read) {
    if (size < 12 || size > 0xffffffff) throw new Error('Ugyldig pakkestørrelse');
    const header = read(0, 12);
    const view = new DataView(header.buffer, header.byteOffset, header.byteLength);
    if (String.fromCharCode(...header.subarray(0, 4)) !== 'DATA' || view.getUint32(4, true) !== 0x100) {
      throw new Error('Ikke en støttet DATA-pakke');
    }
    const count = view.getUint32(8, true);
    if (count > Math.floor((size - 12) / 264)) throw new Error('Ugyldig antall oppføringer');
    const entries = [];
    let position = 12;
    for (let i = 0; i < count; i++) {
      if (position + 264 > size) throw new Error('Avkortet pakkehode');
      const entry = read(position, 264);
      const payload = new DataView(entry.buffer, entry.byteOffset, entry.byteLength).getUint32(260, true);
      const nameEnd = entry.subarray(0, 72).indexOf(0);
      const name = entry.slice(0, nameEnd < 0 ? 72 : nameEnd);
      position += 264;
      if (position + payload > size) throw new Error('Avkortet pakkeinnhold');
      entries.push({ name, offset: position, size: payload });
      position += payload;
    }
    if (position !== size) throw new Error('Uventede data etter pakken');
    return entries;
  }

  function encodeIndex(entries) {
    const bytes = new Uint8Array(12 + entries.reduce((sum, entry) => sum + 10 + entry.name.length, 0));
    bytes.set(new TextEncoder().encode('FGPIDX1\n'));
    const view = new DataView(bytes.buffer);
    view.setUint32(8, entries.length, true);
    let at = 12;
    for (const entry of entries) {
      view.setUint16(at, entry.name.length, true); at += 2;
      bytes.set(entry.name, at); at += entry.name.length;
      view.setUint32(at, entry.offset, true);
      view.setUint32(at + 4, entry.size, true); at += 8;
    }
    return bytes;
  }

  function indexPackages(packages, readBlob, progress = () => {}) {
    const indexed = new Map();
    for (const [path, file] of packages) {
      try {
        indexed.set(path, { file, entries: packageIndex(file.size, (offset, size) => readBlob(sliceSource(file, offset, offset + size))) });
      } catch (error) { throw new Error(path + ': ' + error.message); }
      progress(indexed.size, packages.size);
    }
    // Match assemble_gamedata.py: localized entries follow their Data layer.
    // Blob composition keeps the original files local and avoids a 1.3 GB copy.
    for (const [path, language] of Array.from(indexed)) {
      if (!path.startsWith('Language')) continue;
      const target = path.replace(/^Language/, 'Data');
      const data = indexed.get(target);
      if (!data) { indexed.set(target, language); continue; }
      // Prepared installations already have this language package as their
      // tail. Do not append it twice: scenes also enumerate package entries.
      const tail = data.entries.slice(-language.entries.length);
      const shift = data.file.size - language.file.size;
      if (!language.entries.length || (tail.length === language.entries.length && tail.every((entry, i) => {
        const other = language.entries[i];
        return entry.offset === other.offset + shift && entry.size === other.size &&
          entry.name.length === other.name.length && entry.name.every((byte, j) => byte === other.name[j]);
      }))) continue;
      const entries = data.entries.concat(language.entries.map(entry => ({ ...entry, offset: entry.offset + data.file.size - 12 })));
      const header = new Uint8Array(readBlob(sliceSource(data.file, 0, 12)));
      new DataView(header.buffer).setUint32(8, entries.length, true);
      const file = { kind: 'joined', size: data.file.size + language.file.size - 12, parts: [
        { source: new Blob([header]), start: 0, end: 12 },
        { source: data.file, start: 12, end: data.file.size },
        { source: language.file, start: 12, end: language.file.size },
      ] };
      if (file.size > 0xffffffff) throw new Error('Pakken er for stor: ' + target);
      indexed.set(target, { file, entries });
    }
    return Array.from(indexed, ([path, item]) => ({ path, file: item.file, index: encodeIndex(item.entries) }));
  }

  function readRange(files, path, offset, size) {
    const file = files.get(path);
    if (!file || !Number.isSafeInteger(offset) || !Number.isSafeInteger(size) || offset < 0 || size < 0 || offset + size > file.size) {
      throw new Error('Kan ikke lese lokal spillfil: ' + path);
    }
    if (!size) return new Uint8Array();
    // Blob URLs address local bytes, never the web server. Revoke each slice
    // immediately. Synchronous reads preserve Package::data()'s contract.
    const url = URL.createObjectURL(sliceSource(file, offset, offset + size));
    try {
      const request = new XMLHttpRequest();
      request.open('GET', url, false);
      request.overrideMimeType('text/plain; charset=x-user-defined');
      request.send(null);
      if (request.status !== 200 || request.responseText.length !== size) throw new Error('Lokal fil kunne ikke leses');
      return Uint8Array.from(request.responseText, character => character.charCodeAt(0) & 255);
    } finally { URL.revokeObjectURL(url); }
  }

  root.FGPLocalFiles = { MOUNT, select, validateExecutable, packageIndex, encodeIndex, indexPackages, readRange, sliceSource };
  if (typeof module !== 'undefined') module.exports = root.FGPLocalFiles;
})(globalThis);
