/* Index on a worker so large installations never block the picker/progress UI. */
importScripts('local-files.js', 'disc-image.js');
self.onmessage = ({ data }) => {
  try {
    const reader = new FileReaderSync();
    const read = blob => new Uint8Array(reader.readAsArrayBuffer(blob));
    const sources = new Map(data.packages || []), loose = [];
    if (data.discs) {
      if (data.discs.length !== 2) throw new Error('Velg begge CD-bildene samtidig (ISO eller BIN).');
      const volumes = new Set();
      for (const file of data.discs) {
        const image = FGPDiscImage.openImage(file.size, (offset, size) => read(file.slice(offset, offset + size)));
        const volume = image.volume.toUpperCase();
        if (!['FGPGULL1', 'FGPGULL2'].includes(volume) || volumes.has(volume)) {
          throw new Error('Velg ett CD-bilde av FGPGULL1 og ett av FGPGULL2.');
        }
        volumes.add(volume);
        for (const [name, entry] of image.files) {
          const match = /^(data[1-3]?|lang[1-3]?)\/([^/]+\.bin)$/i.exec(name);
          if (match) {
            const layer = match[1].toLowerCase().replace(/^data/, 'Data').replace(/^lang/, 'Language');
            const path = layer + '/' + match[2].toLowerCase();
            if (sources.has(path)) throw new Error('Flere CD-filer har samme navn: ' + path);
            sources.set(path, { kind: 'disc', file, sectorSize: image.sectorSize, ...entry });
          } else if (/^data\/fence\/tunes\/[^/]+\.tun$/i.test(name)) {
            if (entry.size > 1024 * 1024) throw new Error('Ugyldig melodifil');
            loose.push({ path: name.toLowerCase().replace(/^data/, 'Data'), bytes: image.readFile(entry) });
          }
        }
      }
      for (const path of ['Data/menu.bin', 'Data/common.bin', 'Language/common.bin', 'Data3/sceneindex.bin']) {
        if (!sources.has(path)) throw new Error('CD-bildene mangler ' + path);
      }
    }
    const packages = FGPLocalFiles.indexPackages(sources, read,
      (done, total) => self.postMessage({ done, total }));
    self.postMessage({ packages, loose }, [...packages.map(item => item.index.buffer), ...loose.map(item => item.bytes.buffer)]);
  } catch (error) { self.postMessage({ error: error.message }); }
};
