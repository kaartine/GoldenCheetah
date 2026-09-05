# Workout Game Course Conversion Mode Contract

## Scope and priorities

This contract applies when a time-authored workout is explicitly converted to
a distance-authored MTB course. The source workout is immutable. Conversion is
deterministic from the normalized source intervals, FTP, mode, road-physics
parameters, conversion algorithm version, supported prescription-metadata
version and seed.

The workout prescription is authoritative. In decreasing priority, conversion
preserves the training stimulus, key efforts, recovery safety and only then
optimizes ride flow. Terrain, feature density, curvature, connectors and the
estimated distance are the primary ways in which the modes differ.

The prescription transform audits every duration or power change before the
distance-course builder fits terrain and distance around it. A failed audit,
road-plan validation, road-quality audit or ETA causes conversion to fail
closed; no artifact may be saved.

## Source-data limitation and fail-safe metadata rule

`WorkoutGameInterval` currently contains only `startMs`, `durationMs`,
`startWatts` and `endWatts`. Those values cannot reliably distinguish an
authored recovery from a non-prescriptive warmup, cooldown or transition. In
particular, position as the first or last interval, low intensity, a ramp or a
generated `WorkoutGameFeature` classification is not evidence that time is
non-prescriptive.

Consequently, absent prescription metadata makes every source interval
prescribed. No mode may automatically shorten any such interval. Malformed,
length-mismatched, unknown or unsupported metadata fails conversion closed. A
later production implementation may accept schema-3
`source.prescriptionMetadata` version 1 with one explicit role per source
interval:

- `prescribed` (the default, including ordinary recoveries);
- `non-prescriptive-warmup`;
- `non-prescriptive-cooldown`; or
- `non-prescriptive-transition`.

Only the final three roles authorize a duration adjustment. Metadata is an
authorization boundary, not a hint: intensity or generated terrain may never
promote `prescribed` to a non-prescriptive role. This task specifies that
metadata contract and its tests but does not add it to production code.

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

| Guarantee | Workout first | Balanced | Ride first |
| --- | ---: | ---: | ---: |
| Every start/end target-power error | 0 W | 0 W | 0 W |
| Every key-effort duration error | 0 ms | 0 ms | 0 ms |
| Every ordinary recovery duration error | 0 ms | 0 ms | default 0 ms; retention >= 95% |
| Unannotated interval duration error | 0 ms | 0 ms | 0 ms |
| Explicit non-prescriptive part, per-interval absolute duration change | 0% | <= 3% | <= 8% |
| Aggregate work-duration deviation | 0% | <= 3% absolute | <= 8% absolute |
| Aggregate recovery-duration deviation | 0% | 0% | -5% to +8% |
| Total nominal-duration deviation | 0% | <= 3% absolute | <= 8% absolute |
| Absolute load deviation | 0% | <= 3% | <= 8% |

Workout first copies the duration and start/end watts of every interval exactly
(0 ms and 0 W difference). Its runtime minimum and maximum exposure for each
section are also the source duration; terrain must fit the prescription.

Balanced copies every key effort and every ordinary recovery exactly. It may
adjust only an explicitly annotated non-prescriptive warmup, cooldown or
transition, by at most 3% in either direction. Ordinary recovery must not be
treated as a transition. Every adjustment and its metadata role is reported in
the preview.

Ride first copies every key effort exactly. Every ordinary recovery defaults to
100% retention or extension. Recovery may fall below 100% only when required
for a validated physical or safety constraint after preservation and extension
were attempted; compactness, target distance and flow are not sufficient
reasons. It never falls below 95%. Flow may change only an explicitly annotated
non-critical transition, and total nominal duration remains within 8% of the
source. Stimulus, key efforts and recovery safety take precedence over flow.

All allowed scaling is rounded once to integer milliseconds. The converter
backs an authorized change off deterministically when any aggregate limit would
otherwise be exceeded. It never changes a key effort or an unannotated part to
satisfy a terrain, distance, load or duration target.

## Measurable terrain, feature and curvature guarantees

The current generator supplies the mode anchors `gradeScale` 0.82/1.00/1.18 and
`technicality` 0.15/0.55/0.95. The low (`<= 0.25`), middle and high (`>= 0.85`)
palette branches define the terrain bands below. The common safety envelope is
grade `[-12%, +12%]`, at most 85 degrees per non-berm road piece, no more than
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
2--4 / 5--7 / 8--10 bands deliberately keep all three modes playful while
providing increasing technical intensity and variety. `Curvature` is accumulated
absolute heading change in degrees per 100 ordinary metres, excluding the same
quality-exempt branches. A mode with no palette-eligible distance reports
exposure and density as `N/A`, not zero.

