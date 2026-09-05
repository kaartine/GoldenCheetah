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
increases deterministically from Workout first through Balanced to Ride first,
while every result remains inside the common road-quality contract.

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
prescribed. No conversion mode may shorten the nominal duration of any such
interval. Runtime progression may reach a section boundary only after that
mode's separately declared minimum exposure. Malformed, length-mismatched,
unknown or unsupported metadata fails conversion closed. Schema 3 introduced
`source.prescriptionMetadata`; its version 1 format accepts one explicit role
per source interval:

- `prescribed` (the default, including ordinary recoveries);
- `non-prescriptive-warmup`;
- `non-prescriptive-cooldown`; or
- `non-prescriptive-transition`.

Only the final three roles authorize a duration adjustment. Metadata is an
authorization boundary, not a hint: intensity or generated terrain may never
promote `prescribed` to a non-prescriptive role. This task specifies that
metadata contract and its tests enforce it in production code.

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
| Every prescribed recovery duration error | 0 ms | 0 ms | 0 ms |
| Unannotated interval duration error | 0 ms | 0 ms | 0 ms |
| Explicit non-prescriptive part, per-interval absolute duration change | 0% | <= 3% | <= 8% |
| Aggregate work-duration deviation | 0% | <= 3% absolute | <= 8% absolute |
| Aggregate recovery-duration deviation | 0% | 0% | -5% to +8% |
| Total nominal-duration deviation | 0% | <= 3% absolute | <= 8% absolute |
| Absolute load deviation | 0% | <= 3% | <= 8% |
| Minimum runtime key-effort exposure | 100% | 100% | 100% |
| Minimum runtime prescribed work exposure | 100% | 100% | 100% |
| Minimum runtime prescribed recovery exposure | 100% | 100% | 100% |

Workout first copies the duration and start/end watts of every interval exactly
(0 ms and 0 W difference). Its runtime minimum exposure for each section is
also the source duration; terrain must fit the prescription.

Balanced copies every prescribed interval exactly. It may
adjust only an explicitly annotated non-prescriptive warmup, cooldown or
transition, by at most 3% in either direction. Ordinary recovery must not be
treated as a transition. Every adjustment and its metadata role is reported in
the preview.

Ride first also copies every prescribed interval exactly. It can change only an
explicitly annotated non-prescriptive warmup, cooldown or transition, and total
nominal duration remains within 8% of the source. Compactness, target distance
and trail flow never authorize changing a prescribed interval. Stimulus, key
efforts and recovery safety take precedence over flow.

All allowed scaling is rounded once to integer milliseconds. The converter
backs an authorized change off deterministically when any aggregate limit would
otherwise be exceeded. It never changes a key effort or an unannotated part to
satisfy a terrain, distance, load or duration target.

Runtime progression caps each raw trainer-distance increment with a continuous
time-derived bound measured from active movement in the current section.
Reaching a section's distance early cannot finish it before its minimum
exposure. Rejected excess distance is not banked for later. Stationary time does
not satisfy the exposure gate or push the rider forward. A moving rider may use
the interval between the minimum and maximum exposure to finish the distance;
at the maximum, runtime advances without banking rejected trainer distance.
Trainer slope lookup and game position use that same bounded course distance,
so the display and trainer cannot disagree about the active section. Paused
time is excluded by the training session clock.

Within a ramp, target power follows active section time rather than distance.
The rider can therefore move slowly without stretching the ramp's opening
target across the whole section. Course position remains distance-driven; only
the prescribed target timeline is time-driven. The same target is published to
the training data generator and Workout Game telemetry/HUD.

Preview ETA assumes continuous active riding at 85%, 100% or 115% of the
prescribed target and applies the same maximum-section transition as runtime.
For an entirely prescribed workout, all three estimates equal its authored
duration. It does not predict stops; stationary time freezes runtime progress
and therefore extends the actual ride beyond the preview estimate.

Every prescribed interval has 100% minimum runtime exposure in every mode.
Only explicitly versioned non-prescriptive roles may use a lower mode-specific
minimum in a future transform.

`maximumDurationMs` is a bounded active-riding envelope, not permission to move
a stationary rider. When a moving rider reaches the maximum, playback advances
deterministically to the next section (or finishes the course), reports that
forced transition for the update and discards rejected trainer distance. Any
active-time overrun is carried into the next section so a delayed update cannot
extend a prescribed target without bound. This changes neither recorded data
nor the immutable source prescription.

## Measurable terrain and feature guarantees

