#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('summary', Path(__file__).resolve().parents[1] / 'tools/fsr412-vk/summarize-benchmark.py')
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class BenchmarkTests(unittest.TestCase):
    def test_warmup_and_tail(self):
        result = summary.summarize('noise\nBENCH,0,999,999,999\n' + '\n'.join(
            f'BENCH,{i},{i},{2*i},{3*i}' for i in range(1, 101)), 1)
        self.assertEqual(result['samples'], 100)
        self.assertEqual(result['cpu_record_ms'], {'mean': 50.5, 'p50': 50, 'p95': 95, 'p99': 99, 'max': 100})
        self.assertEqual(result['gpu_replay_ms']['p99'], 198)

    def test_bad_or_empty_samples(self):
        for text in ('', 'BENCH,0,1,2,3', 'BENCH,40,nan,2,3', 'BENCH,40,-1,2,3', 'BENCH,40,1,2'):
            with self.assertRaises(ValueError):
                summary.summarize(text)


if __name__ == '__main__':
    unittest.main()
