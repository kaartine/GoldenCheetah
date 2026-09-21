# Workout Game Course Conversion Mode Contract

## Scope and priorities

This contract applies when a time-authored workout is explicitly converted to
a distance-authored MTB course. The source workout is immutable. Conversion is
deterministic from the normalized source intervals, FTP, mode, road-physics
parameters, conversion algorithm version, supported prescription-metadata
version and seed.

The workout prescription is authoritative. In decreasing priority, conversion
preserves the training stimulus, key efforts, recovery safety and only then
optimizes ride flow. Terrain, feature density, grade scale and the estimated
distance are the primary ways in which the modes differ. Route curvature also
increases deterministically from Calm training trail through Varied training
trail to Technical game trail,
while every result remains inside the common road-quality contract.

The current transform preserves every normalized source duration and start/end
target exactly, then fits distance, generated terrain effort and road geometry
around that immutable profile. A failed audit, road-plan validation,
road-quality audit or ETA causes conversion to fail closed; no artifact may be
saved.

## Source-data limitation and fail-safe metadata rule

`WorkoutGameInterval` currently contains only `startMs`, `durationMs`,
`startWatts` and `endWatts`. Those values cannot reliably distinguish an
authored recovery from a non-prescriptive warmup, cooldown or transition. In
particular, position as the first or last interval, low intensity, a ramp or a
generated `WorkoutGameFeature` classification is not evidence that time is
non-prescriptive.

Consequently, absent prescription metadata makes every source interval
prescribed. No current conversion mode may change the nominal duration of any
source interval. Runtime section boundaries are reached only by ridden
distance; elapsed time neither holds a rider before a boundary nor skips them
past one. Malformed, length-mismatched, unknown or unsupported metadata fails
conversion closed. Schema 3 introduced
`source.prescriptionMetadata`; its version 1 format accepts one explicit role
per source interval:

- `prescribed` (the default, including ordinary recoveries);
- `non-prescriptive-warmup`;
- `non-prescriptive-cooldown`; or
- `non-prescriptive-transition`.

Schemas 3--5 used the final three roles to authorize bounded duration changes.
They remain readable for compatibility, but conversion algorithm 6 does not
perform those changes. Metadata is still an authorization boundary, not a
hint: intensity or generated terrain may never promote `prescribed` to a
non-prescriptive role.

Role validation is fail-closed. A non-prescriptive warmup must be the first
interval, a non-prescriptive cooldown must be the last, and a non-prescriptive
transition must be non-recovery and non-key. In particular, metadata that labels
an ordinary low-power recovery as a transition is invalid rather than an
authorization to shorten it.

## Prescription definitions and audit metrics

An interval's intensity is its linearly averaged target power divided by FTP.
A **recovery** has average intensity at or below `0.65 FTP`. An ordinary 5:00
low-power interval between efforts is a recovery, never a transition merely
because changing it would improve flow.

A non-recovery interval is a **key effort** when any of these is true:

- duration is at least 120 seconds and intensity is at least `0.85 FTP`;
- duration is at least 20 seconds and intensity is at least `1.05 FTP`; or
- duration is at least 10 seconds and intensity is at least `1.20 FTP`.

Estimated load is named `load points`, not TSS. For a linear ramp from `P0` to
`P1`, its contribution is:

```
100 * duration_hours * (P0^2 + P0*P1 + P1^2) / (3 * FTP^2)
```

Work, recovery and total-duration deviations compare aggregate generated
nominal duration with the corresponding aggregate source duration. Load
deviation compares generated load points with source load points. Percentages
are signed; negative means shorter or lighter. Per-interval retention is
`generated nominal duration / source duration`.

## Measurable prescription guarantees

| Guarantee | Calm training trail | Varied training trail | Technical game trail |
| --- | ---: | ---: | ---: |
| Every start/end target-power error | 0 W | 0 W | 0 W |
| Every key-effort duration error | 0 ms | 0 ms | 0 ms |
| Every prescribed recovery duration error | 0 ms | 0 ms | 0 ms |
| Unannotated interval duration error | 0 ms | 0 ms | 0 ms |
| Explicit non-prescriptive part duration change | 0% | 0% | 0% |
| Aggregate work-duration deviation | 0% | 0% | 0% |
| Aggregate recovery-duration deviation | 0% | 0% | 0% |
| Total nominal-duration deviation | 0% | 0% | 0% |
| Absolute load deviation | 0% | 0% | 0% |
| Runtime progression | ridden distance only | ridden distance only | ridden distance only |

