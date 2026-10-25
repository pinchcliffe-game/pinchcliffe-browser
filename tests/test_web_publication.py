#!/usr/bin/env python3
"""Verify the asset boundary against a dirty old build over real HTTP."""
import functools
import http.server
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/web'))
import prepare_web_data
import check_publication
import serve


class PublicationTests(unittest.TestCase):
    def test_incremental_cleanup_does_not_follow_asset_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            originals = root / 'originals'
            originals.mkdir()
            (originals / 'FGP.exe').write_bytes(b'private executable')
            out = root / 'build'
            out.mkdir()
            (out / 'data').symlink_to(originals, target_is_directory=True)
            (out / 'vfs').mkdir()
            (out / 'vfs' / 'FGP.exe').write_bytes(b'old bundled executable')
            (out / 'FGPNativePOC.data').write_bytes(b'old preload bundle')
            (out / 'preload.json').write_text('{}')
            prepare_web_data.prepare(out)
            self.assertEqual((originals / 'FGP.exe').read_bytes(), b'private executable')
            self.assertEqual(set(p.name for p in out.iterdir()), set(prepare_web_data.STATIC_FILES))

    def test_release_audit_rejects_assets_links_and_old_preloads(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in check_publication.REQUIRED:
                (root / name).write_text('authored engine content')
            (root / 'FGPNativePOC.html').write_text('<input id="game-folder"><script src="local-files.js"></script>')
            (root / 'FGPNativePOC.wasm').write_bytes(b'\0asm')
            check_publication.check(root)
            (root / 'FGPNativePOC.data').write_bytes(b'original content')
            with self.assertRaises(ValueError):
                check_publication.check(root)
            (root / 'FGPNativePOC.data').unlink()
            (root / 'FGPNativePOC.js').write_text('loadPackage({})')
            with self.assertRaises(ValueError):
                check_publication.check(root)
            (root / 'FGPNativePOC.js').unlink()
            (root / 'FGPNativePOC.js').symlink_to(root / 'local-files.js')
            with self.assertRaises(ValueError):
                check_publication.check(root)

    def test_only_engine_files_are_served_even_with_old_assets_present(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ['FGPNativePOC.html', 'FGPNativePOC.wasm', 'FGPNativePOC.data', 'FGP.exe', 'menu.bin', 'preload.json']:
                (root / name).write_bytes(b'0123456789')
            (root / 'data').mkdir()
            (root / 'data' / 'menu.bin').write_bytes(b'original assets')
            (root / 'local-files.js').symlink_to(root / 'FGP.exe')
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(serve.RangeHandler, directory=str(root)))
            server.throttle = 0
            server.leaderboard = serve.Leaderboard(root / 'scores.json')
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f'http://127.0.0.1:{server.server_address[1]}'
            try:
                with urllib.request.urlopen(base + '/') as response:
                    self.assertEqual(response.status, 200)
                request = urllib.request.Request(base + '/FGPNativePOC.wasm', headers={'Range': 'bytes=0-3'})
                with urllib.request.urlopen(request) as response:
                    self.assertEqual(response.status, 206)
                    self.assertEqual(response.read(), b'0123')
                for path in ['/data/menu.bin', '/FGPNativePOC.data', '/FGP.exe', '/menu.bin', '/preload.json', '/local-files.js', '/%46GP.exe', '/../FGP.exe', '/data/']:
                    for method in ['GET', 'HEAD']:
                        with self.assertRaises(urllib.error.HTTPError) as error:
                            urllib.request.urlopen(urllib.request.Request(base + path, method=method, headers={'Range': 'bytes=0-3'}))
                        self.assertEqual(error.exception.code, 404, (method, path))
                        error.exception.close()
            finally:
                server.shutdown()
                server.server_close()
                thread.join()


if __name__ == '__main__':
    unittest.main()
