# FTMS profiles and source mapping

The bridge implements Bluetooth Fitness Machine Service (FTMS 1.0, `0x1826`)
as both a client and a server. All integers below are little-endian. Signed
fields use two's complement; distances and Cross Trainer flags use three bytes.
Resolutions describe one encoded integer unit, not the internal float unit.

## Optional metrics and record lifetime

Every metric uses `MetricSample { value, valid }`. `valid == false` means **not
supplied**, not zero. Storage behind an invalid sample must not be consumed.
The legacy telemetry interface has explicit validity bits as well. A genuine
reported zero remains valid, including heart rate, speed, power, and distance.
Non-finite values are not encoded.

Decoding is transactional: a short packet invalidates the entire decoded result;
no partial record is published. A new complete record replaces the previous
record, clearing omitted fields. FTMS continuation fragments are assembled only
within a bounded record lifetime. Disconnect/reset and stale input clear metrics.
Vendor protocols set only fields decoded from their particular message layout;
complementary messages have bounded per-field freshness.

Encoders emit a field only when it is valid. Fields sharing one presence flag
are inseparable: **all** members must be present, otherwise the entire group is
omitted. In particular, no zero stroke count, average step rate, energy rate,
or belt force is invented to complete a group. The mapping layer may convert
a present inclination to its geometric ramp angle (`atan(grade/100)` in degrees)
for treadmill/cross-trainer output, preserving existing treadmill behavior;
the source sample still distinguishes a reported angle from an absent angle.

Flag bit 0 is inverted: `0` includes the base group, `1` omits it. The base is
speed for bike/treadmill/cross trainer, stroke rate **and** count for rowers,
floors **and** step count for step climbers, and floors alone for stair climbers.
Large output records are split into ATT-MTU-sized notifications without splitting
a flagged group; only the fragment carrying the base group clears bit 0.

No missing energy, distance, elapsed time, average, or count is synthesized.
Conversions operate on present source values only. Rower pace/speed conversion
requires a positive divisor (`speed_kmh = 1800 / pace_seconds_per_500m`);
zero is retained as a measured value, not divided into. Treadmill pace is a
different unit and must not be interpreted as rowing pace. Geometric ramp-angle
conversion also runs only on a valid inclination, including a real zero grade.

Web/API missing values are JSON `null` and the dashboard displays “–”.
Availability is independent of the source's capabilities: supporting power
does not imply the current notification contained power.

## Automatic and manual profile selection

| Source device/protocol | Automatic output | Field mapping |
|---|---|---|
| FTMS `ftms` | Profile selected from subscribed data characteristic / unambiguous Service Data | Every flagged field retained in normalized units |
| `schwinn-ic4` | Indoor Bike | Read-only Indoor Bike Data; no extra controls |
| `echelon`, `domyos`, `yesoul`, `keiser-m3i` | Indoor Bike | Decoded speed, cadence, power, HR, resistance; documented protocol estimates require present inputs |
| `ifit-bike`, `schwinn-170`, iFit bike manual profiles | Indoor Bike | Only fields in the selected firmware layout |
| `ifit-freestride`, `ifit-elliptical`, `ifit-el-trainer`, iFit elliptical manual profiles | Cross Trainer | Elliptical cadence → steps/min; profile-specific speed, inclination, resistance, power, HR |
| `concept2-pm5` | Rower | Rowing cadence → strokes/min; decoded stroke count, speed/pace, distance, power, HR where supplied |
| `tacx-fec`, `elite-legacy`, `elite-dr`, `elite-misuro`, `wahoo-legacy`, `cps`, `csc` | Indoor Bike | CPS power and present wheel/crank deltas → speed/cadence |
| `ifit-treadmill`, `bowflex-t216`, `horizon-*`, `spirit-*`, `sole-*`, `walkingpad-*`, `wp2-*`, treadmill manual profiles | Treadmill | Decoded speed/inclination/HR/distance/time only |
| `hrs` | Indoor Bike | HR only; speed is absent |
| FTMS step/stair climber | Step / Stair Climber respectively | Floors, counts, step rates, elevation, energy, HR, MET and time as flagged |

Cadence is normalized in rpm, steps/min, or strokes/min according to the input
profile. Wire encoders apply the appropriate 1 or 0.5 resolution. Count and rate
are distinct: a rate does not manufacture a cumulative count or an average.
The cross-trainer paired step-rate field therefore needs a supplied average
step rate as well as instantaneous cadence.

The Wi-Fi settings selector accepts `auto`, `bike`, `treadmill`, `cross-trainer`,
`rower`, `step-climber`, and `stair-climber`. This override is persisted in the
existing CRC-checked NVS configuration. It changes the **output** profile, not
the source parser or its control capabilities. Fields with no representation
in the selected profile are omitted. Existing configuration enum ordinals are
preserved.

