"""Export local GPU evidence and PNGs without third-party Python packages."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import zlib


def png(source, destination):
    magic, size, maximum, pixels = source.read_bytes().split(b"\n", 3)
    width, height = map(int, size.split())
    channels = 3 if magic == b"P6" else 1
    assert magic in (b"P5", b"P6") and maximum == b"255"
    rows = b"".join(b"\0" + pixels[y * width * channels:(y + 1) * width * channels]
                    for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    destination.write_bytes(b"\x89PNG\r\n\x1a\n"
                            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8,
                                                         2 if channels == 3 else 0, 0, 0, 0))
                            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def trace_summary(root, frame):
    names = {}
    for line in (root / f"geometry-{frame}-operations.txt").read_text().splitlines():
        number, name = line.split(" ", 1)
        names[int(number)] = name
    packets = {}
    for line in (root / f"geometry-{frame}-schedule.txt").read_text().splitlines():
        match = re.match(r"packet (\d+) DIRECT op=(\d+) lists=(\d+)", line)
        if match:
            packet, operation, lists = map(int, match.groups())
            packets[packet] = (operation, lists)
    events = json.loads((root / f"geometry-{frame}-cpu.json").read_text())["traceEvents"]
    spans = []
    for event in events:
        match = re.fullmatch(r"list (\d+):(\d+)", event["name"])
        if not match:
            continue
        packet, child = map(int, match.groups())
        operation, lists = packets[packet]
        if 1 <= child <= lists and operation in names:
            spans.append(dict(name=names[operation], packet=packet, bucket=child - 1,
                              worker=event["tid"], start_us=event["ts"], duration_us=event["dur"]))
    edges = []
    for span in spans:
        if span["duration_us"]:
            edges += [(span["start_us"], 1, span["worker"]),
                      (span["start_us"] + span["duration_us"], -1, span["worker"])]
    active, peak = {}, 0
    for _, direction, worker in sorted(edges):
        active[worker] = active.get(worker, 0) + direction
        peak = max(peak, sum(value > 0 for value in active.values()))
    assert peak >= 2, "No overlapping native technique recording callbacks"
    return dict(frame=frame, peak_simultaneous_workers=peak,
                workers=sorted({span["worker"] for span in spans}), native_callback_count=len(spans),
                callback_total_us=sum(span["duration_us"] for span in spans),
                spans=spans, scope="Native recording callback including setup and Close; excludes owner freezing; CPU overlap only")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--target", default="App.MetaRenderGraph")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    source = repo / "Build/bin/x64" / args.configuration / args.target / "Logs"
    evidence = repo / "Build/Verification/meta-render-graph-u4" / f"{args.configuration}-{args.target}" / "geometry"
    evidence.mkdir(parents=True, exist_ok=True)
    files = list(source.glob("geometry-*")) + list(source.glob("queued-slot-*"))
    for file in files:
        shutil.copy2(file, evidence / file.name)
    for frame in (0, 1, 4, 7):
        for pattern in (f"geometry-{frame}-parallel-albedo.ppm",
                        f"geometry-{frame}-parallel-shadow-*.pgm",
                        f"geometry-{frame}-parallel-probe-4.ppm"):
            for file in evidence.glob(pattern):
                png(file, file.with_suffix(".png"))
    summaries = [trace_summary(evidence, frame) for frame in range(8)]
    (evidence.parent / "geometry-trace-summary.json").write_text(json.dumps(summaries, indent=2))
    metrics = {}
    for file in evidence.glob("*.txt"):
        if "-schedule" in file.name or "-operations" in file.name:
            continue
        values = re.findall(r"([a-z_]+)=(\d+)", file.read_text())
        if values:
            metrics[file.stem] = {key: int(value) for key, value in values}
    (evidence.parent / "geometry-metrics.json").write_text(json.dumps(metrics, indent=2, sort_keys=True))
    print(f"Exported {len(files)} raw evidence files; native callback overlap verified in all eight frames: {evidence}")


if __name__ == "__main__":
    main()