| Terrain/flow metric | Workout first | Balanced | Ride first |
| --- | ---: | ---: | ---: |
| `gradeScale` | 0.82 | 1.00 | 1.18 |
| `technicality` | 0.15 | 0.55 | 0.95 |
| Palette-eligible technical terrain exposure | 25--45% | 50--75% | 75--100% |
| Technical feature density | 2--4 / 10 sections | 5--7 / 10 sections | 8--10 / 10 sections |
| Palette | roots, rollers, easy rock garden and log-over mixed with smooth trail; climbs retained; no gap jump | roots, rollers, rock garden, log-over and skinny mixed with smooth trail; occasional recovery berm; no gap jump | skinny, rock garden or rock slab trail; berm recovery; log-over, tabletop or gated gap jump sprint |
| Scored challenge on a suitable prescribed work/key-effort section | allowed without changing start/end power, interval time or minimum exposure | allowed without changing start/end power, interval time or minimum exposure | allowed without changing start/end power, interval time or minimum exposure |
| Scored challenge on a prescribed recovery | never | never | never |
| Curvature target | 45--75 deg/100 m | 60--120 deg/100 m | 75--170 deg/100 m |

Workout first is not a no-game mode: roots, rollers, an easy rock garden and a
log-over are available inside its lower exposure and density bands. A scored
challenge may be attached to a suitable work or key-effort section in any mode,
provided it does not change the section's start/end target power, interval
duration, minimum exposure or any other prescription guarantee. Prescribed
recovery never receives a scored challenge in any mode. Balanced and Ride first
increase technical difficulty and variety through the existing safe feature
catalog; their distinction from Workout first is technical intensity, density,
curvature, `gradeScale` and flow, not game versus no-game.

For the same source and seed, the accepted curvature values must satisfy
`Workout first < Balanced < Ride first`; adjacent modes differ by at least 15
degrees/100 m. On inputs where a metric is applicable, technical exposure must
also strictly increase. If a short or structurally uniform workout makes an
aggregate band inapplicable, the preview still has to show a different terrain
signature and the exact grade/technicality anchors. A generator unable to meet
both the mode band and the common safety envelope fails closed instead of
changing the prescription.

Gap jumps remain Ride-first-only and deterministic. They retain the existing
safe line, power, geometry and road-quality gates. A recovery section never
becomes a jump. Every complete plan must pass `WorkoutGameRoadPlanValidator`
and `WorkoutGameRoadQuality`; fast, nominal and slow estimates must all finish.

## Preview and persistence contract

Before saving, the dialog computes all three modes from the same immutable
source and stable seed. Each comparison row shows:

- nominal duration and total-duration deviation;
- estimated distance;
- estimated load points and load deviation;
- work-duration and recovery-duration deviation;
- grade scale, elevation/terrain signature and technical terrain exposure;
- feature count/density and curvature; and
- every per-interval duration change, its signed amount and authorized metadata
  role (or an explicit `no prescription changes` result).

Selecting a mode changes the elevation/power preview, terrain/flow summary and
detailed ETA. Switching away and back reproduces identical course, summary and
road-plan bytes. Mode selection and preview generation are side-effect free:
neither the course nor its sidecar exists or changes before the user invokes
Create/Save. Create/Save persists the already-previewed deterministic result,
apart from user-edited title/path metadata.

New or explicitly regenerated documents use schema version 3, record the
conversion algorithm version and may record prescription metadata version 1.
Schema-1 and schema-2 documents remain readable and canonical: loading never
regenerates or silently adds metadata, and their missing metadata invokes the
fail-safe prescribed role for every interval. An explicit save may upgrade the
container while preserving the legacy conversion-algorithm identity. Unknown
schema, algorithm or prescription-metadata versions fail closed.

## Test-first RED baseline

Run the static contract suite without writing Python bytecode:

```
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s unittests/Build/courseConversionContracts -p 'test*.py' -v
```

At this contract-only commit, five tests pass and the single expected test-first
failure is:

```
missing production contract/header/API:
src/Train/WorkoutGameCoursePrescription.h
```

The failure records the deliberately absent production contract, metadata,
summary and persistence APIs. The C++/Qt contract tests describe those later
APIs but are intentionally not built or run in this contract-only phase.