All three presets copy the nominal duration and start/end watts of every source
interval exactly. They differ only in distance, effort variation, elevation,
road shape, technical density and feature choice. Current documents keep the
legacy `minimumDurationMs` and `maximumDurationMs` fields equal to nominal
duration for compatibility; playback ignores all three as progression gates.

Course position, active section, ramp progress, generated terrain target,
trainer target lookup, cues and finish state all derive from the same clamped
course distance. If raw distance does not increase, elapsed time cannot advance
the route. If physics produces coasting distance it can advance the route,
including under downhill gravity.

Preview ETA simulates continuous riding at 85%, 100% and 115% of the generated
reference-effort profile. The three estimates normally differ and are not
forced to equal authored duration. They do not predict stops; stopping extends
the actual ride because the route remains unfinished.

## Measurable terrain and feature guarantees

The current generator supplies the mode anchors `gradeScale` 0.70/1.00/1.30 and
`technicality` 0.10/0.55/0.95 and route-turn scales 1.00/1.60/3.20. The low
(`<= 0.25`), middle and high (`>= 0.85`) palette branches define the terrain
bands below. The generated effort profile uses a grade envelope of
`[-3%, +12%]`, at most 85 degrees per non-berm road piece, no more than
25 m near-straight, at least three alternating deliberate bends and at least 45
degrees accumulated turn in every ordinary 100 m window, plus a 75--85 degree
turn at least every 400 ordinary metres.

`Technical terrain exposure` is the length-weighted share of palette-eligible
road distance whose terrain is marked technical by `WorkoutGameFeatureCatalog`.
Palette-eligible means a section classified as `WarmupTrail`, `Trail` or
`FlowTrail` before applying the mode palette. Prescribed recoveries, authored
climbs, sprint challenges and safety-exempt challenge branches are excluded
from its denominator and audited separately. `Technical feature density` is
the count of those technical sections per ten palette-eligible sections. Its
1--3 / 5--7 / 8--10 bands deliberately keep all three modes playful while
providing increasing technical intensity and variety. A fully recovery-only
workout is SmoothTrail in every mode and never gains technical terrain or a
scored challenge. A workout with fewer than two palette-eligible work sections
does not have enough granularity for three honest exposure levels; it also uses
the safe smooth fallback and reports exposure as `N/A`.

| Terrain/flow metric | Calm training trail | Varied training trail | Technical game trail |
| --- | ---: | ---: | ---: |
| `gradeScale` | 0.70 | 1.00 | 1.30 |
| `technicality` | 0.10 | 0.55 | 0.95 |
| deterministic route-turn scale | 1.00 | 1.60 | 3.20 |
| Palette-eligible technical terrain exposure target | 10--30% | 50--75% | 75--100% |
| Technical feature density | 1--3 / 10 sections | 5--7 / 10 sections | 8--10 / 10 sections |
| Palette and effort-semantic features | roots and rollers; falling effort may become a drop and rising effort rollers | roots, rollers, rock garden, log-over and skinny, with effort-aligned drops, slabs or tabletops | dense technical palette with effort-aligned drops, slabs, climbs, tabletops and gated gap jumps |
| Scored challenge on a suitable prescribed work/key-effort section | allowed without changing source start/end power or nominal duration | allowed without changing source start/end power or nominal duration | allowed without changing source start/end power or nominal duration |
| Scored challenge on a prescribed recovery | never | never | never |

Calm training trail is not a no-game mode: roots and rollers are available
inside its lower exposure and density bands. A
scored challenge may be attached to a suitable work or key-effort section in
any mode, provided it does not change the section's start/end target power,
nominal duration or any other prescription guarantee.
Prescribed recovery never receives a scored challenge in any mode. Varied
training trail and Technical game trail increase technical difficulty and
variety through the existing safe feature catalog; their distinction from
Calm training trail is technical intensity, density, `gradeScale` and flow,
not game versus no-game.

