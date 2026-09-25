#!/usr/bin/env python3
"""Ledger verification for a Memcheck suppression file.

usage: ledger.py DRAFT.supp LABEL:NOSUPP.xml:SUPP.xml [...]

For each pair of reports on the same binary, without and with the draft:
  * removed records = blocking records of the unsuppressed report minus those
    still present with the draft (multiset over kind + symbolic stack);
  * every removed record must start in library code (first non-allocator frame
    of the error stack not in the program) -> otherwise VIOLATION;
  * every removed record must be matched by at least one draft stanza (checked
    offline with Valgrind's frame-matching semantics on fun:/obj: patterns);
  * records that appear only with the draft are reported (nondeterminism);
  * stanzas used by no pair (per Valgrind's suppcounts) are listed for pruning.
"""
import fnmatch
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter

PROGRAM = re.compile(r"/(GoldenCheetah|tst_[A-Za-z]+|webengine_control|qcss_control)$")
ALLOCATOR = re.compile(r"^(malloc|calloc|realloc|memalign|posix_memalign|operator new|operator new\[\]|strdup)\b")
BLOCKING_LEAKS = {"Leak_DefinitelyLost", "Leak_IndirectlyLost", "Leak_PossiblyLost"}
SKIND = {"Leak_DefinitelyLost": "Leak", "Leak_IndirectlyLost": "Leak", "Leak_PossiblyLost": "Leak",
         "InvalidRead": "Addr", "InvalidWrite": "Addr", "UninitCondition": "Cond",
         "UninitValue": "Value", "SyscallParam": "Param"}


def soname(obj):
    name = obj.rsplit("/", 1)[-1]
    match = re.match(r"(.+?\.so\.\d+)", name)
    return match.group(1) if match else name


def records(path):
    root = ET.parse(path).getroot()
    out = []
    for error in root.findall("error"):
        kind = error.findtext("kind")
        if kind == "Leak_StillReachable":
            continue
        frames = [(f.findtext("fn") or "", f.findtext("obj") or "") for f in error.find("stack").findall("frame")]
        mangled = []
        suppression = error.find("suppression")
        if suppression is not None:
            mangled = [(s.findtext("fun") or "", s.findtext("obj") or "") for s in suppression.findall("sframe")]
        size = int(error.findtext("xwhat/leakedbytes") or 0)
        out.append({"kind": kind, "frames": frames, "mangled": mangled, "bytes": size,
                    "key": (kind, tuple((fn, soname(obj)) for fn, obj in frames))})
    counts = {}
    for pair in root.findall("suppcounts/pair"):
        counts[pair.findtext("name")] = int(pair.findtext("count"))
    return out, counts


def stanzas(path):
    result = []
    with open(path) as handle:
        text = handle.read()
    for block in re.findall(r"\{\n(.*?)\n\}", text, re.S):
        lines = [l.strip() for l in block.splitlines() if l.strip() and not l.strip().startswith("#")]
        name, skind = lines[0], lines[1]
        leak_kinds = None
        frames = []
        for line in lines[2:]:
            if line.startswith("match-leak-kinds:"):
                leak_kinds = {k.strip() for k in line.split(":", 1)[1].split(",")}
            elif line.startswith(("fun:", "obj:")) or line == "...":
                frames.append(line)
        result.append({"name": name, "skind": skind, "leak_kinds": leak_kinds, "frames": frames})
    return demangle(result)


def demangle(result):
    """Adds each stanza's frames with demangled function names ("demangled")."""
    # Reports without --gen-suppressions only carry demangled names: keep both forms.
    names = sorted({f[4:] for s in result for f in s["frames"] if f.startswith("fun:")})
    demangled = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True,
                               text=True, check=True).stdout.splitlines()
    table = dict(zip(names, demangled))
    for s in result:
        s["demangled"] = [f"fun:{table[f[4:]]}" if f.startswith("fun:") else f for f in s["frames"]]
    return result


