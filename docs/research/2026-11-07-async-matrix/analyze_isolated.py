"""Compare identical fixed-frame workloads, one fresh process per mask."""
import argparse
import csv
import hashlib
import json
import math
import re
import statistics as stats
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()

def summary(values):
    values = sorted(values)
    assert values and all(math.isfinite(v) for v in values)
    return dict(n=len(values), median=stats.median(values), min=values[0], max=values[-1],
                p95=values[math.ceil(.95 * len(values)) - 1])

trials, inputs, samples = {}, [], {mask: [] for mask in range(8)}
for path in sorted(args.directory.glob('batch-??-row-?.csv')):
    batch, mask = map(int, re.fullmatch(r'batch-(\d+)-row-(\d+)', path.stem).groups())
    assert 1 <= batch <= 3 and 0 <= mask < 8 and (batch, mask) not in trials
    rows = list(csv.DictReader(path.open(encoding='utf-8-sig', newline='')))
    assert len(rows) == 128 and [int(r['sample']) for r in rows] == list(range(128))
    for row in rows:
        assert int(row['series']) == 0 and row['order'] == 'single'
        assert int(row['row']) == mask
        assert int(row['water_async']) == bool(mask & 1)
        assert int(row['particles_async']) == bool(mask & 2)
        assert int(row['ao_async']) == bool(mask & 4)
        assert row['drain'] == 'cpu-drain' and abs(float(row['dt']) - 1 / 60) < 1e-12
        assert (int(row['water_instances']), int(row['water_particles']), int(row['particle_capacity'])) == (1, 51975, 250000)
        assert float(row['gpu_ms']) > 0
    trials[batch, mask] = dict(batch=batch, row=mask,
        gpu=summary([float(r['gpu_ms']) for r in rows]),
        cpu_submit=summary([float(r['cpu_submit_ms']) for r in rows]))
    samples[mask].extend(rows)
    inputs.append(dict(path=str(path.resolve()), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
assert set(trials) == {(batch, mask) for batch in range(1, 4) for mask in range(8)}

matrix = []
baseline = stats.median(trials[b, 0]['gpu']['median'] for b in range(1, 4))
for mask in range(8):
    medians = [trials[b, mask]['gpu']['median'] for b in range(1, 4)]
    paired = [100 * (1 - trials[b, mask]['gpu']['median'] / trials[b, 0]['gpu']['median']) for b in range(1, 4)]
    gpu = stats.median(medians)
    matrix.append(dict(row=mask, gpu_ms=gpu, improvement_percent=100 * (1 - gpu / baseline),
        trial_medians=medians, trial_medians_summary=summary(medians),
        paired_improvement_percent=paired, faster_than_direct_trials=sum(v > 0 for v in paired),
        gpu_samples=summary([float(r['gpu_ms']) for r in samples[mask]]),
        cpu_submit=summary([float(r['cpu_submit_ms']) for r in samples[mask]])))
marginal = []
for bit, label in [(1, 'water'), (2, 'particles'), (4, 'ao')]:
    for mask in range(8):
        if mask & bit:
            continue
        paired = [trials[b, mask]['gpu']['median'] - trials[b, mask | bit]['gpu']['median'] for b in range(1, 4)]
        before, after = matrix[mask]['gpu_ms'], matrix[mask | bit]['gpu_ms']
        marginal.append(dict(effect=label, from_row=mask, to_row=mask | bit,
            saved_ms=before - after, improvement_percent=100 * (1 - after / before),
            paired_saved_ms=paired, faster_trials=sum(v > 0 for v in paired)))
result = dict(primary_metric='median of three fresh-process GPU medians, 64 warmup + 128 measured frames, CPU drain',
    orders=[[0,7,1,6,2,5,3,4], [4,3,5,2,6,1,7,0], [3,6,1,4,0,7,2,5]],
    inputs=inputs, matrix=matrix, marginal=marginal, trials=list(trials.values()))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes((json.dumps(result, indent=2) + '\n').encode())
print(json.dumps(dict(matrix=[{k: r[k] for k in ('row','gpu_ms','improvement_percent','trial_medians','faster_than_direct_trials')} for r in matrix],
    ao_marginal=[r for r in marginal if r['effect'] == 'ao']), indent=2))
