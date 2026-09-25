#!/usr/bin/env python3
"""Size guard for the definite,indirect stanzas (DEVELOPMENT-WORKFLOW.md).

usage: check_widened.py SUPP_FILE UNSUPPRESSED_REPORT.xml

A suppression cannot bound the size of what it matches, so every gate and
fixture run checks that the records these stanzas match in the unsuppressed
report have exactly the sizes the pure-Qt controls produced, each at most once:

* GPUInfo fallback/offscreen surfaces: single blocks of 40, 168, 248 (the
  offscreen/QtTest variant) or 320 bytes, either kind;
* members of the WebEngine context's two lost mojo/ipcz structures (ipcz-cycle-members.json):
  as the definitely lost root, exactly their own structure's total (3240 bytes for
  the GPU process-host invitation pipe, 3400 for the UKM recorder pipe); as an
  indirectly lost block, the member's size in the controls.

The structures' other roots in the controls (the UKM Router, not widened) are
checked the same way on their full chains, so a structure also cannot grow
unnoticed while one of them is its root. Anything else, including a widened
stanza none of these rules covers, means a growing or different leak: exit 1. A
report in which they match nothing passes and says so.
"""
import json
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ledger  # noqa: E402

SURFACE_SIZES = {40, 168, 248, 320}
SURFACES = ("QOffscreenSurface", "newFallbackSurface")
CYCLE = json.loads((Path(__file__).resolve().parent / "ipcz-cycle-members.json").read_text())
MEMBERS = {tuple(member["frames"]): member for member in CYCLE["members"]}
ROOTS = ledger.demangle([{"name": f"ipcz-{root['structure']}-root", "skind": "Memcheck:Leak",
                          "leak_kinds": {"definite", "indirect"}, "frames": root["frames"]}
                         for root in CYCLE["roots"]])


def rule_for(stanza):
    """(label, allowed indirect sizes, allowed definite totals) of a widened stanza or root."""
    member = MEMBERS.get(tuple(stanza["frames"])) or next(
        (root for root in CYCLE["roots"] if root["frames"] == stanza["frames"]), None)
    if member is not None:
        return f"ipcz structure {member['structure']}", {member["size"]}, {member["structure"]}
    frames = " ".join(stanza["demangled"])
    label = next((marker for marker in SURFACES if marker in frames), None)
    if label is None:
        return "no rule", set(), set()
    return label, SURFACE_SIZES, SURFACE_SIZES


def main():
    widened = [s for s in ledger.stanzas(sys.argv[1]) if s["leak_kinds"] == {"definite", "indirect"}]
    root = ET.parse(sys.argv[2]).getroot()
    if root.tag != "valgrindoutput":
        sys.exit(f"{sys.argv[2]} is not a Memcheck XML report")
    records, _ = ledger.records(sys.argv[2])
    blocks = [int(e.findtext("xwhat/leakedblocks") or 0) for e in root.findall("error")
              if e.findtext("kind") != "Leak_StillReachable"]
    ok = len(widened) == 2 + len(MEMBERS)
    for stanza in widened + ROOTS:
        marker, indirect_sizes, definite_totals = rule_for(stanza)
        found = [(r["kind"], r["bytes"], b) for r, b in zip(records, blocks)
                 if ledger.stanza_matches(stanza, r)]
        sizes = Counter(size for _, size, _ in found)
        good = marker != "no rule" and max(sizes.values(), default=0) <= 1 and all(
            b == 1 and size in (definite_totals if kind == "Leak_DefinitelyLost" else indirect_sizes)
            for kind, size, b in found)
        ok &= good
        shown = sorted((kind[5:], size, b) for kind, size, b in found)
        print(f"{'OK' if good else 'MISMATCH'} {stanza['name']} ({marker}): "
              f"{shown if shown else 'no matching record in this report'}")
    print("WIDENED", "OK" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
