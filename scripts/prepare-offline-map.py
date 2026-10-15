"""Convert locally rendered raster MBTiles to the ESP32 zoom-16 SD cache."""
import argparse
from contextlib import closing
import io
import math
from pathlib import Path
import shutil
import sqlite3

from PIL import Image

ZOOM = 16


def tile_range(bounds, zoom=ZOOM):
    """Return inclusive XYZ limits from WGS84 west,south,east,north."""
    n = 1 << zoom
    west, south, east, north = bounds
    if not (-180 <= west < east <= 180 and -85 <= south < north <= 85):
        raise ValueError('Invalid WGS84 bounds (dateline crossing unsupported)')

    def xy(lon, lat):
        return (int((lon + 180) / 360 * n),
                int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi)
                    / 2 * n))

    xmin, ymin = xy(west, north)
    xmax, ymax = xy(east, south)
    return xmin, ymin, min(xmax, n - 1), ymax


def normalize(data):
    """Flatten transparency, quantize, enforce decoder dimensions/48-KiB cap."""
    with Image.open(io.BytesIO(data)) as image:
        if image.size != (256, 256):
            raise ValueError('Tile must be 256x256 pixels')
        rgba = image.convert('RGBA')
        rgb = Image.new('RGB', image.size, 'white')
        rgb.paste(rgba, mask=rgba.getchannel('A'))
        for colors in (256, 128, 64, 32, 16):
            output = io.BytesIO()
            rgb.quantize(colors=colors).save(output, format='PNG', optimize=True)
            if len(output.getvalue()) <= 49152:
                return output.getvalue()
    raise ValueError('Cannot fit tile in ESP32 decoder buffer')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='Raster MBTiles, TMS tile rows')
    parser.add_argument('--output', required=True, type=Path,
                        help='Staging directory or mounted SD root')
    parser.add_argument('--bbox', nargs=4, type=float, required=True,
                        metavar=('WEST', 'SOUTH', 'EAST', 'NORTH'))
    parser.add_argument('--zoom', type=int, choices=range(1, 19), default=16)
    parser.add_argument('--write', action='store_true',
                        help='Write tiles; default only estimates')
    args = parser.parse_args()
    xmin, ymin, xmax, ymax = tile_range(args.bbox, args.zoom)
    # Standard MBTiles stores TMS rows, opposite to XYZ north-to-south rows.
    with closing(sqlite3.connect(
            args.source.resolve().as_uri() + '?mode=ro', uri=True)) as db:
        query = ('FROM tiles WHERE zoom_level=? AND tile_column BETWEEN ? AND ? '
                 'AND tile_row BETWEEN ? AND ?')
        n = 1 << args.zoom
        params = (args.zoom, xmin, xmax, n - 1 - ymax, n - 1 - ymin)
        count = db.execute('SELECT COUNT(*) ' + query, params).fetchone()[0]
        expected = (xmax - xmin + 1) * (ymax - ymin + 1)
        print(f'Available: {count:,}; bounding rectangle: {expected:,} tiles')
        # Conservative 32-KiB FAT clusters: a 48-KiB file uses up to 64 KiB.
        required = count * 65536 + 512 * 1024 * 1024
        print(f'Budget including 512 MiB reserve: {required / 2**30:.2f} GiB')
        if not count:
            raise ValueError('No zoom-16 raster tiles in selected bounds')
        if not args.write:
            return
        args.output.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(args.output).free < required:
            raise ValueError('Insufficient free space for conservative SD budget')
        tiles = args.output / 'maps'
        tiles.mkdir(exist_ok=True)
        written = 0
        for x, row, data in db.execute(
                'SELECT tile_column,tile_row,tile_data ' + query, params):
            path = tiles / str(args.zoom) / str(x) / f'{n - 1 - row}.png'
            path.parent.mkdir(parents=True, exist_ok=True)
            payload = normalize(data)
            if path.exists() and path.read_bytes() == payload:
                continue
            temporary = path.with_suffix('.png.part')
            temporary.write_bytes(payload)
            temporary.replace(path)
            written += 1
        print(f'Wrote {written:,} tiles to {tiles}; identical tiles retained')
        (args.output / 'MAP-LICENSE.txt').write_text(
            'Map data: OpenStreetMap contributors, ODbL 1.0.\n'
            'https://www.openstreetmap.org/copyright\n'
            'Retain any additional source/style attribution with this card.\n',
            encoding='utf-8')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, sqlite3.Error) as error:
        raise SystemExit(str(error)) from error
