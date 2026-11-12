#!/usr/bin/env python3
"""Summarize --profile CSV files, excluding rows marked as warmup."""
import argparse
import csv
import math
import statistics
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('files', type=Path, nargs='+')
args = parser.parse_args()
for path in args.files:
    with path.open(newline='') as stream:
        rows = [row for row in csv.DictReader(stream) if row['warmup'] == '0']
    if not rows:
        parser.error(f'{path}: no measured frames after warmup')
    keys = [key for key in rows[0] if key.endswith('_ms')]
    print(f'\n{path}: {len(rows)} measured frames; {1000 / statistics.mean(float(r["total_ms"]) for r in rows):.2f} FPS')
    print('stage             mean ms  median ms     p95 ms')
    for key in keys:
        values = sorted(float(row[key]) for row in rows)
        print(f'{key:18s} {statistics.mean(values):8.3f} {statistics.median(values):10.3f} {values[math.ceil(len(values)*.95)-1]:10.3f}')
    totals = sorted(float(row['total_ms']) for row in rows)
    print(f'total: p99={totals[math.ceil(len(totals)*.99)-1]:.3f} ms; '
          f'max={totals[-1]:.3f} ms; >25 ms={sum(v > 25 for v in totals)}/{len(totals)}')
