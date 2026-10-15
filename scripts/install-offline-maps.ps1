param(
    [string]$Port = 'COM4',
    [string]$QgisPython = 'C:\Program Files\QGIS 4.2.3\bin\python-qgis.bat',
    [switch]$SkipFlash
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    if (!(Test-Path -LiteralPath $QgisPython)) { throw "QGIS Python not found: $QgisPython" }
    if (!(Test-Path -LiteralPath 'offline-work/regions.gpkg')) { throw 'Prepared regional vector data is missing: offline-work/regions.gpkg' }
    & python -m pip install -r scripts/requirements-offline-map.txt
    if ($LASTEXITCODE -ne 0) { throw 'Python dependency installation failed' }
    if (!$SkipFlash) {
        Write-Host 'Close the serial monitor. Hold BOOT during Connecting if needed.'
        & pio run -e cyd -t upload --upload-port $Port
        if ($LASTEXITCODE -ne 0) { throw 'Firmware flash failed. Hold BOOT during Connecting and run this script again.' }
    }
    $regions = @(
        @{ Name = 'sumatra'; Bounds = @('94.5', '-6.1', '106.5', '6.2') },
        @{ Name = 'java'; Bounds = @('104.8', '-9.0', '114.7', '-5.5') },
        @{ Name = 'bali'; Bounds = @('114.4', '-9.0', '115.8', '-8.0') }
    )
    foreach ($region in $regions) {
        $raster = "offline-work/$($region.Name)-regions-z12.mbtiles"
        $boundsArgs = $region.Bounds
        if (!(Test-Path -LiteralPath $raster)) {
            $building = "$raster.building"
            # Remove only this script's incomplete intermediate database.
            if (Test-Path -LiteralPath $building) { Remove-Item -LiteralPath $building }
            & $QgisPython scripts/render-offline-map.py offline-work/regions.gpkg $building --bbox @boundsArgs --zoom 12
            if ($LASTEXITCODE -ne 0) { throw "Rendering failed: $($region.Name)" }
            Move-Item -LiteralPath $building -Destination $raster
        }
        & python scripts/prepare-offline-map.py $raster --output offline-work/regional-sd --bbox @boundsArgs --zoom 12 --write
        if ($LASTEXITCODE -ne 0) { throw "Tile preparation failed: $($region.Name)" }
    }
    & python scripts/upload-map-serial.py --port $Port --tiles offline-work/regional-sd/tiles --zoom 12
    if ($LASTEXITCODE -ne 0) { throw 'Map transfer failed. Tap RESET before retrying with -SkipFlash.' }
    Write-Host 'Regional tile copy complete; files passed SD readback CRC verification. Verify the MAP display outdoors.'
} finally {
    Pop-Location
}
