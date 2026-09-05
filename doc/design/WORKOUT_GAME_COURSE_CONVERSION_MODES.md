# Workout Game Course Conversion Mode Contract

## Scope

This contract applies when a time-authored workout is explicitly converted to
a distance-authored MTB course. The source workout remains immutable. The
conversion is deterministic from the normalized source intervals, FTP, mode,
road-physics parameters, conversion algorithm version and seed.

The conversion separates two concerns:

- the prescription transform decides which source durations may change and
  audits training load, key efforts and recovery;
- the distance-course builder fits deterministic terrain and road distance to
  that audited output prescription without changing its power targets.

Loading an existing course never regenerates it. Regeneration is an explicit
edit action and uses the algorithm version current at that time. Schema 1 and 2
documents remain readable; newly generated or explicitly re-saved documents
record both schema version 3 and the conversion algorithm version.

## Definitions

An interval's intensity is its linearly averaged target power divided by FTP.

A **recovery** has average intensity at or below `0.65 FTP`.

A non-recovery interval is a **key effort** when any of these is true:

- duration is at least 120 seconds and intensity is at least `0.85 FTP`;
- duration is at least 20 seconds and intensity is at least `1.05 FTP`; or
- duration is at least 10 seconds and intensity is at least `1.20 FTP`.

Estimated load is deliberately named `load points`, not TSS. For a linear ramp
from `P0` to `P1`, its contribution is:

```
100 * duration_hours * (P0^2 + P0*P1 + P1^2) / (3 * FTP^2)
```

Work and recovery deviation compare aggregate generated nominal duration with
the corresponding aggregate source duration. Load deviation compares generated
load points with source load points. Reported percentages are signed; negative
means the generated course is shorter or lighter.

## Measurable Mode Guarantees

| Guarantee | Workout first | Balanced | Ride first |
| --- | ---: | ---: | ---: |
| Generated key-effort duration error | 0 ms | 0 ms | 0 ms |
| Generated target-power error | 0 W | 0 W | 0 W |
| Aggregate work-duration deviation | <= 0.1% | <= 3% | <= 8% |
| Aggregate recovery-duration deviation | <= 0.1% | <= 8% | <= 15% |
| Total nominal-duration deviation | <= 0.1% | <= 8% | <= 15% |
| Absolute load deviation | <= 0.1% | <= 3% | <= 5% |
| Each recovery's generated nominal duration | >= 99.9% source | >= 92% source | >= 85% source |
| Each recovery's minimum ride exposure | >= 98% source | >= 80% source | >= 75% source |
| Key-effort minimum/maximum ride exposure | 99-101% source | 97-105% source | 95-105% source |

Workout first copies all source durations and powers exactly. Only terrain is
fitted around them; its near-zero tolerances are validation headroom.

Balanced copies every key effort, shortens recovery transitions nominally by
6%, and shortens other work/transition sections nominally by 2%. It may widen
safe timing branches, but its aggregate limits above are authoritative.

Ride first copies every key effort, shortens recovery transitions nominally by
15%, and shortens other work/transition sections nominally by 8%. If that would
reduce load by more than 5%, all non-key changes are proportionally backed off.
This produces more compact transitions for trail flow without sacrificing the
primary stimulus.

All duration scaling is rounded once to integer milliseconds. The converter
backs changes off deterministically if an aggregate tolerance would otherwise
be exceeded; it never changes a key effort to satisfy an aggregate limit.

## Safety Invariants

Every successful conversion must satisfy all of the following:

1. Intervals remain ordered and contiguous; no interval is inserted, removed or
   reordered.
2. Target start/end watts are finite, non-negative and bit-for-bit unchanged.
3. Every key effort retains its source nominal duration exactly.
4. Every recovery retains the per-interval nominal and minimum exposure floors
   in the table, including short recoveries.
5. Work, recovery, total-duration and load deviations remain within the chosen
   mode's limits after millisecond rounding.
6. Generated section grades remain finite and within `[-12%, +12%]`.
7. A recovery section never becomes a jump. Gap jumps remain Ride-first-only,
   deterministic, and retain the existing safe line and road-quality gates.
8. The complete road plan must pass `WorkoutGameRoadPlanValidator` and
   `WorkoutGameRoadQuality`; failure is closed and no artifact is saved.
9. Fast, nominal and slow estimates must all finish. Invalid inputs, resource
   limits, no-progress results and contract violations fail closed.

## Preview Contract

Before saving, the dialog computes all three modes from the same immutable
source and stable seed. A comparison row for each mode shows:

- nominal course duration;
- estimated distance;
- estimated load points and load deviation;
- aggregate work-duration deviation; and
- aggregate recovery-duration deviation.

Selecting a row/mode changes the elevation/power preview and detailed ETA.
Switching away and back must reproduce the exact course, summary and road plan.
Saving regenerates with the same inputs and must produce the same persisted
course bytes apart from user-edited title/path metadata.

