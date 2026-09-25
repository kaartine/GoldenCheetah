#!/usr/bin/env python3
"""Final Qt 6.8.3 WebEngine exception files from all control reports, classified
and pruned against unsuppressed gate/fixture reports.

usage: final_supp.py OUTDIR APPDIR_GLIB_BUILD_ID PIN_FILE REPORT...
           -- SYSTEM_GLIB_CONTROL.xml... -- APPDIR_LIBS_CONTROL.xml...

REPORT is an unsuppressed gate/fixture report (NOSUPP.xml) or SUPP_FILE:SUPP.xml,
a run with the candidate files whose Valgrind suppression counts are read.

* stanzas come only from gen_supp.py over the control reports;
* split: main (no glib; fixtures and gate) / appdir-glib (gate only, glib pinned
  to the AppDir build); glib stanzas are taken only from controls that ran with
  the AppDir's libraries, never from system-glib controls;
* an allowed stanza is kept only if it matches (offline, Valgrind's frame
  semantics as in ledger.py) a blocking record of an unsuppressed report, or
  Valgrind credited it in a run with the candidate files. Credits alone are not
  enough to drop a stanza (Valgrind credits only the first matching one), and the
  offline matcher cannot compare the short names of inlined frames with the full
  signatures in reports, hence both. Candidates are classified before any run, so
  only allowed stanzas can take credit;
* fails on unpinned sonames or object wildcards.
"""
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

GEN = Path(__file__).resolve().parent / "gen_supp.py"


# Two full chains under QWebEngineProfile(const QString &) are one lost structure
# in the controls and the gate, but whether Memcheck reports a block as its root
# (definitely lost) or as reached from it (indirectly lost) varies between runs.
# Only these stanzas match both kinds; never possible or reachable. check_widened.py
# bounds their sizes and counts in every gate run.
GPU_INFO_TO_PROFILE = (
    "fun:_ZN19QRhiGles2InitParams18newFallbackSurfaceERK14QSurfaceFormat",
    "fun:_ZN15QtWebEngineCore7GPUInfoC1Ev", "fun:instance", "fun:instance", "fun:operator()",
    "fun:_ZN15QtWebEngineCore16WebEngineContext14isGbmSupportedEv",
    "fun:_ZN15QtWebEngineCore16WebEngineContextC1Ev",
    "fun:_ZN15QtWebEngineCore16WebEngineContext7currentEv",
    "fun:_ZN15QtWebEngineCore14ProfileAdapterC1ERK7QString",
    "fun:_ZN17QWebEngineProfileC1ERK7QStringP7QObject")
WIDENED_CHAINS = {
    "fallback-surface": ("fun:_Znwm", *GPU_INFO_TO_PROFILE),
    "offscreen-surface": ("fun:_Znwm", "fun:_ZN17QOffscreenSurfaceC1EP7QScreenP7QObject",
                          *GPU_INFO_TO_PROFILE),
}


def widen(body):
    frames = tuple(l for l in body if l.startswith(("fun:", "obj:")))
    kinds = [l for l in body if l.startswith("match-leak-kinds:")]
    for label, chain in WIDENED_CHAINS.items():
        if frames == chain and kinds and kinds[0] in ("match-leak-kinds: definite",
                                                      "match-leak-kinds: indirect"):
            return label, ("Memcheck:Leak", "match-leak-kinds: definite,indirect", *frames)
    return None, body


CUT_RANK = {"program": 0, "complete": 0, "dispatcher": 1, "library": 2, "truncated": 3}
ALLOCATOR = re.compile(r"^fun:(malloc|calloc|realloc|memalign|posix_memalign|_Znwm|_Znam|_ZnwmRKSt9nothrow_t|strdup)$")
# Generic Qt/glib event delivery frames: named, but they do not identify the path.
GENERIC = re.compile(r"^fun:(_ZN7QObject5eventEP6QEvent|_ZN19QApplicationPrivate13notify_helper|"
                     r"_ZN16QCoreApplication15notifyInternal2|_ZN23QCoreApplicationPrivate16sendPostedEvents|"
                     r"g_main_context_dispatch|g_main_context_iteration|_ZN20QEventDispatcherGlib13processEvents)")