def frame_matches(pattern, fun, obj):
    if pattern.startswith("fun:"):
        return bool(fun) and (fun == pattern[4:] or fnmatch.fnmatchcase(fun, pattern[4:]))
    return fnmatch.fnmatchcase(obj, pattern[4:])


def stanza_matches(stanza, record):
    kind = record["kind"]
    expected = "Memcheck:" + SKIND.get(kind, "?")
    if stanza["skind"] != expected and not (expected[-4:] in ("alue", "Addr")
                                            and re.fullmatch(re.escape(expected) + r"\d+", stanza["skind"])):
        return False
    if kind in BLOCKING_LEAKS and stanza["leak_kinds"] is not None:
        wanted = {"Leak_DefinitelyLost": "definite", "Leak_IndirectlyLost": "indirect",
                  "Leak_PossiblyLost": "possible"}[kind]
        if wanted not in stanza["leak_kinds"] and "all" not in stanza["leak_kinds"]:
            return False
    stack = record["mangled"] or record["frames"]
    patterns = stanza["frames"] if record["mangled"] else stanza["demangled"]
    objs = [obj for _, obj in record["frames"]]
    if len(patterns) > len(stack):
        return False
    for index, pattern in enumerate(patterns):
        fun = stack[index][0]
        obj = objs[index] if index < len(objs) else stack[index][1]
        if not frame_matches(pattern, fun, obj):
            return False
    return True


def starts_in_library(record):
    for fn, obj in record["frames"]:
        if ALLOCATOR.match(fn) or "vgpreload" in obj:
            continue
        return not PROGRAM.search(obj), fn or obj
    return False, "?"


def main():
    draft = stanzas(sys.argv[1])
    used = Counter()
    ok = True
    for spec in sys.argv[2:]:
        label, nosupp, supp = spec.split(":")
        before, _ = records(nosupp)
        after, counts = records(supp)
        used.update({k: v for k, v in counts.items() if v})
        removed = Counter(r["key"] for r in before) - Counter(r["key"] for r in after)
        appeared = Counter(r["key"] for r in after) - Counter(r["key"] for r in before)
        by_key = {r["key"]: r for r in before}
        violations, unmatched, kinds = [], [], Counter()
        for key, count in removed.items():
            record = by_key[key]
            kinds[record["kind"]] += count
            library, origin = starts_in_library(record)
            if not library:
                violations.append((count, record["kind"], origin))
            if not any(stanza_matches(s, record) for s in draft):
                unmatched.append((count, record["kind"], origin))
        print(f"== {label}: {len(before)} -> {len(after)} blocking records; removed {sum(removed.values())} "
              f"{dict(kinds)}; appeared-only-with-draft {sum(appeared.values())}")
        for count, kind, origin in violations:
            ok = False
            print(f"   VIOLATION removed record starts in program: {count}x {kind} at {origin[:100]}")
        for count, kind, origin in unmatched:
            print(f"   note: removed but no offline stanza match (nondeterminism?): {count}x {kind} at {origin[:100]}")
        for key, count in appeared.items():
            print(f"   appeared: {count}x {key[0]} top {[f for f, _ in key[1][:3]]}")
        remaining = Counter()
        for r in after:
            library, origin = starts_in_library(r)
            gc = next((fn for fn, obj in r["frames"] if PROGRAM.search(obj)), "-")
            remaining[(r["kind"], "lib" if library else "PROGRAM", gc[:70])] += 1
        print("   remaining (kind, origin, first program frame):")
        for (kind, where, gc), count in remaining.most_common():
            print(f"     {count:4d} {kind} {where} {gc}")
    unused = [s["name"] for s in draft if s["name"] not in used]
    print(f"== stanzas: {len(draft)} in draft, {len(draft) - len(unused)} used, {len(unused)} unused")
    print("VERDICT", "OK" if ok else "VIOLATIONS")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
