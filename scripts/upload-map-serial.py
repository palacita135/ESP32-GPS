"""Upload prepared zoom-16 PNG tiles through the ESP32 serial SD protocol."""
import argparse
from pathlib import Path
import re
import time
import zlib
import serial


def response(port, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().decode('ascii', errors='replace').strip()
        if line.startswith('MAP '):
            if line.startswith('MAP ERROR'):
                raise RuntimeError(line)
            return line
    raise TimeoutError('No MAP response; flash firmware with map_transfer.h support')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM4')
    parser.add_argument('--tiles', type=Path)
    parser.add_argument('--zoom', type=int, choices=range(1, 19), default=12)
    args = parser.parse_args()
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 0.5
    port.dtr = port.rts = False
    with port:
        time.sleep(4)  # Allow boot/splash after USB bridge control-line changes.
        port.reset_input_buffer()
        port.write(b'MAPINFO\n')
        info = response(port)
        print(info, flush=True)
        if 'sd=1' not in info:
            raise RuntimeError('SD card not mounted by firmware')
        if not args.tiles:
            return
        if f'zoom={args.zoom} ' not in info:
            raise RuntimeError('Firmware zoom differs from pack zoom')
        port.write(b'MAPFAST\n')
        if response(port) != 'MAP FAST':
            raise RuntimeError('Could not enable fast serial transfer')
        port.baudrate = 921600
        count = skipped = 0
        for path in sorted(args.tiles.glob(f'{args.zoom}_*_*.png')):
            match = re.fullmatch(rf'{args.zoom}_(\d+)_(\d+)\.png', path.name)
            if not match:
                continue
            data = path.read_bytes()
            if not (33 <= len(data) <= 49152 and data[:8] == b'\x89PNG\r\n\x1a\n'):
                raise ValueError(f'Invalid prepared tile: {path}')
            x, y = map(int, match.groups())
            crc = zlib.crc32(data)
            port.write(f'MAPPUT {x} {y} {len(data)} {crc:08x}\n'.encode())
            reply = response(port)
            if reply == 'MAP REPLACE':
                reply = response(port)
            if reply == 'MAP EXISTS':
                skipped += 1
                continue
            if reply != 'MAP READY':
                raise RuntimeError(f'Unexpected response: {reply}')
            for start in range(0, len(data), 1024):
                block = data[start:start + 1024]
                port.write(block)
                done = start + len(block)
                reply = response(port)
                expected = (f'MAP DONE {len(data)} {crc:08x}' if done == len(data)
                            else f'MAP ACK {done}')
                if reply != expected:
                    raise RuntimeError(f'Unexpected response: {reply}')
            count += 1
            if count == 1 or count % 100 == 0:
                print(f'{count} copied, {skipped} existing: {path.name} (CRC readback OK)',
                      flush=True)
        port.write(b'MAPSLOW\n')
        if response(port) != 'MAP SLOW':
            raise RuntimeError('Could not restore monitor baud')
        port.baudrate = 115200
        print(f'Finished: {count} copied, {skipped} existing. Existing files passed size/CRC verification.')


if __name__ == '__main__':
    main()
