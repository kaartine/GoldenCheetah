#!/usr/bin/env python3
"""Members of the two lost mojo/ipcz structures of a WebEngine profile, from
pure-Qt control reports and Valgrind's block lists of the same controls.

usage: cycle_members.py OUTPUT.json CONTROL.xml... -- BLOCK_LIST.txt...
       (the profile-only offscreen controls: disk-noloop and disk-cycles
       repetitions; block lists as described in README.md, "Structure members")

The WebEngine context leaves two unreachable structures, once per process, each
rooted in an ipcz Router: the GPU process-host invitation pipe (3,240 bytes,
created with the context by the first profile) and the UKM recorder pipe (3,400
bytes, created on the in-process GPU thread). ~ProfileAdapter fills mojo blocks
into both. Memcheck reports as
definitely lost whichever block of a structure its scan reaches first, so a
chain of a structure can be a definite root or an indirect block. A chain is a
member if
  (1) its full chain ends at one of the two program-boundary entries: through
      WebEngineContext::WebEngineContext() in the ProfileAdapter constructor with
      the Router as the definitely lost root, or through flushMessages() in
      ~ProfileAdapter as an indirectly lost block;
  (2) it occurs in every control report; and
  (3) in every block list, its record is the root of, or a block held by, the
      root of the same structure (named by the root's total).
Chains that meet (1) and (2) but not (3) are listed as unplaced and are not
widened. The definite roots that are not members (the UKM Router) are listed as
roots. Members and roots of a structure must fit in its total.
final_supp.py widens exactly the members; check_widened.py bounds the sizes of
members and roots.
"""
import json
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_supp  # noqa: E402

ANY_HEADER = re.compile(r" in loss record [\d,]+ of ")
HEADER = re.compile(r"^([\d,]+)(?: \(([\d,]+) direct, [\d,]+ indirect\))? bytes in [\d,]+ blocks "
                    r"are (definitely|indirectly|possibly) lost in loss record ([\d,]+) of")
FRAME = re.compile(r"^   (?:at|by) 0x[0-9A-Fa-f]+: (.*?)(?: \((?:in )?[^()]*\))?$")
HELD = re.compile(r"indirect loss record ([\d,]+)")
STRUCTURES = {3240: "GPU process-host invitation pipe", 3400: "UKM recorder pipe"}


def number(text):
    return int(text.replace(",", ""))


def chains(path):
    """(entry or None, kind, bytes, stanza frames, demangled chain) of each blocking leak record."""
    result = []
    for error in ET.parse(path).getroot().findall("error"):
        kind = error.findtext("kind")
        suppression = error.find("suppression")
        if kind not in ("Leak_DefinitelyLost", "Leak_IndirectlyLost") or suppression is None:
            continue
        stack = error.find("stack").findall("frame")
        names = [frame.findtext("fn") or "???" for frame in stack]
        objects = [frame.findtext("obj") or "" for frame in stack]
        end = next((i for i, obj in enumerate(objects) if gen_supp.PROGRAM_OBJECTS.search(obj)), len(stack))
        if end == 0:
            continue
        chain = names[:end]
        joined = " ".join(chain)
        entry = None
        if end == len(stack):  # no program frame: a thread-rooted or truncated chain, never a member
            pass
        elif (kind == "Leak_DefinitelyLost" and "WebEngineContext::WebEngineContext()" in joined
                and "MakeRefCounted<ipcz::Router>" in joined
                and chain[-1].startswith("QWebEngineProfile::QWebEngineProfile")):
            entry = "profile-constructor"
        elif (kind == "Leak_IndirectlyLost" and "flushMessages" in joined
              and "ProfileAdapter::~ProfileAdapter" in joined
              and chain[-1].startswith("QWebEngineProfile::~QWebEngineProfile")):
            entry = "profile-destructor"
        sframes = suppression.findall("sframe")[:end]
        frames = tuple(f"fun:{s.findtext('fun')}" if s.findtext("fun")
                       else gen_supp.library_pattern(objects[i]) for i, s in enumerate(sframes))
        size = int(error.findtext("xwhat/leakedbytes"))
        if kind == "Leak_DefinitelyLost":  # the root's own block: "3,240 (200 direct, 3,040 indirect)"
            direct = re.search(r"\(([\d,]+) direct", error.findtext("xwhat/text") or "")
            size = int(direct.group(1).replace(",", "")) if direct else size
        result.append((entry, kind, size, frames, tuple(chain)))
    return result


