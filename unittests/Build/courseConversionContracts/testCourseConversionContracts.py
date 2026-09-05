#!/usr/bin/env python3

import json
import math
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
HEADER = ROOT / "src/Train/WorkoutGameCoursePrescription.h"
CONVERSION_HEADER = ROOT / "src/Train/WorkoutGameCourseConversion.h"
SUMMARY_HEADER = ROOT / "src/Train/WorkoutGameCourseSummary.h"
DOCUMENT_HEADER = ROOT / "src/Train/WorkoutGameCourseDocument.h"
DIALOG_SOURCE = ROOT / "src/Train/WorkoutGameCourseConversionDialog.cpp"
PREVIEW_SOURCE = ROOT / "src/Train/WorkoutGameCoursePreviewWidget.cpp"
TERRAIN_SOURCE = ROOT / "src/Train/WorkoutGameCourseTerrain.cpp"
DESIGN = ROOT / "doc/design/WORKOUT_GAME_COURSE_CONVERSION_MODES.md"
FIXTURE = (ROOT / "unittests/Train/workoutGameCourseConversion/fixtures"
           / "mode_contract.json")


def load_points(intervals, ftp, durations):
    return sum(
        100.0 * (duration / 3_600_000.0)
        * (interval["startWatts"] ** 2
           + interval["startWatts"] * interval["endWatts"]
           + interval["endWatts"] ** 2)
        / (3.0 * ftp ** 2)
        for interval, duration in zip(intervals, durations)
    )


class CourseConversionContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        cls.design = DESIGN.read_text(encoding="utf-8")

    def test_unannotated_fixture_is_fail_safe_in_every_mode(self):
        self.assertIsNone(self.fixture["prescriptionMetadata"])
        source = [item["durationMs"] for item in self.fixture["intervals"]]
        source_load = load_points(
            self.fixture["intervals"], self.fixture["ftpWatts"], source)

        for mode, expected in self.fixture["expected"].items():
            with self.subTest(mode=mode):
                output = expected["outputDurationMs"]
                self.assertEqual(output, source)
                self.assertEqual(sum(output), expected["durationMs"])
                self.assertEqual(expected["workDeviationPercent"], 0.0)
                self.assertEqual(expected["recoveryDeviationPercent"], 0.0)
                self.assertEqual(expected["loadDeviationPercent"], 0.0)
                self.assertTrue(math.isclose(
                    load_points(self.fixture["intervals"],
                                self.fixture["ftpWatts"], output),
                    source_load, rel_tol=0.0, abs_tol=1.0e-12))

    def test_five_minute_recovery_is_checked_separately(self):
        matches = [
            (index, interval)
            for index, interval in enumerate(self.fixture["intervals"])
            if interval["id"] == "five-minute-recovery"
        ]
        self.assertEqual(len(matches), 1)
        index, recovery = matches[0]
        self.assertEqual(recovery["durationMs"], 5 * 60 * 1000)
        average_watts = (recovery["startWatts"]
                         + recovery["endWatts"]) * 0.5
        self.assertLessEqual(average_watts / self.fixture["ftpWatts"], 0.65)
        self.assertGreater(index, 0)
        self.assertLess(index + 1, len(self.fixture["intervals"]))

        for mode, expected in self.fixture["expected"].items():
            with self.subTest(mode=mode):
                self.assertEqual(expected["outputDurationMs"][index], 300000)
        self.assertEqual(
            self.fixture["contracts"]["Balanced"]
                ["minimumRecoveryRetention"],
            1.0)
        self.assertEqual(
            self.fixture["contracts"]["RideFirst"]
                ["minimumRecoveryRetention"],
            1.0)
        self.assertEqual(
            self.fixture["contracts"]["RideFirst"]
                ["defaultRecoveryRetention"],
            1.0)

    def test_prescription_limits_are_workout_first(self):
        contracts = self.fixture["contracts"]
        for mode in ("WorkoutFirst", "Balanced", "RideFirst"):
            with self.subTest(mode=mode):
                self.assertEqual(
                    contracts[mode]["maximumIntervalPowerErrorWatts"], 0.0)
                self.assertEqual(
                    contracts[mode]["maximumKeyEffortDurationErrorMs"], 0)
        self.assertEqual(
            contracts["WorkoutFirst"]["maximumTotalDurationDeviationPercent"],
            0.0)
        self.assertEqual(
            contracts["Balanced"]["maximumNonPrescriptiveChangePercent"],
            3.0)
        self.assertEqual(
            contracts["Balanced"]["minimumRecoveryRetention"], 1.0)
        self.assertEqual(
            contracts["RideFirst"]["maximumTotalDurationDeviationPercent"],
            8.0)
        self.assertGreaterEqual(
            contracts["RideFirst"]["minimumRecoveryRetention"], 1.0)

    def test_terrain_and_feature_bands_are_distinct(self):
        contracts = self.fixture["contracts"]
        modes = ("WorkoutFirst", "Balanced", "RideFirst")
        self.assertEqual(
            [contracts[mode]["gradeScale"] for mode in modes],
            [0.82, 1.0, 1.18])
        self.assertEqual(
            [contracts[mode]["technicality"] for mode in modes],
            [0.15, 0.55, 0.95])
        self.assertEqual(
            [contracts[mode]["technicalTerrainExposurePercent"] for mode in modes],
            [[25.0, 45.0], [50.0, 75.0], [75.0, 100.0]])
        self.assertEqual(
            [contracts[mode]["technicalFeatureDensityPerTenSections"]
             for mode in modes],
            [[2.0, 4.0], [5.0, 7.0], [8.0, 10.0]])
        self.assertEqual(
            [contracts[mode]["minimumRuntimeWorkExposurePercent"]
             for mode in modes],
            [100.0, 100.0, 100.0])
        self.assertEqual(
            [contracts[mode]["minimumRuntimeRecoveryExposurePercent"]
             for mode in modes],
            [100.0, 100.0, 100.0])
        self.assertEqual(
            [contracts[mode]["minimumRuntimeKeyEffortExposurePercent"]
             for mode in modes],
            [100.0, 100.0, 100.0])
        self.assertEqual(
            contracts["WorkoutFirst"]["allowedTechnicalTerrain"],
            ["roots", "rollers", "easy-rock-garden", "log-over"])
        self.assertNotIn(
            "gap-jump", contracts["WorkoutFirst"]["allowedTechnicalTerrain"])
        for mode in modes:
            with self.subTest(mode=mode):
                self.assertTrue(contracts[mode]["scoredChallengeOnWorkAllowed"])
                self.assertFalse(
                    contracts[mode]["scoredChallengeOnRecoveryAllowed"])

    def test_design_records_preview_metadata_and_legacy_rules(self):
        normalized_design = " ".join(self.design.split())
        required = (
            "No conversion mode may shorten the nominal duration of any such interval",
            "Runtime progression may reach a section boundary only after",
            "ordinary 5:00",
            "technical terrain exposure",
            "Workout first is not a no-game mode",
            "Prescribed recovery never receives a scored challenge",
            "feature count and density",
            "Create/Save",
            "preserved/total hard and easy segments",
            "must never be inferred only from aggregate work/rest percentages",
            "strictly increase",
            "nested technical sets",
            "SmoothTrail in every mode",
            "original workout power profile on a time axis",
            "schema version 4",
            "original workout's ordered lap markers and timed text instructions",
            "Schema 1 through 3 documents remain readable and canonical",
        )
        for phrase in required:
            with self.subTest(phrase=phrase):
                self.assertIn(phrase, normalized_design)

    def test_preview_exposes_accurate_segment_retention(self):
        conversion = (CONVERSION_HEADER.read_text(encoding="utf-8")
                      + SUMMARY_HEADER.read_text(encoding="utf-8"))
        dialog = DIALOG_SOURCE.read_text(encoding="utf-8")
        for token in (
                "preservedKeyEffortCount",
                "keyEffortCount",
                "preservedRecoveryCount",
                "recoveryCount"):
            with self.subTest(token=token):
                self.assertIn(token, conversion)
        for object_name in (
                "keyEffortRetentionValue",
                "recoveryRetentionValue",
                "workoutFirstComparisonValue",
                "balancedComparisonValue",
                "rideFirstComparisonValue"):
            with self.subTest(object_name=object_name):
                self.assertIn(object_name, dialog)
        for label in ("Hard segments preserved", "Easy segments preserved"):
            with self.subTest(label=label):
                self.assertIn(label, dialog)
        self.assertNotIn("Key efforts preserved", dialog)
        self.assertNotIn("Recoveries preserved", dialog)

    def test_preview_and_terrain_implement_audited_semantics(self):
        preview = PREVIEW_SOURCE.read_text(encoding="utf-8")
        terrain = TERRAIN_SOURCE.read_text(encoding="utf-8")
        dialog = DIALOG_SOURCE.read_text(encoding="utf-8")
        self.assertIn("workoutPowerProfile", preview)
        self.assertIn("Original workout power - time", preview)
        self.assertIn("Generated terrain - distance", preview)
        self.assertIn("selectTechnicalTerrain", terrain)
        self.assertIn("WorkoutGameTerrainKind::SmoothTrail", terrain)
        self.assertIn("courseModeComparison", dialog)
        self.assertIn("curve events", dialog)

    def test_production_contract_header_and_api_exist(self):
        self.assertTrue(
            HEADER.is_file(),
            "missing production contract/header/API: "
            "src/Train/WorkoutGameCoursePrescription.h")
        source = (HEADER.read_text(encoding="utf-8")
                  + SUMMARY_HEADER.read_text(encoding="utf-8"))
        conversion = (CONVERSION_HEADER.read_text(encoding="utf-8")
                      + SUMMARY_HEADER.read_text(encoding="utf-8"))
        document = DOCUMENT_HEADER.read_text(encoding="utf-8")
        for token in (
                "WorkoutGameCourseModeContract",
                "WorkoutGameCoursePrescriptionMetadata",
                "WorkoutGameCourseIntervalRole",
                "minimumRecoveryRetention",
                "maximumNonPrescriptiveDurationChangePercent",
                "technicalTerrainExposure",
                "technicalFeatureDensityPerTenSections"):
            with self.subTest(token=token):
                self.assertIn(token, source)
        self.assertIn("prescriptionMetadata", conversion)
        self.assertIn("CurrentSchemaVersion = 4", document)
        self.assertIn("CurrentConversionAlgorithmVersion = 3", document)
        self.assertIn("conversionAlgorithmVersion", document)
        self.assertIn("sourceLaps", document)
        self.assertIn("sourceTexts", document)


if __name__ == "__main__":
    unittest.main()