For the same source and seed with at least two palette-eligible work sections,
both technical-section density and distance-weighted technical exposure must
strictly increase. The generator constructs nested technical sets: Calm is a
subset of Varied and Varied is a subset of Technical. Within those density
counts it greedily selects the next section by generated distance toward the
35/60/90 percent targets, with a stable seed-derived tie break. Consequently a
long section cannot reverse the ordering. A generator unable to preserve the
strict ordering and the common safety envelope fails closed instead of changing
the prescription.

Long source intervals are subdivided into visual sections at the configured
variation wavelength without changing their aggregate duration or target
endpoints. The discrete section boundaries can still make an exact exposure
band mathematically unreachable; conversion preserves the workout and reports
the actual strictly ordered exposure.

The effort curve controls feature meaning as well as elevation. Falling effort
is suitable for a drop or jump landing, rising effort for an acceleration
approach, slab, tabletop or gap jump, and sustained high effort for a climb,
slab or rock garden. Recovery remains smooth and unscored regardless of the
palette. A climb shorter than its safe canonical geometry is downgraded rather
than allowed to lengthen the fixed course.

Gap jumps remain Technical-game-trail-only and deterministic. They retain the
existing safe line, power, geometry and road-quality gates. A recovery section
never becomes a jump. Every complete plan must pass
`WorkoutGameRoadPlanValidator` and `WorkoutGameRoadQuality`; fast, nominal and
slow estimates must all finish.

## Preview and persistence contract

Before saving, the dialog computes all three modes from the same immutable
source and stable seed. A compact three-row comparison is visible above the
long selected-mode details at 900x700 and shows:

- nominal duration;
- estimated distance;
- ascent and technical terrain exposure; and
- preserved/total hard and easy segments.

The selected-mode details retain load and duration deviations, generation
anchors, feature count and density, terrain-effort variation percentage and
variation wavelength. Current conversion reports no per-interval duration
changes.
`Hard segment` and `easy segment` are deliberately used in the UI because the
underlying ERG point-to-point segments are not authored workout step groups.

The dialog states once that all three presets create a fixed-distance route,
reference gear 6 follows generated terrain effort, virtual gears change
resistance, and elapsed time alone never advances the route. Primary labels use
`Workout first`, `Balanced`, and `Ride first`. A secondary detail line retains
the 0.70/1.00/1.30 grade scales, 1.00/1.60/3.20 curvature scales and
1--3/5--7/8--10 technical sections per ten eligible sections.

Hard- and easy-segment retention must never be inferred only from aggregate
work/rest percentages: both preserved/total counts are first-class preview
values in the selected-mode detail and in every all-three comparison row.

The preview keeps the immutable original workout power profile on a time axis,
so interval width always represents authored duration. Generated elevation is a
separate distance-axis chart. Selecting a mode changes only that terrain chart,
the terrain/flow summary and detailed ETA. Switching away and back reproduces
identical course, summary and
road-plan bytes. Mode selection and preview generation are side-effect free:
neither the course nor its sidecar exists or changes before the user invokes
Create/Save. Create/Save persists the already-previewed deterministic result,
apart from user-edited title/path metadata.

New or explicitly regenerated documents use schema version 7 and conversion
algorithm version 6. They store terrain-effort parameters, reference effort at
section endpoints, source annotations, the distance-authored road plan and its
bounded resolved asset-physics snapshot. Current conversion does not calculate
or persist a source-content hash. Schema 6 introduced that behavior, and schema
7 retains it: normalized source intervals are the regeneration input. Schemas
1 through 6 remain readable;
the schema 1 through 5 legacy SHA-256 field is validated when present, and an
explicit save upgrades to schema 7 without carrying the hash forward.

CRS export maps source lap and timed-text positions onto generated course
distance. During a game ride, targets and cues follow the same distance-derived
nominal source position as the route, so a stopped rider cannot advance either
by waiting. Unknown schema, algorithm or prescription-metadata versions fail
closed.

## Verification

Run the static contract suite without writing Python bytecode:

```
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s unittests/Build/courseConversionContracts -p 'test*.py' -v
```

The static suite verifies that this design contract, fixture and production APIs
remain aligned. The corresponding C++/Qt suites exercise conversion,
persistence, source adaptation, dialog behavior and runtime playback.

ETA uses bounded fixed physics steps and clamps its final distance at the course
finish. Runtime playback independently clamps raw distance and resolves every
section boundary from that distance; it contains no minimum/maximum exposure
transition path.
