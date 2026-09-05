#!/usr/bin/env python3

import json
import math
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
HEADER = ROOT / "src/Train/WorkoutGameCoursePrescription.h"
FIXTURE = (ROOT / "unittests/Train/workoutGameCourseConversion/fixtures"
           / "mode_contract.json")


def constant(source, name):
    match = re.search(
        rf"static constexpr double {name}\s*=\s*([0-9.]+);", source)
    if not match:
        raise AssertionError(f"missing contract constant {name}")
    return float(match.group(1))


def load_points(intervals, ftp, duration_key):
    return sum(
        100.0 * (interval[duration_key] / 3_600_000.0)
        * (interval["startWatts"] ** 2
           + interval["startWatts"] * interval["endWatts"]
           + interval["endWatts"] ** 2)
        / (3.0 * ftp ** 2)
        for interval in intervals
    )


class CourseConversionContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))

    def transform(self, mode):
        ftp = self.fixture["ftpWatts"]
        recovery_threshold = constant(self.header, "RecoveryIntensity")
        long_seconds = constant(self.header, "KeyLongDurationSeconds")
        long_intensity = constant(self.header, "KeyLongIntensity")
        hard_seconds = constant(self.header, "KeyHardDurationSeconds")
        hard_intensity = constant(self.header, "KeyHardIntensity")
        sprint_seconds = constant(self.header, "KeySprintDurationSeconds")
        sprint_intensity = constant(self.header, "KeySprintIntensity")
        work_scale = constant(self.header, mode + "OtherWorkScale")
        recovery_scale = constant(self.header, mode + "RecoveryScale")
        load_limit = constant(self.header, mode + "MaximumLoadDeviationPercent")
        output = []
        for source in self.fixture["intervals"]:
            item = dict(source)
            intensity = ((item["startWatts"] + item["endWatts"]) * 0.5
                         / ftp)
            item["recovery"] = intensity <= recovery_threshold
            seconds = item["durationMs"] / 1000.0
            item["key"] = (not item["recovery"] and (
                (seconds >= long_seconds and intensity >= long_intensity)
                or (seconds >= hard_seconds and intensity >= hard_intensity)
                or (seconds >= sprint_seconds and intensity >= sprint_intensity)))
            scale = (1.0 if item["key"] else
                     recovery_scale if item["recovery"] else work_scale)
            item["outputDurationMs"] = round(item["durationMs"] * scale)
            output.append(item)

        source_load = load_points(output, ftp, "durationMs")
        output_load = load_points(output, ftp, "outputDurationMs")
        loss = 100.0 * (source_load - output_load) / source_load
        if loss > load_limit:
            fraction = load_limit / loss
            for item in output:
                if not item["key"]:
                    delta = item["durationMs"] - item["outputDurationMs"]
                    item["outputDurationMs"] = round(
                        item["durationMs"] - delta * fraction)
        return output

    def test_fixture_matches_published_contract(self):
        for mode, expected in self.fixture["expected"].items():
            output = self.transform(mode)
            source_work = sum(i["durationMs"] for i in output
                              if not i["recovery"])
            output_work = sum(i["outputDurationMs"] for i in output
                              if not i["recovery"])
            source_recovery = sum(i["durationMs"] for i in output
                                  if i["recovery"])
            output_recovery = sum(i["outputDurationMs"] for i in output
                                  if i["recovery"])
            source_load = load_points(
                output, self.fixture["ftpWatts"], "durationMs")
            output_load = load_points(
                output, self.fixture["ftpWatts"], "outputDurationMs")
            self.assertEqual(sum(i["outputDurationMs"] for i in output),
                             expected["durationMs"])
            self.assertAlmostEqual(
                100.0 * (output_work - source_work) / source_work,
                expected["workDeviationPercent"], places=9)
            self.assertAlmostEqual(
                100.0 * (output_recovery - source_recovery) / source_recovery,
                expected["recoveryDeviationPercent"], places=9)
            self.assertAlmostEqual(
                100.0 * (output_load - source_load) / source_load,
                expected["loadDeviationPercent"], places=9)
            for item in output:
                if item["key"]:
                    self.assertEqual(item["outputDurationMs"],
                                     item["durationMs"])
                    self.assertEqual(item["startWatts"],
                                     self.fixture["intervals"][output.index(item)]
                                     ["startWatts"])

    def test_contract_limits_are_ordered_and_safe(self):
        self.assertLess(
            constant(self.header, "WorkoutFirstMaximumLoadDeviationPercent"),
            constant(self.header, "BalancedMaximumLoadDeviationPercent"))
        self.assertLess(
            constant(self.header, "BalancedMaximumLoadDeviationPercent"),
            constant(self.header, "RideFirstMaximumLoadDeviationPercent"))
        self.assertGreaterEqual(
            constant(self.header, "RideFirstRecoveryScale"), 0.85)
        self.assertLessEqual(
            constant(self.header, "RideFirstMaximumLoadDeviationPercent"), 5.0)


if __name__ == "__main__":
    unittest.main()
