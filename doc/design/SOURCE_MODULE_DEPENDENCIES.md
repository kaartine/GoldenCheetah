# Source module dependency baseline

GoldenCheetah currently builds its primary C and C++ source directories with
shared include paths. The resulting direct-include graph is cyclic. The source
dependency check prevents that known debt from changing without review; it
does not describe the current graph as an approved layering policy.

## Scope

The check scans the authored source union in these first-party modules:

`ANT`, `Charts`, `Cloud`, `Core`, `FileIO`, `Gui`, `Metrics`, `Planning`,
`Python`, `R`, and `Train`.

It deliberately ignores the active qmake configuration so platform and
optional-feature branches are checked together. Common C/C++ header, source,
inline, parser, and lexer suffixes are scanned case-insensitively. Third-party
trees, resources, local qmake injections, build outputs, generated parser
products, and generated SIP products are outside the graph. Authored parser
forwarding headers and the authored Python SIP bridge files remain in scope.

## Resolution and fail-closed rules

Literal `#include` and `#import` directives resolve in compiler-like order: an
including file's directory for quoted operands, an explicit source-module path,
the operand's exact path below a module root, and then (only for a one-component
operand) a unique authored header basename. Ambiguous first-party names,
wrong-case first-party matches, unsafe paths, symlinked inputs, invalid source
encoding, and unreviewed macro includes fail the check. Known generated parser
and SIP headers are represented explicitly rather than read from a build
directory. Unresolved quoted includes are also explicit baseline records;
unresolved angle includes without a case-folded first-party match are treated
as external toolchain or library headers.

The baseline stores each exact cross-module source-file-to-target-header edge,
not merely a module pair or a count. Consequently, adding a new include fails
even when the two modules are already mutually dependent. Removing an include
also requires removing its stale baseline entry in the same reviewed change.

## Developer workflow

Run the blocking comparison from the repository root:

```sh
python3 util/source_module_dependencies.py \
  --repository . \
  --baseline doc/design/source-module-dependencies.baseline \
  --check
```

After intentionally changing a direct dependency, inspect the candidate before
updating anything:

```sh
python3 util/source_module_dependencies.py --repository . --print-candidate
```

Only after reviewing the directed edge and its architectural effect, regenerate
the canonical baseline:

```sh
python3 util/source_module_dependencies.py \
  --repository . \
  --baseline doc/design/source-module-dependencies.baseline \
  --write-baseline
```

The required CI auxiliary test exercises the analyzer with synthetic fixtures
and compares the live source graph with the checked-in baseline.

## Residual work

This gate observes direct textual includes, not runtime calls, Qt signal flow,
linker dependencies, generated code internals, or dynamically selected headers.
The `GC_ANT_LIBUSB_HEADER` macro remains one reviewed opaque input. The next
architecture step is to define an intended acyclic module/target policy, then
remove baseline edges one behavior-protected vertical seam at a time. Until
that policy exists, the baseline is a debt inventory and change-control gate,
not evidence that ARCH-002 is fully resolved.
