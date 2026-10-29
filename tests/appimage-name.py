#!/usr/bin/env python3
"""Date boundaries, daily revision increments and rerun stability."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "packaging/chen-appimage/appimage-name.py"
spec = importlib.util.spec_from_file_location("appimage_name", path)
naming = importlib.util.module_from_spec(spec)
spec.loader.exec_module(naming)


class AppImageNames(unittest.TestCase):
    build_run = {"id": 100, "created_at": "2026-10-28T23:00:00Z", "head_sha": "abc12345" + "0" * 32}

    def artifact(self, suffix, run_id=99):
        return {"name": naming.ARTIFACT_PREFIX + suffix, "workflow_run": {"id": run_id}}

    def test_first_daily_package(self):
        names = naming.select_names(self.build_run, [])
        self.assertEqual(names["revision"], 0)
        self.assertEqual(names["appimage_name"], "KytyPS5-chenxiao07-abc12345-x86_64-102826.AppImage")
        self.assertEqual(names["artifact_name"], naming.ARTIFACT_PREFIX + "102826")

    def test_more_packages_same_day(self):
        first = self.artifact("102826")
        self.assertEqual(naming.select_names(self.build_run, [first])["revision"], 1)
        self.assertTrue(naming.select_names(self.build_run, [first, self.artifact("102826v1", 98)])
                        ["appimage_name"].endswith("-102826v2.AppImage"))

    def test_use_highest_revision_after_deletion(self):
        names = naming.select_names(self.build_run, [self.artifact("102826v3")])
        self.assertEqual(names["revision"], 4)

    def test_new_day_resets_revision(self):
        run = dict(self.build_run, created_at="2026-10-29T04:01:00Z")
        self.assertTrue(naming.select_names(run, [self.artifact("102826v3")])
                        ["appimage_name"].endswith("-102926.AppImage"))

    def test_use_local_date_before_utc_midnight_boundary(self):
        run = dict(self.build_run, created_at="2026-10-29T01:00:00Z")
        self.assertEqual(naming.select_names(run, [])["date"], "102826")

    def test_rerun_retains_its_original_name(self):
        artifacts = [self.artifact("102826v1", 100), self.artifact("102826v4", 104)]
        self.assertEqual(naming.select_names(self.build_run, artifacts)["revision"], 1)

    def test_other_artifacts_do_not_consume_revisions(self):
        artifacts = [self.artifact("102726v9"), self.artifact("102826v0"),
                     {"name": "KytyPS5-chenxiao07-AppImage-x86_64", "workflow_run": {"id": 1}}]
        self.assertEqual(naming.select_names(self.build_run, artifacts)["revision"], 0)

    def test_conflicting_rerun_names_are_rejected(self):
        with self.assertRaises(ValueError):
            naming.select_names(self.build_run, [self.artifact("102826", 100), self.artifact("102826v1", 100)])


if __name__ == "__main__":
    unittest.main()
