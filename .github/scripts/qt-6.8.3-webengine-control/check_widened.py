#!/usr/bin/env python3
"""Size guard for the two definite,indirect stanzas (DEVELOPMENT-WORKFLOW.md).

usage: check_widened.py SUPP_FILE UNSUPPRESSED_REPORT.xml

A suppression cannot bound the size of what it matches, so every gate and
fixture run checks that the records these stanzas match in the unsuppressed
report are single blocks of the sizes the pure-Qt controls produced (each size at
most once; 248 B is the offscreen/QtTest variant). Anything else means a growing
or different leak: exit 1. A report in which they match nothing passes and says so.
"""
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ledger  # noqa: E402

CONTROL_SIZES = {40, 168, 248, 320}


def main():
    widened = [s for s in ledger.stanzas(sys.argv[1]) if s["leak_kinds"] == {"definite", "indirect"}]
    root = ET.parse(sys.argv[2]).getroot()
    records, _ = ledger.records(sys.argv[2])
    blocks = [int(e.findtext("xwhat/leakedblocks") or 0) for e in root.findall("error")
              if e.findtext("kind") != "Leak_StillReachable"]
    ok = len(widened) == 2
    for stanza in widened:
        found = [(r["bytes"], b) for r, b in zip(records, blocks) if ledger.stanza_matches(stanza, r)]
        sizes = Counter(size for size, _ in found)
        good = all(b == 1 and size in CONTROL_SIZES for size, b in found) and max(sizes.values(), default=0) <= 1
        ok &= good
        print(f"{'OK' if good else 'MISMATCH'} {stanza['name']}: "
              f"{sorted(found) if found else 'no matching record in this report'}")
    print("WIDENED", "OK" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
