# Offline Indonesia maps on a 32 GB SD card

The ESP32 displays raster PNG tiles at **zoom 16 only**, 256 x 256 pixels,
maximum 48 KiB compressed per tile. It cannot render OSM PBF, vector MBTiles,
PMTiles or Garmin maps. Prepare maps on the PC, then copy the resulting
`tiles` directory into the root of a FAT32 card. Keep `/tracks` intact.

A marketed 32 GB card has about 29.8 GiB before formatting. Start with a city
or riding area; reserve space for logs. Nationwide zoom-16 coverage can require
millions of files, including sea tiles in rectangular exports. Do not assume
it fits. Very large flat FAT directories also increase lookup/copy times;
this implementation preserves the existing firmware cache layout.

## 1. Download raw data, not public web tiles

Geofabrik provides OSM extracts for Indonesia and smaller island regions:
https://download.geofabrik.de/asia/indonesia.html

For a smaller first pack use the Java, Sumatra, or other regional download
linked there. Download **free shapefiles** for straightforward local styling
in QGIS; alternatively use the OSM PBF through GDAL's OSM driver. PBF is raw
vector data, not a ready-to-use ESP32 map. Nationwide imports/rendering can
consume substantial PC memory, disk and time.

Example raw country download (run only when nationwide source is needed):

```powershell
New-Item -ItemType Directory -Force offline-work
curl.exe --fail --location --retry 3 --continue-at - --output offline-work/indonesia-latest.osm.pbf https://download.geofabrik.de/asia/indonesia-latest.osm.pbf
```

Do not bulk export QGIS's online OpenStreetMap XYZ layer. OSM's public raster
service prohibits bulk/offline downloading:
https://operations.osmfoundation.org/policies/tiles/

## 2. Render local data in QGIS

1. Load the downloaded local vector layers (roads, buildings, land use, water,
   places) into QGIS. For PBF, use GDAL/OGR import and select its layers;
   converting a large extract to GeoPackage first makes repeated rendering easier.
2. Style roads, water, land and place labels. Use an opaque light background.
   These are your locally rendered maps, not the standard OSM Carto style.
3. Set project CRS to **EPSG:3857**. Source GPS coordinates remain WGS84
   **EPSG:4326**; QGIS/GDAL/PROJ handles reprojection. UTM is unnecessary for
   the device's Web Mercator tile grid.
4. Remove online XYZ/WMS layers so the export uses local data only.
5. Open Processing Toolbox > **Generate XYZ tiles (MBTiles)**.
6. Choose a small area first, minimum and maximum zoom **16**, PNG format,
   opaque background, and output e.g. `offline-work/java-area.mbtiles`.
   Keep raster tiles at 256 x 256; include a margin around intended routes.
7. Inspect the rendered map in QGIS before preparing the SD card. Check road
   visibility, labels, GPS alignment and coverage. Exporting at other zooms
   will not help the current fixed-zoom firmware.

QGIS tool reference:
https://documentation.qgis.org/3.44/en/docs/user_manual/processing_algs/qgis/rastertools.html

## 3. Estimate, convert, copy

```powershell
python -m pip install -r scripts/requirements-offline-map.txt
# Example Jakarta bounds: west south east north, decimal WGS84 degrees.
# Replace with your actual rendered area.
python scripts/prepare-offline-map.py offline-work/java-area.mbtiles --output offline-work/sdcard --bbox 106.70 -6.35 107.00 -6.05
# Add --write after reviewing the size estimate.
python scripts/prepare-offline-map.py offline-work/java-area.mbtiles --output offline-work/sdcard --bbox 106.70 -6.35 107.00 -6.05 --write
```

Copy `offline-work/sdcard/tiles` and `MAP-LICENSE.txt` to the FAT32 card root.
You may instead set `--output` to the confirmed SD drive root. The tool reads
standard MBTiles TMS rows and flips them into XYZ filenames such as
`tiles/16_52197_33900.png`. It flattens transparency, quantizes colors to fit
48 KiB, writes atomically, and retains identical existing files on repeat runs.
Quantization can reduce visual quality; inspect the resulting PNGs.
It rejects vector tiles and non-256-pixel images. Missing tiles in the source
remain missing: available/rectangle counts do not certify land coverage.
Its conservative space budget assumes up to 64 KiB allocation per tile plus
512 MiB reserve; use FAT32 allocation units no larger than 32 KiB.
Do not point multiple exporters at the same destination concurrently.

Keep OSM attribution and any source/style notices with the pack. The device
already displays OSM attribution; locally styled OSM data is under ODbL:
https://www.openstreetmap.org/copyright

## 4. Device operation

SD tiles are preferred automatically, even with no hotspot. For a deployment
that never downloads missing map tiles, set `Config::offline_maps_only = true`
in `include/config.h`, then build/upload normally. This avoids the HTTPS
worker and its queues, but does not disable the separate Wi-Fi setup interface.
Missing coverage stays a grey background with the GPS/track overlay.

Test first with a small pack: power off, insert card, boot without internet,
obtain a GPS fix outdoors, open MAP and confirm tiles/position/track. Move
across tile boundaries, reboot, then check CSV logging still works. PC build
and conversion tests do not prove SD performance or physical device behavior.
