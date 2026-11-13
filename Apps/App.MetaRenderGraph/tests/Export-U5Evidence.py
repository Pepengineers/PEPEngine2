"""Export U5 readbacks and previews without third-party Python dependencies."""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path
import shutil
import struct
import zlib


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def cube_preview(path, data, size, mips):
    face_bytes = sum(max(1, size >> mip) ** 2 * 8 for mip in range(mips))
    assert len(data) == face_bytes * 6
    pixels = bytearray()
    for y in range(size):
        for face in range(6):
            for x in range(size):
                rgb = struct.unpack_from('<4e', data, face * face_bytes + (y * size + x) * 8)[:3]
                for value in rgb:
                    assert math.isfinite(value), 'Nonfinite environment radiance'
                    value = max(0, value)
                    pixels.append(round((value / (1 + value)) ** (1 / 2.2) * 255))
    png(path, size * 6, size, pixels)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--configuration', choices=('Debug', 'Release'), default='Debug')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    source = repo / 'Build/bin/x64' / args.configuration / 'App.MetaRenderGraph/Logs'
    output = repo / 'Build/Verification/meta-render-graph-u5' / (args.configuration + '-App.MetaRenderGraph')
    output.mkdir(parents=True, exist_ok=True)
    for file in source.glob('environment-*'):
        shutil.copy2(file, output / file.name)
    metrics = []
    for scenario in range(5):
        files = [output / f'environment-{scenario}-{mode}.bin' for mode in ('serial', 'parallel', 'baseline')]
        values = [file.read_bytes() for file in files]
        assert values[0] == values[1] == values[2], 'Environment output mismatch'
        metrics.append(dict(scenario=scenario, bytes=len(values[0]), max_error=0,
                            sha256=hashlib.sha256(values[0]).hexdigest()))
        if scenario:
            # Source fixture selects 16px / 5 mips; diffuse32, specular128/8mips,
            # BRDF128 precede the source cube in the tightly packed readback.
            start = 32 * 32 * 6 * 8 + sum((128 >> mip) ** 2 * 6 * 8 for mip in range(8)) + 128 * 128 * 8
            cube_preview(output / f'environment-{scenario}-source.png', values[0][start:], 16, 5)
    geometry = output / 'geometry'
    geometry.mkdir(exist_ok=True)
    for pattern in ('geometry-*', 'queued-slot-*'):
        for file in source.glob(pattern):
            shutil.copy2(file, geometry / file.name)
    lighting = []
    for file in sorted(geometry.glob('geometry-*-parallel-lighting.bin')):
        serial = file.with_name(file.name.replace('-parallel-', '-serial-'))
        data = file.read_bytes()
        assert data == serial.read_bytes(), 'Native lighting/probe serial/parallel mismatch'
        text = file.with_suffix('.txt').read_text()
        match = re.search(r'width=(\d+) height=(\d+)', text)
        width, height = map(int, match.groups())
        assert len(data) >= width * height * 28
        lighting.append(dict(file=file.name, bytes=len(data), max_error=0,
                             baseline_status='pending explicit original-pass comparison'))
        # Raw AO/HDR/SSR/composition planes precede the probe metadata/mips.
        # Previews are tone mapped; comparisons above use the complete raw data.
        step = max(1, (width + 639) // 640)
        preview_width, preview_height = (width + step - 1) // step, (height + step - 1) // step
        for image, name in enumerate(('deferred', 'ssr', 'composition')):
            offset = width * height * (4 + image * 8)
            pixels = bytearray()
            for y in range(0, height, step):
                for x in range(0, width, step):
                    rgb = struct.unpack_from('<4e', data, offset + (y * width + x) * 8)[:3]
                    for value in rgb:
                        assert math.isfinite(value), 'Nonfinite native HDR radiance'
                        value = max(0, value)
                        pixels.append(round((value / (1 + value)) ** (1 / 2.2) * 255))
            png(file.with_name(file.stem + '-' + name + '.png'), preview_width, preview_height, pixels)
    (output / 'lighting-summary.json').write_text(json.dumps(lighting, indent=2) + '\n')
    (output / 'environment-summary.json').write_text(json.dumps(metrics, indent=2) + '\n')
    print(f'PASS exported {args.configuration}: 5 exact serial/parallel/original comparisons, 4 source previews')


if __name__ == '__main__':
    main()