The current generator supplies the mode anchors `gradeScale` 0.82/1.00/1.18 and
`technicality` 0.15/0.55/0.95 and route-turn scales 1.00/1.30/2.60. The low
(`<= 0.25`), middle and high (`>= 0.85`) palette branches define the terrain
bands below. The common safety envelope is
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
providing increasing technical intensity and variety. A mode with no
palette-eligible distance reports exposure and density as `N/A`, not zero.

| Terrain/flow metric | Workout first | Balanced | Ride first |
| --- | ---: | ---: | ---: |
| `gradeScale` | 0.82 | 1.00 | 1.18 |
| `technicality` | 0.15 | 0.55 | 0.95 |
| deterministic route-turn scale | 1.00 | 1.30 | 2.60 |
| Palette-eligible technical terrain exposure target | 25--45% | 50--75% | 75--100% |
| Technical feature density | 2--4 / 10 sections | 5--7 / 10 sections | 8--10 / 10 sections |
| Palette | roots, rollers, easy rock garden and log-over mixed with smooth trail; climbs retained; no gap jump | roots, rollers, rock garden, log-over and skinny mixed with smooth trail; no gap jump | skinny, rock garden or rock slab trail; berm recovery; log-over, tabletop or gated gap jump sprint |
| Scored challenge on a suitable prescribed work/key-effort section | allowed without changing start/end power, interval time or minimum exposure | allowed without changing start/end power, interval time or minimum exposure | allowed without changing start/end power, interval time or minimum exposure |
| Scored challenge on a prescribed recovery | never | never | never |

Workout first is not a no-game mode: roots, rollers, an easy rock garden and a
log-over are available inside its lower exposure and density bands. A scored
challenge may be attached to a suitable work or key-effort section in any mode,
provided it does not change the section's start/end target power, interval
duration, minimum exposure or any other prescription guarantee. Prescribed
recovery never receives a scored challenge in any mode. Balanced and Ride first
increase technical difficulty and variety through the existing safe feature
catalog; their distinction from Workout first is technical intensity, density,
`gradeScale` and flow, not game versus no-game.

For the same source and seed, technical-section density must strictly increase.
Distance-weighted exposure normally increases too. If a short, highly uneven or
structurally uniform workout makes an
aggregate band inapplicable, the preview still has to show a different terrain
signature and the exact grade/technicality anchors. A generator unable to meet
both the technical-section density band and the common safety envelope fails
closed instead of changing the prescription.

Technical-section density is the enforceable generation contract. Exposure is
also shown as a distance-weighted diagnostic, but highly unequal source
intervals can make its target band mathematically unreachable without changing
the workout. In that case conversion preserves the workout and reports the
actual exposure instead of failing or silently changing an interval.

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
- preserved/total key efforts and preserved/total recoveries as separate,
  explicit retention values;
- work-duration and recovery/rest-duration deviation as separate values;
- grade scale, elevation/terrain signature, actual road-plan bend count,
  accumulated turn and technical terrain exposure;
- feature count and density; and
- every per-interval duration change, its signed amount and authorized metadata
  role (or an explicit `no prescription changes` result).

Key-effort and recovery retention must never be inferred only from aggregate
work/rest percentages: both preserved/total counts are first-class preview
values in the selected-mode detail and in every all-three comparison row.

Selecting a mode changes the elevation/power preview, terrain/flow summary and
detailed ETA. Switching away and back reproduces identical course, summary and
road-plan bytes. Mode selection and preview generation are side-effect free:
neither the course nor its sidecar exists or changes before the user invokes
Create/Save. Create/Save persists the already-previewed deterministic result,
apart from user-edited title/path metadata.

New or explicitly regenerated documents use schema version 4 and conversion
algorithm version 3, which identifies preset-scaled route curvature. They may
also record prescription metadata version 1.
Schema 4 also stores the original workout's ordered lap markers and timed text
instructions; CRS export maps their source times onto generated course distance
without changing target power or section timing. During a game ride, lap and
text-cue lookup follows the active workout timeline rather than a slow rider's
visual distance, so the instruction and its target remain aligned. Schema 1
through 3 documents
remain readable and canonical: loading never regenerates or silently adds
metadata, and their missing metadata invokes the fail-safe prescribed role for
every interval. An explicit save may upgrade the container while preserving
the legacy conversion-algorithm identity and the selected mode's route shape.
Unknown schema, algorithm or prescription-metadata versions fail closed.

## Verification

Run the static contract suite without writing Python bytecode:

```
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s unittests/Build/courseConversionContracts -p 'test*.py' -v
```

The static suite verifies that this design contract, fixture and production APIs
remain aligned. The corresponding C++/Qt suites exercise conversion,
persistence, source adaptation, dialog behavior and runtime playback.
