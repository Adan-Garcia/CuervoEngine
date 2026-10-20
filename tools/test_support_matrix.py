#!/usr/bin/env python3
"""Unit tests for tools/support_matrix.py (run by ctest as support_matrix_unit)."""

import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import support_matrix as sm  # noqa: E402


class SupportMatrix(unittest.TestCase):
    def setUp(self):
        self.data = sm.load()

    def test_source_is_consistent(self):
        self.assertEqual(sm.validate(self.data), [])

    def test_documents_are_up_to_date(self):
        self.assertEqual(sm.check(self.data), [])

    def first_entry(self, id_):
        for s in self.data["sections"]:
            for e in s["entries"]:
                if e["id"] == id_:
                    return e
        raise KeyError(id_)

    def test_flag_disagreement_is_rejected(self):
        e = self.first_entry("tier-full")
        e["web"] = True
        self.assertTrue(any("'web' flag" in m for m in sm.validate(self.data)))

    def test_planned_feature_on_a_platform_is_rejected(self):
        e = self.first_entry("physics")
        e["platforms"] = ["linux"]
        e["desktop_only"] = True
        self.assertTrue(any("works on no platform" in m for m in sm.validate(self.data)))

    def test_shipped_feature_without_platform_is_rejected(self):
        e = self.first_entry("replays")
        e["platforms"] = []
        e["web"] = False
        e["desktop_only"] = False
        self.assertTrue(any("needs at least one platform" in m for m in sm.validate(self.data)))

    def test_windows_row_must_match_target_status(self):
        self.first_entry("windows")["status"] = "stable"
        self.assertTrue(any("target windows" in m for m in sm.validate(self.data)))

    def test_windows_stays_experimental(self):
        self.assertEqual(self.first_entry("windows")["status"], "experimental")

    def test_stale_region_is_detected(self):
        data = copy.deepcopy(self.data)
        self.first_entry("tier-full")  # sanity
        for s in data["sections"]:
            for e in s["entries"]:
                if e["id"] == "tier-full":
                    e["note"] = "changed"
        self.assertTrue(any("out of date" in m for m in sm.check(data)))

    def test_tables_mention_every_entry(self):
        status = sm.STATUS.read_text(encoding="utf-8")
        for s in self.data["sections"]:
            for e in s["entries"]:
                self.assertIn(e["name"].replace("|", "\\|"), status)


if __name__ == "__main__":
    unittest.main()
