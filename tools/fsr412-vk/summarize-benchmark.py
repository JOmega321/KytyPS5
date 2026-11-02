#!/usr/bin/env python3
"""Summarize standalone BENCH rows; these are not gameplay frame times."""
import argparse
import json
import math
import statistics
from pathlib import Path


def summarize(text, warmup=30):
    rows = []
    for line in text.splitlines():
        if not line.startswith('BENCH,'):
            continue
        fields = line.split(',')
        if len(fields) != 5:
            raise ValueError('Malformed BENCH row')
        frame = int(fields[1])
        values = [float(x) for x in fields[2:]]
        if any(not math.isfinite(x) or x < 0 for x in values):
            raise ValueError('Invalid timing sample')
        if frame >= warmup:
            rows.append(values)
    if not rows:
        raise ValueError('No samples after warmup')
    result = {'samples': len(rows), 'warmup_frames': warmup,
              'scope': 'Standalone synthetic replay; not gameplay or FPS'}
    for column, name in enumerate(('cpu_record_ms', 'gpu_replay_ms', 'submit_wait_ms')):
        values = sorted(row[column] for row in rows)
        def percentile(p):
            # Nearest-rank percentile, including the tail rather than interpolating it away.
            return values[max(0, math.ceil(p * len(values)) - 1)]
        result[name] = {'mean': statistics.mean(values), 'p50': percentile(.50),
                        'p95': percentile(.95), 'p99': percentile(.99), 'max': values[-1]}
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--warmup', type=int, default=30)
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error('warmup must be nonnegative')
    print(json.dumps(summarize(args.log.read_text(), args.warmup), indent=2))
