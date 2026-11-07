import argparse
import csv
import hashlib
import json
import math
import statistics as stats
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--phase', choices=['water', 'ao'], required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('inputs', nargs='+', type=Path)
args = parser.parse_args()
combinations = 4 if args.phase == 'water' else 8

def summary(values):
    values = sorted(values)
    assert values and all(math.isfinite(v) for v in values)
    return dict(n=len(values), median=stats.median(values), mean=stats.mean(values),
                p95=values[math.ceil(.95 * len(values)) - 1],
                stdev=stats.stdev(values) if len(values) > 1 else 0,
                min=values[0], max=values[-1])

series_results = []
by_mask = {i: [] for i in range(combinations)}
baseline_drift = []
inputs = []
for run, path in enumerate(args.inputs):
    rows = list(csv.DictReader(path.open(encoding='utf-8-sig', newline='')))
    grouped = {}
    for row in rows:
        mask = int(row['row'])
        assert 0 <= mask < combinations
        assert mask == int(row['water_async']) + 2 * int(row['particles_async']) + 4 * int(row['ao_async'])
        assert row['drain'] == 'cpu-drain'
        assert abs(float(row['dt']) - 1 / 60) < 1e-12
        assert int(row['water_instances']) == 1 and int(row['water_particles']) == 51975
        assert int(row['particle_capacity']) == 250000
        assert float(row['gpu_ms']) > 0
        grouped.setdefault(int(row['series']), []).append(row)
        by_mask[mask].append(row)
    assert sorted(grouped) == list(range(2 * combinations)), 'Missing matrix series'
    run_results = []
    sample_counts = set()
    for series, selected in sorted(grouped.items()):
        expected = series if series < combinations else 2 * combinations - 1 - series
        assert all(int(r['row']) == expected for r in selected), 'Unexpected row order'
        assert all(r['order'] == ('forward' if series < combinations else 'reverse') for r in selected)
        assert [int(r['sample']) for r in selected] == list(range(len(selected))), 'Missing/duplicate samples'
        sample_counts.add(len(selected))
        result = dict(run=run, series=series, row=expected, order=selected[0]['order'],
                      gpu=summary([float(r['gpu_ms']) for r in selected]),
                      cpu_submit=summary([float(r['cpu_submit_ms']) for r in selected]))
        series_results.append(result)
        run_results.append(result)
    assert len(sample_counts) == 1 and next(iter(sample_counts)) > 0
    before, after = run_results[0]['gpu']['median'], run_results[-1]['gpu']['median']
    baseline_drift.append(dict(run=run, first_ms=before, last_ms=after, change_percent=100 * (after / before - 1)))
    inputs.append(dict(path=str(path.resolve()), sha256=hashlib.sha256(path.read_bytes()).hexdigest(), samples=len(rows)))

matrix = []
for mask, rows in by_mask.items():
    series_medians = [s['gpu']['median'] for s in series_results if s['row'] == mask]
    assert len(series_medians) == 2 * len(args.inputs)
    compute = [float(r['compute_sum_ms']) for r in rows if float(r['compute_sum_ms']) >= 0]
    matrix.append(dict(row=mask, water_async=bool(mask & 1), particles_async=bool(mask & 2), ao_async=bool(mask & 4),
                       gpu_series_medians=summary(series_medians),
                       gpu_samples=summary([float(r['gpu_ms']) for r in rows]),
                       cpu_submit=summary([float(r['cpu_submit_ms']) for r in rows]),
                       compute_sum=summary(compute) if compute else None))
baseline = matrix[0]['gpu_series_medians']['median']
for row in matrix:
    elapsed = row['gpu_series_medians']['median']
    row['vs_direct_ms'] = elapsed - baseline
    row['improvement_percent'] = 100 * (baseline - elapsed) / baseline
marginal = []
for bit, label in [(1, 'water'), (2, 'particles'), (4, 'ao')]:
    if bit >= combinations:
        continue
    for mask in range(combinations):
        if mask & bit:
            continue
        before = matrix[mask]['gpu_series_medians']['median']
        after = matrix[mask | bit]['gpu_series_medians']['median']
        pairs = []
        for run in range(len(args.inputs)):
            for order in ('forward', 'reverse'):
                selected = {s['row']: s['gpu']['median'] for s in series_results if s['run'] == run and s['order'] == order}
                pairs.append(selected[mask] - selected[mask | bit])
        marginal.append(dict(effect=label, from_row=mask, to_row=mask | bit,
                             saved_ms=before-after, improvement_percent=100 * (before-after) / before,
                             paired_saved_ms=summary(pairs), faster_pairs=sum(v > 0 for v in pairs)))
result = dict(phase=args.phase, primary_metric='median of per-series GPU medians; CPU-drain full frame',
              inputs=inputs, matrix=matrix, marginal=marginal, baseline_drift=baseline_drift, series=series_results)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes((json.dumps(result, indent=2) + '\n').encode())
print(json.dumps(dict(matrix=[dict(row=r['row'], gpu_ms=r['gpu_series_medians']['median'],
                                  p95=r['gpu_samples']['p95'], improvement_percent=r['improvement_percent']) for r in matrix],
                      baseline_drift=baseline_drift), indent=2))
