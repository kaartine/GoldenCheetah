#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest


REPOSITORY = Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "contrib/workout-game-assets"
sys.path.insert(0, str(TOOLS))

import generate_runtime_asset_catalog as catalog  # noqa: E402


CATALOG_PATH = (
    REPOSITORY / "src/Resources/json/workout-game-asset-catalog.json"
)


class WorkoutGameRuntimeCatalogTest(unittest.TestCase):
    def test_generated_catalog_is_deterministic_and_fresh(self) -> None:
        first = catalog.generate_catalog_bytes(REPOSITORY)
        second = catalog.generate_catalog_bytes(REPOSITORY)

        self.assertEqual(first, second)
        self.assertTrue(first.endswith(b"\n"))
        self.assertEqual(first, CATALOG_PATH.read_bytes())

    def test_catalog_admits_only_approved_assets(self) -> None:
        document = json.loads(catalog.generate_catalog_bytes(REPOSITORY))
        self.assertEqual(document["schemaVersion"], 1)
        self.assertEqual(document["generatorVersion"], 1)
        self.assertEqual(
            [asset["assetId"] for asset in document["assets"]],
            [
                "EN-03-distant-ridges",
                "EN-08-forest-floor-props",
                "EN-09-forest-verge-clusters",
                "FT-02-log-over-greybox",
                "FT-03-bunny-hop-greybox",
                "FT-04-drop-greybox",
                "RB-01-rider-bike",
                "TR-08-surface-atlas",
            ],
        )
        self.assertNotIn("EN-01-conifer-set", first_ids(document))
        self.assertNotIn("FT-01-tabletop-greybox", first_ids(document))
        self.assertNotIn("FT-12-gap-jump-three-line", first_ids(document))

    def test_catalog_resources_are_packaged_and_physics_is_explicit(self) -> None:
        document = json.loads(catalog.generate_catalog_bytes(REPOSITORY))
        qrc_aliases = catalog.qrc_aliases(REPOSITORY)

        for asset in document["assets"]:
            self.assertEqual(asset["physics"]["authority"], "external")
            self.assertTrue(asset["resources"])
            for resource in asset["resources"]:
                self.assertEqual(
                    qrc_aliases[resource["repositoryPath"]],
                    resource["url"],
                )

    def test_catalog_digest_covers_canonical_payload(self) -> None:
        document = json.loads(catalog.generate_catalog_bytes(REPOSITORY))
        digest = document.pop("catalogSha256")
        self.assertEqual(digest, catalog.catalog_digest(document))


def first_ids(document: dict) -> set[str]:
    return {asset["assetId"] for asset in document["assets"]}


if __name__ == "__main__":
    unittest.main()