def classify(body, cut):
    """DEVELOPMENT-WORKFLOW.md condition 2: error records only with the control's
    full chain (program or complete cut); a leak stanza cut inside the libraries
    whose frames are only unnamed objects may match only indirect leaks (their
    definite root is still reported); definite/possible leak stanzas need a named,
    path-identifying frame or the control's full chain."""
    frames = [l for l in body if l.startswith(("fun:", "obj:"))]
    kinds = next((l.split(":", 1)[1].strip() for l in body if l.startswith("match-leak-kinds:")), None)
    named = [f for f in frames if f.startswith("fun:") and not ALLOCATOR.match(f) and not GENERIC.match(f)]
    leak = body[0] == "Memcheck:Leak"
    kind = kinds if leak else body[0].split(":", 1)[1]
    if not leak:
        # Error records (invalid access, uninitialised value, syscall parameter) only
        # with the control's full chain: a cut inside the libraries can end at a
        # generic point (e.g. xcb flushing requests queued by any caller).
        allowed = cut in ("program", "complete")
    else:
        allowed = cut in ("program", "complete") or bool(named) or kinds == "indirect"
    return {"cut": cut, "named": bool(named), "kind": kind, "allowed": allowed}


def is_glib(body):
    """Stanzas with glib frames (object or g_* function) need the glib pin."""
    return any("libglib" in l or l.startswith("fun:g_") for l in body)


def normalise(line):
    # gen_supp.py writes versioned sonames as obj:*/libX.so.N*; older drafts wrote
    # the AppDir's obj:*/libX.so.N without the trailing wildcard.
    return line + "*" if re.fullmatch(r"obj:\*/[^*]+\.so\.\d+", line) else line


def blocks(text):
    for block in re.findall(r"\{\n(.*?)\n\}", text, re.S):
        lines = [normalise(l.strip()) for l in block.splitlines() if l.strip()]
        yield lines[0], tuple(lines[1:])


