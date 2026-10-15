"""Render a local road/place/water GeoPackage into raster MBTiles using QGIS."""
import argparse
import math
import os
from pathlib import Path
import sqlite3
import sys

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from qgis.core import (
    Qgis, QgsApplication, QgsVectorLayer, QgsMapSettings, QgsMapRendererSequentialJob,
    QgsCoordinateReferenceSystem, QgsRectangle, QgsLineSymbol, QgsFillSymbol,
    QgsMarkerSymbol, QgsPalLayerSettings, QgsTextFormat, QgsTextBufferSettings,
    QgsVectorLayerSimpleLabeling, QgsSingleSymbolRenderer,
)
from qgis.PyQt.QtCore import QSize, QBuffer, QIODevice
from qgis.PyQt.QtGui import QColor, QFont, QFontDatabase

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--bbox', nargs=4, type=float, required=True)
parser.add_argument('--zoom', type=int, choices=range(1, 17), default=12)
args = parser.parse_args()
app = QgsApplication([], False)
app.initQgis()
QFontDatabase.addApplicationFont('C:/Windows/Fonts/arial.ttf')

layers = []
for name in ('places', 'roads', 'water'):
    layer = QgsVectorLayer(f'{args.source.resolve()}|layername={name}|option:IMMUTABLE=YES', name, 'ogr')
    if not layer.isValid():
        raise SystemExit(f'Invalid layer: {name}')
    if name == 'roads':
        layer.setSubsetString("highway IN ('motorway','motorway_link','trunk','trunk_link','primary','primary_link','secondary','secondary_link','tertiary','tertiary_link')" if args.zoom <= 12 else '')
        symbol = QgsLineSymbol.createSimple({'line_color': '#c17a42', 'line_width': '0.35'})
    elif name == 'water':
        symbol = QgsFillSymbol.createSimple({'color': '#a9d6e8', 'outline_style': 'no'})
    else:
        layer.setSubsetString("place IN ('city','town')" if args.zoom <= 12 else '')
        symbol = QgsMarkerSymbol.createSimple({'color': '#333333', 'size': '0.8', 'outline_style': 'no'})
        label = QgsPalLayerSettings()
        label.fieldName = 'name'
        text = QgsTextFormat()
        text.setFont(QFont('Arial'))
        text.setSize(9)
        text.setColor(QColor('#222222'))
        halo = QgsTextBufferSettings()
        halo.setEnabled(True)
        halo.setSize(0.7)
        halo.setColor(QColor('white'))
        text.setBuffer(halo)
        label.setFormat(text)
        layer.setLabeling(QgsVectorLayerSimpleLabeling(label))
        layer.setLabelsEnabled(True)
    layer.setRenderer(QgsSingleSymbolRenderer(symbol))
    layers.append(layer)

west, south, east, north = args.bbox
if not (-180 <= west < east <= 180 and -85 <= south < north <= 85):
    raise SystemExit('Invalid bounds')
n = 1 << args.zoom

def tile(lon, lat):
    return (min(n - 1, int((lon + 180) / 360 * n)),
            int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n))

xmin, ymin = tile(west, north)
xmax, ymax = tile(east, south)
if args.output.exists():
    raise SystemExit('Output exists; choose a new MBTiles path')
db = sqlite3.connect(args.output)
db.execute('CREATE TABLE metadata (name TEXT, value TEXT)')
db.execute('CREATE TABLE tiles (zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER, tile_data BLOB, PRIMARY KEY (zoom_level,tile_column,tile_row))')
db.executemany('INSERT INTO metadata VALUES (?,?)', [
    ('name', args.source.stem), ('format', 'png'), ('type', 'baselayer'),
    ('minzoom', str(args.zoom)), ('maxzoom', str(args.zoom)),
    ('bounds', ','.join(map(str, args.bbox))),
    ('attribution', 'OpenStreetMap contributors; locally rendered road overview'),
])
settings = QgsMapSettings()
settings.setLayers(layers)
settings.setTextRenderFormat(Qgis.TextRenderFormat.AlwaysText)
settings.setDestinationCrs(QgsCoordinateReferenceSystem('EPSG:3857'))
settings.setBackgroundColor(QColor('#f5f2e9'))
settings.setOutputSize(QSize(1024, 1024))
settings.setOutputDpi(96)
world = 40075016.68557849
span = world / n
count = 0
total = (xmax - xmin + 1) * (ymax - ymin + 1)
print(f'Rendering {total} tiles at zoom {args.zoom}', flush=True)
for y in range(ymin, ymax + 1, 4):
    for x in range(xmin, xmax + 1, 4):
        left = -world / 2 + x * span
        top = world / 2 - y * span
        settings.setExtent(QgsRectangle(left, top - 4 * span, left + 4 * span, top))
        job = QgsMapRendererSequentialJob(settings)
        job.start()
        job.waitForFinished()
        if job.errors():
            raise RuntimeError(str(job.errors()))
        image = job.renderedImage()
        for dy in range(min(4, ymax - y + 1)):
            for dx in range(min(4, xmax - x + 1)):
                output = QBuffer()
                output.open(QIODevice.OpenModeFlag.WriteOnly)
                if not image.copy(dx * 256, dy * 256, 256, 256).save(output, 'PNG'):
                    raise RuntimeError('PNG encode failed')
                db.execute('INSERT INTO tiles VALUES (?,?,?,?)',
                           (args.zoom, x + dx, n - 1 - y - dy, bytes(output.data())))
                count += 1
        db.commit()
    print(f'{count}/{total} rendered', flush=True)
db.close()
print(f'Completed {args.output}', flush=True)
# Native Qt/QGIS provider teardown can crash on Windows at interpreter shutdown.
# All database writes are committed and closed before process exit.
sys.stdout.flush()
os._exit(0)
