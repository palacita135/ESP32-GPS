"""Merge extracted region layers before rendering overlapping tile borders."""
import argparse
from pathlib import Path
from osgeo import gdal

gdal.UseExceptions()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('sources', nargs='+', type=Path)
args = parser.parse_args()
if args.output.exists():
    raise SystemExit('Output exists')
first = True
for name in ('roads', 'places', 'water'):
    for index, source in enumerate(args.sources):
        print(f'Merging {source.name}:{name}', flush=True)
        result = gdal.VectorTranslate(
            str(args.output), str(source), format='GPKG', layers=[name],
            layerName=name, accessMode=None if first else ('update' if index == 0 else 'append'),
            options=['-unsetFid'], layerCreationOptions=['SPATIAL_INDEX=YES'] if index == 0 else [],
        )
        if result is None:
            raise RuntimeError('Merge failed')
        result = None
        first = False
print('Merged local data', flush=True)