## Treadmill Data — `0x2ACD`

Flags: `uint16`. “Source” below names the normalized metric(s); vendor sources
provide only their decoded subset.

| Field | Flag bit | Type | Unit / resolution | Source |
|---|---:|---|---|---|
| Instantaneous speed | 0 = 0 | uint16 | km/h, 0.01 | speed |
| Average speed | 1 | uint16 | km/h, 0.01 | average speed |
| Total distance | 2 | uint24 | m, 1 | reported distance |
| Inclination + ramp angle | 3 | sint16 + sint16 | %, 0.1; degrees, 0.1 | inclination + reported angle, or mapped geometric angle |
| Positive + negative elevation gain | 4 | uint16 + uint16 | m, 0.1 | elevation gains |
| Instantaneous pace | 5 | uint8 | km/min, 0.1 | treadmill pace |
| Average pace | 6 | uint8 | km/min, 0.1 | average treadmill pace |
| Total energy + energy/hour + energy/minute | 7 | uint16 + uint16 + uint8 | kcal, 1; kcal/h, 1; kcal/min, 1 | reported energy group |
| Heart rate | 8 | uint8 | bpm, 1 | HR |
| Metabolic equivalent | 9 | uint8 | MET, 0.1 | MET |
| Elapsed time | 10 | uint16 | s, 1 | reported elapsed |
| Remaining time | 11 | uint16 | s, 1 | remaining |
| Force on belt + power output | 12 | sint16 + sint16 | N, 1; W, 1 | belt force + power |

## Cross Trainer Data — `0x2ACE`

Flags: **uint24**, not uint16. Direction is encoded in the flags, not as a
following byte; a received direction bit of zero means valid forward movement.

| Field | Flag bit | Type | Unit / resolution | Source |
|---|---:|---|---|---|
| Instantaneous speed | 0 = 0 | uint16 | km/h, 0.01 | speed |
| Average speed | 1 | uint16 | km/h, 0.01 | average speed |
| Total distance | 2 | uint24 | m, 1 | distance |
| Steps/minute + average step rate | 3 | uint16 + uint16 | steps/min, 1 | elliptical cadence/step rate + average step rate |
| Stride count | 4 | uint16 | count, 0.1 | reported stride count |
| Positive + negative elevation gain | 5 | uint16 + uint16 | m, 1 | elevation gains |
| Inclination + ramp angle | 6 | sint16 + sint16 | %, 0.1; degrees, 0.1 | inclination + reported angle, or mapped geometric angle |
| Resistance level | 7 | sint16 | level, 0.1 | resistance |
| Instantaneous power | 8 | sint16 | W, 1 | power |
| Average power | 9 | sint16 | W, 1 | average power |
| Energy group | 10 | uint16 + uint16 + uint8 | kcal / kcal/h / kcal/min, 1 | reported energy group |
| Heart rate | 11 | uint8 | bpm, 1 | HR |
| Metabolic equivalent | 12 | uint8 | MET, 0.1 | MET |
| Elapsed time | 13 | uint16 | s, 1 | elapsed |
| Remaining time | 14 | uint16 | s, 1 | remaining |
| Movement direction | 15 | flag | 0 forward, 1 backward | reported direction |

## Rower Data — `0x2AD1`

Flags: uint16. Pace is seconds per 500 metres, not treadmill pace.

| Field | Flag bit | Type | Unit / resolution | Source |
|---|---:|---|---|---|
| Stroke rate + stroke count | 0 = 0 | uint8 + uint16 | strokes/min, 0.5; count, 1 | rowing cadence + reported stroke count |
| Average stroke rate | 1 | uint8 | strokes/min, 0.5 | average stroke rate |
| Total distance | 2 | uint24 | m, 1 | distance |
| Instantaneous pace | 3 | uint16 | s/500m, 1 | rower pace, or conversion from present positive speed |
| Average pace | 4 | uint16 | s/500m, 1 | average rowing pace |
| Instantaneous power | 5 | sint16 | W, 1 | power |
| Average power | 6 | sint16 | W, 1 | average power |
| Resistance level | 7 | sint16 | level, 1 | resistance |
| Energy group | 8 | uint16 + uint16 + uint8 | kcal / kcal/h / kcal/min, 1 | reported energy group |
| Heart rate | 9 | uint8 | bpm, 1 | HR |
| Metabolic equivalent | 10 | uint8 | MET, 0.1 | MET |
| Elapsed time | 11 | uint16 | s, 1 | elapsed |
| Remaining time | 12 | uint16 | s, 1 | remaining |

## Indoor Bike Data — `0x2AD2`

Flags: uint16.

