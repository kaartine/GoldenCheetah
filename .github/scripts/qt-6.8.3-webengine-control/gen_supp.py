#!/usr/bin/env python3
"""Turn Valgrind --gen-suppressions=all XML from pure-Qt control runs into
library-only suppression stanzas.

Each stanza is cut at the first frame whose object is the program itself
(control binary, GoldenCheetah or a tst_ fixture), so a stanza can only
describe allocations/errors that happen entirely inside libraries. A chain
without a program frame is "complete" if it ends at a thread or process root
(clone, start_thread, ...) and "truncated" otherwise (--num-callers); a
truncated chain is only a cut inside the libraries. Object
paths are reduced to the library basename with its version suffix
wildcarded, because the AppDir ships sonames while /opt/Qt ships full
versions; the exact builds are pinned separately by Build-ID.

A record whose chain runs through Qt's glib event dispatcher also yields a
stanza cut directly after QEventDispatcherGlib::processEvents: everything above
it is the program's event-loop driver (QCoreApplication::exec in the control, a
direct processEvents() call in the application), not the allocating chain. It is
kept only when the allocating/erroring frame is library code and at least
LIBRARY_MINIMUM path-identifying frames (not allocators, generic event delivery
or glib dispatch) lie below the cut.

A record additionally yields a shorter library-boundary stanza: the frames
from the top down to where the stack leaves the library that allocated (or
raised the error), i.e. the first frame in a different Qt library. It is kept
only when the allocating/erroring frame is library code and at least
LIBRARY_MINIMUM non-allocator frames lie before the boundary, so it describes
the library's own internal path but not which library called into it.
"""

import re
import sys
import xml.etree.ElementTree as ET
from collections import OrderedDict

PROGRAM_OBJECTS = re.compile(r"/(webengine_control|qcss_control|GoldenCheetah|tst_[A-Za-z]+)$")
ALLOCATOR = re.compile(r"^(malloc|calloc|realloc|memalign|posix_memalign|_Znwm|_Znam|_ZnwmRKSt9nothrow_t|strdup)$")
QT_LIBRARY = re.compile(r"/libQt6[A-Za-z]+\.so")
LIBRARY_MINIMUM = 7
BLOCKING = {"Leak_DefinitelyLost": "definite", "Leak_IndirectlyLost": "indirect",
            "Leak_PossiblyLost": "possible"}


def soname(obj):
    name = obj.rsplit("/", 1)[-1]
    match = re.match(r"(.+?\.so\.\d+)", name)
    return match.group(1) if match else name


def library_pattern(obj):
    # One form for /opt/Qt (libX.so.6.8.3) and the AppDir (libX.so.6); the exact
    # build is pinned by Build-ID, not by the file name.
    name = soname(obj)
    return f"obj:*/{name}*" if re.search(r"\.so\.\d+$", name) else f"obj:*/{name}"


def library_boundary(frames, objects):
    """Length of the library-boundary prefix of frames, or None."""
    origin = None
    counted = 0
    for index, line in enumerate(frames):
        obj = objects[index] if index < len(objects) else ""
        function = line[4:] if line.startswith("fun:") else ""
        if origin is None:
            if ALLOCATOR.match(function) or "vgpreload" in obj:
                continue
            origin = soname(obj)
        elif QT_LIBRARY.search(obj) and soname(obj) != origin:
            return index if counted >= LIBRARY_MINIMUM else None
        counted += 1
    return None  # never left the library before the program cut: no shorter stanza


GENERIC_DELIVERY = re.compile(
    r"^fun:(_ZN7QObject5eventEP6QEvent|_ZN19QApplicationPrivate13notify_helper|"
    r"_ZN16QCoreApplication15notifyInternal2|_ZN23QCoreApplicationPrivate16sendPostedEvents|"
    r"g_main_context_dispatch|g_main_context_iteration|_ZN20QEventDispatcherGlib13processEvents)")
