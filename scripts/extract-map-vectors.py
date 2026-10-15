"""Extract local OSM road/place/water layers with QGIS's GDAL Python.

Run using QGIS/bin/python-qgis.bat. Input is Geofabrik OSM PBF;
output is a spatially indexed GeoPackage, never online raster tiles.
"""
import argparse
from pathlib import Path
from osgeo import gdal

gdal.UseExceptions()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--bbox', nargs=4, type=float, required=True)
args = parser.parse_args()
if args.output.exists():
    raise SystemExit('Output exists; choose a new GeoPackage path')

queries = [
    ('roads', "SELECT name, highway FROM lines WHERE highway IS NOT NULL"),
    ('places', "SELECT name, place FROM points WHERE place IN ('city','town','village') AND name IS NOT NULL"),
    ('water', "SELECT name FROM multipolygons WHERE natural = 'water' OR landuse = 'reservoir'"),
]
for index, (name, query) in enumerate(queries):
    print(f'Extracting {name} from {args.source.name}', flush=True)
    result = gdal.VectorTranslate(
        str(args.output), str(args.source), format='GPKG',
        accessMode=None if index == 0 else 'update',
        SQLStatement=query, SQLDialect='OGRSQL', layerName=name,
        spatFilter=args.bbox, dstSRS='EPSG:3857',
        layerCreationOptions=['SPATIAL_INDEX=YES'],
        skipFailures=False,
    )
    if result is None:
        raise RuntimeError(f'GDAL failed to extract {name}')
    result = None
print(f'Extracted {args.output}', flush=True)