def block_list(path):
    """Loss records {nr: (kind, total, own size, demangled stack)} and {root nr: held record nrs}."""
    records, held = {}, defaultdict(set)
    current, listing = None, False
    for raw in Path(path).read_text(errors="replace").splitlines():
        if raw.startswith("sending command "):
            listing, current = raw.startswith("sending command block_list"), None
            continue
        line = re.sub(r"^==\d+== ?", "", raw)
        header = HEADER.match(line)
        if not header and ANY_HEADER.search(line):  # e.g. still reachable: not ours
            current = None
            continue
        if header:
            total, direct, kind, nr = header.groups()
            current = number(nr)
            fresh = current not in records
            if fresh:
                records[current] = (kind, number(total), number(direct or total), [])
            continue
        if current is None:
            continue
        frame = FRAME.match(line)
        if frame:
            if fresh:
                records[current][3].append(frame.group(1))
        elif listing and records[current][0] == "definitely":
            nested = HELD.search(line)
            if nested:
                held[current].add(number(nested.group(1)))
    return records, held


def structures(path, chains_):
    """{chain: (structure total, kind, own size)} for every chain whose one record in the
    block list is the root of one of the STRUCTURES or held by it."""
    records, held = block_list(path)
    roots = [root for root in held if records[root][1] in STRUCTURES]
    by_stack = defaultdict(list)
    for nr, (_, _, _, stack) in records.items():
        by_stack[tuple(stack)].append(nr)
    placed = {}
    for chain in chains_:
        numbers = [nr for stack, nrs in by_stack.items() if stack[:len(chain)] == chain for nr in nrs]
        totals = set()
        for nr in numbers:
            for root in roots:
                if nr == root or nr in held[root]:
                    totals.add(records[root][1])
        if len(numbers) == 1 and len(totals) == 1:
            kind, _, own, _ = records[numbers[0]]
            placed[chain] = (totals.pop(), kind, own)
    return placed


def main():
    output, rest = sys.argv[1], sys.argv[2:]
    if "--" not in rest:
        sys.exit(__doc__)
    reports, lists = rest[:rest.index("--")], rest[rest.index("--") + 1:]
    if not reports or not lists:
        sys.exit(__doc__)
    per_report = []
    for path in reports:
        found = chains(path)
        repeated = {f for f in {c[3] for c in found} if sum(c[3] == f for c in found) > 1}
        if any(c[0] and c[3] in repeated for c in found):
            sys.exit(f"{path}: a candidate chain occurs in more than one record")
        per_report.append({frames: (entry, kind, size, chain) for entry, kind, size, frames, chain in found})
    common = set(per_report[0]).intersection(*per_report[1:])
    first = per_report[0]
    candidates = {f for f in common if first[f][0]}
    placements = [structures(path, {first[f][3] for f in common}) for path in lists]
    members, unplaced, roots = [], [], []
    for frames in sorted(common, key=lambda f: (first[f][2], f)):
        entry, kind, size, chain = first[frames]
        seen = {p.get(chain) for p in placements}
        place = seen.pop() if len(seen) == 1 else None
        if frames in candidates:
            if place and place[2] == size:
                members.append({"entry": entry, "structure": place[0], "size": size, "frames": list(frames)})
            else:
                unplaced.append({"entry": entry, "size": size, "frames": list(frames)})
        elif place and place[1] == "definitely" and kind == "Leak_DefinitelyLost" and place[2] == size:
            roots.append({"structure": place[0], "size": size, "frames": list(frames)})
    totals = defaultdict(int)
    for item in members + roots:
        totals[item["structure"]] += item["size"]
    for structure, used in totals.items():
        if used > structure:
            sys.exit(f"structure {structure}: members and roots total {used} B")
    members.sort(key=lambda m: (m["structure"], m["entry"], m["size"], m["frames"]))
    Path(output).write_text(json.dumps({
        "reports": [Path(r).name for r in reports],
        "block_lists": [str(Path(p).parent.name + "/" + Path(p).name) for p in lists],
        "structures": {str(s): {"members_and_roots_bytes": totals[s]} for s in sorted(totals)},
        "members": members, "roots": roots, "unplaced": unplaced}, indent=1) + "\n")
    print(f"{len(members)} members ({', '.join(f'{s}: {totals[s]} B' for s in sorted(totals))}), "
          f"{len(roots)} other roots, {len(unplaced)} unplaced")


if __name__ == "__main__":
    main()