| Field | Flag bit | Type | Unit / resolution | Source |
|---|---:|---|---|---|
| Instantaneous speed | 0 = 0 | uint16 | km/h, 0.01 | speed |
| Average speed | 1 | uint16 | km/h, 0.01 | average speed |
| Instantaneous cadence | 2 | uint16 | rpm, 0.5 | cadence |
| Average cadence | 3 | uint16 | rpm, 0.5 | average cadence |
| Total distance | 4 | uint24 | m, 1 | distance |
| Resistance level | 5 | sint16 | level, 1 | resistance |
| Instantaneous power | 6 | sint16 | W, 1 | power |
| Average power | 7 | sint16 | W, 1 | average power |
| Energy group | 8 | uint16 + uint16 + uint8 | kcal / kcal/h / kcal/min, 1 | reported energy group |
| Heart rate | 9 | uint8 | bpm, 1 | HR |
| Metabolic equivalent | 10 | uint8 | MET, 0.1 | MET |
| Elapsed time | 11 | uint16 | s, 1 | elapsed |
| Remaining time | 12 | uint16 | s, 1 | remaining |

## Step Climber Data — `0x2ACF`

Flags: uint16. This layout is **not** the Stair Climber layout.

| Field | Flag bit | Type | Unit / resolution | Source |
|---|---:|---|---|---|
| Floors + step count | 0 = 0 | uint16 + uint16 | count, 1 | floors + step count |
| Steps/minute | 1 | uint16 | steps/min, 1 | step rate |
| Average step rate | 2 | uint16 | steps/min, 1 | average step rate |
| Positive elevation gain | 3 | uint16 | m, 1 | positive elevation |
| Energy group | 4 | uint16 + uint16 + uint8 | kcal / kcal/h / kcal/min, 1 | reported energy group |
| Heart rate | 5 | uint8 | bpm, 1 | HR |
| Metabolic equivalent | 6 | uint8 | MET, 0.1 | MET |
| Elapsed time | 7 | uint16 | s, 1 | elapsed |
| Remaining time | 8 | uint16 | s, 1 | remaining |

## Stair Climber Data — `0x2AD0`

Flags: uint16. No Step Count field exists in this characteristic.

| Field | Flag bit | Type | Unit / resolution | Source |
|---|---:|---|---|---|
| Floors | 0 = 0 | uint16 | count, 1 | floors |
| Steps/minute | 1 | uint16 | steps/min, 1 | step rate |
| Average step rate | 2 | uint16 | steps/min, 1 | average step rate |
| Positive elevation gain | 3 | uint16 | m, 1 | positive elevation |
| Stride count | 4 | uint16 | count, 1 | stride count |
| Energy group | 5 | uint16 + uint16 + uint8 | kcal / kcal/h / kcal/min, 1 | reported energy group |
| Heart rate | 6 | uint8 | bpm, 1 | HR |
| Metabolic equivalent | 7 | uint8 | MET, 0.1 | MET |
| Elapsed time | 8 | uint16 | s, 1 | elapsed |
| Remaining time | 9 | uint16 | s, 1 | remaining |

## Service metadata and control

Advertising Service Data contains availability flags followed by a uint16
Fitness Machine Type: treadmill bit 0, cross trainer bit 1, step climber bit 2,
stair climber bit 3, rower bit 4, indoor bike bit 5.

Fitness Machine Feature (`0x2ACC`) contains two uint32 masks: machine features
and target-setting features. Only profile-representable measurements and
source-supported controls are advertised. Profile selection never grants control
over a read-only source.

Supported ranges: Speed `0x2AD4` uses uint16 values at 0.01 km/h; Inclination
`0x2AD5` uses signed min/max at 0.1%, Resistance `0x2AD6` signed min/max at
0.1 level, and Power `0x2AD8` signed min/max at 1 W. Increment is unsigned.
Data resistance resolution and target resistance resolution are not the same.

Control Point `0x2AD9` implements Request Control, Reset, Start/Resume,
Stop/Pause and supported target Speed/Inclination/Resistance/Power; bike
simulation is restricted to Indoor Bike. Responses are indications; acknowledged
commands produce Machine Status (`0x2ADA`) and appropriate Training Status
(`0x2AD3`). Unsupported profile/control combinations are rejected rather than
pretending to apply a target. Lifecycle status commands do not invent physical
start/stop capabilities in proprietary input protocols.

The characteristic layouts were checked against Bluetooth SIG's published
[GATT characteristic definitions](https://github.com/oesmith/gatt-xml), including
the distinct step/stair layouts. Tests cover all six byte layouts, missing and
real-zero samples, negative signed values, uint24 distance, short packets,
mapping, notification fragmentation, and record invalidation. Physical equipment
and app interoperability still require hardware testing.
