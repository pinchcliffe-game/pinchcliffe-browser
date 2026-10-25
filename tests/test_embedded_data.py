"""Synthetic regression checks for the original-content publication scanner."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "embedded_data", ROOT / "tools/web/check_embedded_data.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class EmbeddedDataTest(unittest.TestCase):
    def test_detects_payload_inside_binary(self):
        payload = bytes(range(128))
        self.assertEqual(module.find_embedded(b"header" + payload + b"tail",
                                             [("synthetic", payload)]), ["synthetic"])

    def test_detects_partial_payload(self):
        payload = bytes(range(128))
        self.assertEqual(module.find_embedded(payload[32:96],
                                             [("synthetic", payload)]), ["synthetic"])

    def test_clean_engine_and_zero_fill(self):
        self.assertEqual(module.find_embedded(b"authored engine" + bytes(128),
                         [("data", bytes(range(128))), ("zeros", bytes(128))]), [])

    def test_rejects_non_executable(self):
        with self.assertRaises(ValueError):
            module.Image(b"not a PE file")


if __name__ == "__main__":
    unittest.main()