THREAD_ROOTS = {"fun:clone", "fun:clone3", "fun:start_thread", "fun:__libc_start_main",
                "fun:__libc_start_call_main", "fun:_start"}
EVENT_DISPATCHER = "fun:_ZN20QEventDispatcherGlib13processEventsE6QFlagsIN10QEventLoop17ProcessEventsFlagEE"


def dispatcher_cut(frames):
    """Length of the prefix ending at QEventDispatcherGlib::processEvents, or None."""
    if EVENT_DISPATCHER not in frames:
        return None
    end = frames.index(EVENT_DISPATCHER) + 1
    # Only frames that identify the path count: not the allocator, not generic
    # event delivery or glib dispatch, not unnamed QtCore dispatch frames.
    body = [f for f in frames[:end] if not ALLOCATOR.match(f[4:]) and "vgpreload" not in f
            and not GENERIC_DELIVERY.match(f) and "libglib" not in f and "libQt6Core.so" not in f]
    return end if len(body) >= LIBRARY_MINIMUM else None


def stanzas(path):
    root = ET.parse(path).getroot()
    for error in root.findall("error"):
        kind = error.findtext("kind")
        if kind == "Leak_StillReachable":
            continue
        suppression = error.find("suppression")
        if suppression is None:
            continue
        stack = error.find("stack")
        objects = [frame.findtext("obj") or "" for frame in stack.findall("frame")]
        frames = []
        reached_program = False
        for index, sframe in enumerate(suppression.findall("sframe")):
            obj = objects[index] if index < len(objects) else ""
            if PROGRAM_OBJECTS.search(obj):
                reached_program = True
                break
            function = sframe.findtext("fun")
            if not function and not obj.startswith("/"):
                frames = None  # code outside any object (e.g. JIT): cannot be pinned
                break
            frames.append(f"fun:{function}" if function else library_pattern(obj))
        if frames is None:
            print(f"# rejected unpinnable {kind} record in {path}", file=sys.stderr)
            continue
        if not any(not ALLOCATOR.match(f[4:]) and "vgpreload" not in f for f in frames):
            continue  # allocates or errs in the program itself: never a library exception
        skind = suppression.findtext("skind")
        extra = suppression.findtext("skaux")
        body = [skind] + ([extra] if extra else [])
        if kind in BLOCKING and not any(line.startswith("match-leak-kinds:") for line in body):
            body.append(f"match-leak-kinds: {BLOCKING[kind]}")
        # A chain is complete if it reaches the program or a thread/process root;
        # otherwise it was truncated (--num-callers) and is only a library cut.
        if reached_program:
            full = "program"
        elif frames[-1] in THREAD_ROOTS:
            full = "complete"
        else:
            full = "truncated"
        yield kind, full, tuple(body + frames)
        cut = dispatcher_cut(frames)
        if cut is not None and cut < len(frames):
            yield kind, "dispatcher", tuple(body + frames[:cut])
        boundary = library_boundary(frames, objects)
        if boundary is not None:
            yield kind, "library", tuple(body + frames[:boundary])


CUT_RANK = {"program": 0, "complete": 0, "dispatcher": 1, "library": 2, "truncated": 3}


def main():
    prefix, paths = sys.argv[1], sys.argv[2:]
    unique = OrderedDict()
    for path in paths:
        for kind, cut, stanza in stanzas(path):
            # A body that is a full program cut for any record is labelled "program".
            if stanza not in unique or CUT_RANK[cut] < CUT_RANK[unique[stanza][1]]:
                unique[stanza] = (kind, cut)
    for number, (stanza, (kind, cut)) in enumerate(unique.items(), 1):
        print("{")
        print(f"   {prefix}-{number:03d}-{kind}-{cut}")
        for line in stanza:
            print(f"   {line}")
        print("}")
    print(f"# {len(unique)} unique stanzas from {len(paths)} reports", file=sys.stderr)


if __name__ == "__main__":
    main()