def main():
    out, glib_id, pin_file = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    first = sys.argv.index("--")
    second = sys.argv.index("--", first + 1)
    runs = sys.argv[4:first]
    groups = {"system": sys.argv[first + 1:second], "appdir": sys.argv[second + 1:]}
    out.mkdir(parents=True, exist_ok=False)

    candidates, cuts, widened = [], {}, set()
    for group, controls in groups.items():
        generated = subprocess.run([sys.executable, str(GEN), "qt-6.8.3-webengine", *controls],
                                   capture_output=True, text=True, check=True)
        (out / f"gen_supp-{group}.stderr").write_text(generated.stderr)
        for name, body in blocks(generated.stdout):
            if group == "system" and is_glib(body):
                continue  # system glib differs from the AppDir's: never verified there
            label, body = widen(body)
            if label:
                widened.add(label)
            cut = name.rsplit("-", 1)[1]
            if body not in candidates:
                candidates.append(body)
                cuts[body] = cut
            elif CUT_RANK[cut] < CUT_RANK[cuts[body]]:
                cuts[body] = cut

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import ledger
    nosupp = [r for r in runs if ":" not in r]
    credited_runs = [r.split(":", 1) for r in runs if ":" in r]
    # Records of the unsuppressed reports plus those left over in the runs with the
    # candidate files: both are real gate/fixture records.
    reports = list(dict.fromkeys(nosupp + [report for _, report in credited_runs]))
    records = [r for report in reports for r in ledger.records(report)[0]]
    credited = set()
    for supp, report in credited_runs:
        counts = {p.findtext("name"): int(p.findtext("count"))
                  for p in ET.parse(report).getroot().findall("suppcounts/pair")}
        credited.update(body for name, body in blocks(Path(supp).read_text()) if counts.get(name))
    scratch = out / "candidates.supp"
    scratch.write_text("".join("{\n   c-%d\n%s\n}\n" % (n, "\n".join("   " + l for l in b))
                               for n, b in enumerate(candidates)))
    parsed = ledger.stanzas(str(scratch))
    matched = {}
    for n, body in enumerate(candidates):
        matched[body] = sum(1 for r in records if ledger.stanza_matches(parsed[n], r))

    kept, pruned, rejected = [], [], []
    classes = {}
    for body in candidates:
        cls = classify(body, cuts[body])
        classes[body] = cls
        if not cls["allowed"]:
            rejected.append(body)
            continue
        if runs and matched[body] == 0 and body not in credited:  # no reports: first-run candidates
            pruned.append(body)
            continue
        kept.append(body)

    pins = json.loads(Path(pin_file).read_text())
    files = {"main": ([b for b in kept if not is_glib(b)], pins),
             "appdir-glib": ([b for b in kept if is_glib(b)],
                             dict(sorted({**pins, "libglib-2.0.so.0": glib_id}.items())))}
    from collections import Counter
    allowed = [b for b in candidates if classes[b]["allowed"]]
    parsed_by_body = {b: parsed[n] for n, b in enumerate(candidates)}
    open_records = [r for r in records
                    if not any(ledger.stanza_matches(parsed_by_body[b], r) for b in allowed)]
    report_open = sorted({(r["kind"], r["bytes"], next((fn for fn, o in r["frames"]
                                                         if ledger.PROGRAM.search(o)), "-"))
                          for r in open_records})
    def summary(body):
        return " ".join([next((l.split(":", 1)[1].strip() for l in body if l.startswith("match-leak-kinds:")),
                              body[0])] + [l for l in body if l.startswith(("fun:", "obj:"))][:4])

    report = {"widened_chains": sorted(widened), "verification_records": len(records),
              "rejected": [f"{cuts[b]} {summary(b)}" for b in rejected],
              "pruned": [f"{cuts[b]} {summary(b)}" for b in pruned], "open_records": len(open_records),
              "open": [list(x) for x in report_open],
              "candidates": len(candidates), "rejected_by_class": len(rejected),
              "pruned_zero_hits": len(pruned),
              "classes": {f"{c['cut']}/{'named' if c['named'] else 'obj-only'}/{c['kind']}": n
                          for c, n in sorted(Counter(tuple(sorted(classes[b].items())) for b in kept).items())
                          for c in [dict(c)]}}
    for label, (chosen, allowed) in files.items():
        objects = {m for b in chosen for l in b for m in re.findall(r"^obj:\*/([^*]+)\*?$", l)}
        wild = [b for b in chosen if any(re.fullmatch(r"obj:\*(/\*)?|fun:\*", l) for l in b)]
        unpinned = sorted(objects - set(allowed))
        report[label] = {"stanzas": len(chosen), "unpinned": unpinned, "wildcard_stanzas": len(wild)}
        if unpinned or wild:
            print(json.dumps(report, indent=2))
            sys.exit(3)
        prefix = "qt-6.8.3-webengine" + ("" if label == "main" else "-appdir-glib")
        # Name: <prefix>-<n>-<record kind>-<cut>-<named|objonly>
        names = ["%s-%03d-%s-%s-%s" % (prefix, n, classes[b]["kind"].replace(",", "+"), classes[b]["cut"],
                                      "named" if classes[b]["named"] else "objonly")
                 for n, b in enumerate(chosen, 1)]
        report.setdefault("kept_by_valgrind_credit_only", []).extend(
            name for name, b in zip(names, chosen) if runs and matched[b] == 0)
        text = "".join("{\n   %s\n%s\n}\n" % (name, "\n".join("   " + l for l in b))
                       for name, b in zip(names, chosen))
        (out / f"{label}.supp").write_text(text)
        (out / f"{label}.supp.buildids").write_text(json.dumps(allowed, indent=2) + "\n")
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
