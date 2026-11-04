#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarizes KYTY_FRAME_CSV captures: mean, p50, p95, p99 and max per column, per generator, side by side.

Usage: python3 -I tools/local/frame-csv-summary.py [--skip SECONDS] FILE.csv [FILE.csv ...]
--skip leaves out the start of each capture (default 5 s: the context being made and first-use stalls). Compare captures
of the same spot in the game: a single run says little (docs: +-3-5% across boots).
"""
import csv
import sys


def percentile(values, fraction):
    return values[min(len(values) - 1, int(len(values) * fraction))]


def summarize(path, skip_ms):
    columns = {"game_frame_ms": [], "render_busy_ms": [], "generate_cpu_ms": [], "generate_gpu_ms": []}
    generators, frames, generated = {}, 0, 0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if float(row["time_ms"]) < skip_ms:
                continue
            frames += 1
            generated += int(row["generated"] or 0)
            generators[row["generator"]] = generators.get(row["generator"], 0) + 1
            for name, values in columns.items():
                # (0 for game_frame_ms is the first line of a capture, and a long gap is a load or pause, not a frame.)
                if row[name] and (name != "game_frame_ms" or 0 < float(row[name]) < 500):
                    values.append(float(row[name]))
    print("%s: %d game frames, %d generated; generators: %s" % (path, frames, generated, generators or "none"))
    print("  %-17s %8s %8s %8s %8s %8s" % ("", "mean", "p50", "p95", "p99", "max"))
    for name, values in columns.items():
        if not values:
            print("  %-17s (no values)" % name)
            continue
        values.sort()
        print("  %-17s %8.3f %8.3f %8.3f %8.3f %8.3f" % (name, sum(values) / len(values), percentile(values, 0.5), percentile(values, 0.95),
                                                         percentile(values, 0.99), values[-1]))


def main():
    args = sys.argv[1:]
    skip = 5.0
    if args[:1] == ["--skip"]:
        skip = float(args[1])
        args = args[2:]
    if not args:
        raise SystemExit(__doc__)
    for path in args:
        summarize(path, skip * 1000)


if __name__ == "__main__":
    main()
