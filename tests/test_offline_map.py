"""Host checks for MBTiles orientation, format limits and repeat exports."""
import importlib.util
import io
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from PIL import Image

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/prepare-offline-map.py'
spec = importlib.util.spec_from_file_location('offline', SCRIPT)
offline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(offline)


class OfflineTests(unittest.TestCase):
    def test_conversion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'test.mbtiles'
            image = io.BytesIO()
            Image.new('RGBA', (256, 256), (0, 0, 255, 0)).save(image, 'PNG')
            x, y, _, _ = offline.tile_range((106.8, -6.2, 106.801, -6.199))
            with sqlite3.connect(source) as db:
                db.execute('CREATE TABLE tiles (zoom_level, tile_column, tile_row, tile_data)')
                db.execute('INSERT INTO tiles VALUES (?, ?, ?, ?)',
                           (16, x, 65535 - y, image.getvalue()))
            db.close()
            command = [sys.executable, str(SCRIPT), str(source), '--output',
                       str(root / 'sd'), '--bbox', '106.8', '-6.2', '106.801', '-6.199']
            subprocess.run(command, check=True, capture_output=True)
            self.assertFalse((root / 'sd').exists())
            subprocess.run(command + ['--write'], check=True, capture_output=True)
            tile = root / 'sd' / 'tiles' / f'16_{x}_{y}.png'
            self.assertLessEqual(tile.stat().st_size, 49152)
            with Image.open(tile) as result:
                self.assertEqual(result.convert('RGB').getpixel((0, 0)), (255, 255, 255))
            timestamp = tile.stat().st_mtime_ns
            subprocess.run(command + ['--write'], check=True, capture_output=True)
            self.assertEqual(tile.stat().st_mtime_ns, timestamp)

    def test_rejections(self):
        with self.assertRaises(ValueError):
            offline.tile_range((110, -6, 100, -5))
        with self.assertRaises(OSError):
            offline.normalize(b'vector tile')
        image = io.BytesIO()
        Image.new('RGB', (512, 512)).save(image, 'PNG')
        with self.assertRaises(ValueError):
            offline.normalize(image.getvalue())


if __name__ == '__main__':
    unittest.main()
