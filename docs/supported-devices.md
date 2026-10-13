# Supported input devices

**Not verified on hardware.** “Supported” means a matcher and decoder exist,
not certification that a particular model/firmware works. Host vectors exercise
the protocol layouts; vendor vectors are synthetic samples based on upstream
parsers/command bytes, not validated equipment captures.

The table mirrors `src/core/protocols/registry.cpp`. Highest matching priority wins;
ties keep the first registered match. Pairing can override detection with a
registered id. Names are case-sensitive unless explicitly noted below.

The **FTMS output profile** column describes effective output with the `auto`
setting. A persisted override uses
`auto|bike|treadmill|cross-trainer|rower|step-climber|stair-climber`. Every
profile is sent natively with its own FTMS data characteristic (see
[FTMS machine profiles and data fields](#ftms-machine-profiles-and-data-fields)).
An override never adds missing source control capabilities.

| Id (priority) | Device / profile | Match rule | Metrics | Control | Source / attribution | Test status | FTMS output profile |
|---|---|---|---|---|---|---|---|
| `keiser-m3i` (90) | Keiser M3i | Name starts `M3` and manufacturer payload passes Keiser broadcast parser | Cadence, power, nonzero HR; gear decoded as resistance; no speed/distance forwarded | None; advertisement-only (no GATT connection) | qdomyos-zwift `src/devices/m3ibike/m3ibike.cpp`; `yesoul_keiser.hpp/.cpp` | Host broadcast/detection tests; hardware unverified | `bike` |
| `echelon` (80) | Echelon Connect Sport / EX bike protocol | Name starts `ECH`; excludes `ECH-ROW`, `ECH-STRIDE`, `ECH-SC` case-insensitively; must not advertise FTMS | Cadence, resistance; estimated power and speed from upstream formulas | Normalized resistance 0–100% mapped to levels 1–32; requires notification subscriptions; initialization frames are queued without acknowledgement gating | qdomyos-zwift `src/devices/echelonconnectsport/echelonconnectsport.cpp`; `echelon.hpp/.cpp` | Host frame/init/control tests; hardware unverified | `bike` |
| `domyos` (80) | Domyos / Decathlon bike, Chang Yow init variant | Name starts `Domyos-Bike`, excludes `DomyosBridge`, no FTMS advertisement | Speed, cadence, HR, resistance; estimated power | Normalized resistance 0–100% mapped to levels 1–15; requires initialization acknowledgement | qdomyos-zwift `src/devices/domyosbike/domyosbike.cpp`; `domyos.hpp/.cpp` | Host frame/init/control tests; hardware unverified | `bike` |
| `schwinn-ic4` (70) | Schwinn IC4/IC8; Bowflex bikes with matching names | Case-insensitive `IC BIKE`, `C7-` when name length is **not 17**, or `C9/C10` prefix | FTMS Indoor Bike Data metrics (speed, cadence, power, HR as present); resistance decoded internally | None; forced read-only, bike-data-only FTMS wrapper | qdomyos-zwift `src/bluetooth.cpp` discovery and `src/devices/schwinnic4bike/schwinnic4bike.cpp` behavior; registry uses local FTMS codec | Host matcher/read-only tests; hardware unverified | `bike` |
| `yesoul` (70) | Yesoul bike | Name starts `YESOUL`, case-insensitive `YS_G1M_`, or manufacturer prefix `7d 02 01 05 00 ff ff` | Cadence, power, resistance; speed derived from cadence; decoded distance not forwarded | None (upstream read-only behavior) | qdomyos-zwift `src/devices/yesoulbike/yesoulbike.cpp`; `yesoul_keiser.hpp/.cpp` | Host frame/init/detection tests; hardware unverified | `bike` |
| `ifit-bike` (85) | BLE iFit / NordicTrack / ProForm bike | `I_EB`, `I_SB`, or case-insensitive `_IFIT_BIKE` name layout; FTMS preferred unless proprietary service required | Power, cadence, resistance; speed/incline only on profiles reporting them | Normalized resistance; incline only on compatible Studio profiles | `src/devices/proformbike/proformbike.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `bike` |
| `ifit-treadmill` (85) | BLE iFit / NordicTrack / ProForm treadmill | `I_TL`, `I_IT`, or case-insensitive `_IFIT TREADMILL` layout; FTMS preferred unless proprietary service required | Speed, incline when reported | Bounded speed and incline; Cadence LT has no incline control | `src/devices/proformtreadmill/proformtreadmill.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `treadmill` |
| `ifit-freestride` (85) | BLE ProForm FreeStride | Case-insensitive `I_FS` prefix | Speed, cadence, power, resistance as reported | None in upstream FreeStride path | `src/devices/proformelliptical/proformelliptical.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `cross-trainer` |
| `ifit-elliptical` (85) | BLE NordicTrack elliptical | Case-insensitive `I_EL` prefix | Cadence/speed/resistance; incline/power/HR only in profiles with those fields | Normalized resistance; incline only on compatible profiles | `src/devices/nordictrackelliptical/nordictrackelliptical.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `cross-trainer` |
| `ifit-el-trainer` (85) | BLE ProForm elliptical trainer | Case-insensitive `I_VE` prefix | Speed, cadence, resistance as reported | Normalized resistance | `src/devices/proformellipticaltrainer/proformellipticaltrainer.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `cross-trainer` |
| `bowflex-t216` (85) | Bowflex T216 BLE treadmill | Case-insensitive `BOWFLEX T` prefix | Speed and incline; variant-specific frame layouts | None; upstream buttons cannot remotely set these targets | `src/devices/bowflext216treadmill/bowflext216treadmill.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `treadmill` (read-only) |
| `schwinn-170` (85) | Schwinn 170/270 BLE bike | Case-insensitive `SCHWINN 170/270` prefix; no FTMS | Speed, power, crank-derived cadence, resistance | None | `src/devices/schwinn170bike/schwinn170bike.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `bike` |
| `ifit-model-1` … `ifit-model-97` (0, manual) | Explicit iFit / Bowflex / Schwinn firmware profiles | Manual pairing override only; model labels are shown in the UI, and the exact inventory is below | Profile-specific metrics described above | Profile-specific controls; FreeStride/Bowflex/Schwinn remain read-only | Same pinned files as the corresponding family; every init/poll transcript host-tested | host-tested, not hardware-verified | 1–23 `bike`; 24–83 `treadmill`; 84–90 `cross-trainer`; 91–96 read-only `treadmill`; 97 `bike` |
| `tacx-fec` (80) | Tacx Neo / Flux | Case-insensitive `TACX NEO` / `TACX FLUX` or custom service `6e40fec1-b5a3-f393-e0a9-e50e24dcca9e`; no FTMS | CPS power, CPS/CSC cadence and wheel-derived speed, optional HR | Target power and simulated grade via BLE FE-C tunnel; upstream resistance emulation maps 0–100% to 0–10% grade, not a physical brake level | `src/devices/tacxneo2/tacxneo2.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `bike` |
| `elite-legacy` (80) | Elite legacy trainers | Case-insensitive `ELITETRAINER`, exact `KU` / `QD` / `RM`, or custom service `347b0001-7635-408b-8918-8ff3949ce592`; no FTMS | CPS power, CPS/CSC cadence and wheel-derived speed, optional HR | Power, brake resistance, simulation grade after timed proprietary initialization | `src/devices/stagesbike/stagesbike.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `bike` |
| `elite-dr` (80) | Elite DR variant | Exact case-insensitive `DR`; no FTMS | CPS/CSC/HR as available | Power and brake resistance; no simulation grade | `src/devices/stagesbike/stagesbike.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `bike` |
| `elite-misuro` (70) | Elite Misuro B+ | Case-insensitive `MISURO B+` prefix; no FTMS | CPS/CSC/HR as available | None; no brake motor claimed | `src/devices/stagesbike/stagesbike.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `bike` |
| `wahoo-legacy` (80) | Wahoo KICKR without FTMS | Case-insensitive `KICKR SNAP`, `KICKR BIKE`, `KICKR ROLLR`, `KICKR CORE`, `KICKR MOVE `, `WAHOO KICKR`; excludes RUN/Headwind; no FTMS | CPS power, CPS/CSC cadence and wheel-derived speed, optional HR | Power, normalized brake resistance, simulation grade after acknowledged unlock/init | `src/devices/wahookickrsnapbike/wahookickrsnapbike.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `bike` |
| `concept2-pm5` (80) | Concept2 PM5 rower | Case-insensitive `PM5` prefix with `ROW` suffix, or unnamed rowing service `ce060030-43e5-11e4-916c-0800200c9a66`; excludes Ski/Bike names and FTMS | Rowing speed, stroke rate (mapped to cadence), power, HR, elapsed time, distance, pace / average pace (s/500 m), stroke count as available; individual or multiplexed status notifications | None; mechanical damper is not a controllable brake | Concept2 rowing characteristic layouts in `src/devices/concept2skierg/concept2skierg.cpp`, `src/devices/ftmsrower/ftmsrower.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `rower` |
| `horizon-tm`, `horizon-paragon` (80) | Horizon custom BLE / Paragon X | Case-insensitive Horizon/YPTM/HZ_T101/HZ_7.0AT/AFG SPORT/WLT2541 layouts; `PARAGON X` / `JFTMPARAGON` selects Paragon; no FTMS | Speed, incline | Speed/incline within model limits; connection never starts belt | `src/devices/horizontreadmill/horizontreadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `treadmill` |
| `spirit-treadmill`, `spirit-xt485`, `spirit-xt800` (80) | Spirit XT385 / XT485 / XT800/900 | Case-insensitive corresponding `XT385`, `XT485`, `XT800` / `XT900` prefixes; no FTMS | Speed, incline as reported | Status-driven speed buttons; incline only on XT385; stops stepping on stale status or target crossing | `src/devices/spirittreadmill/spirittreadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `treadmill` |
| `sole-treadmill`, `sole-f63`, `sole-f65`, `sole-tt8` (80) | Sole F80/F85/F89/S77/ST90/TRX7.5 / F63 / F65 / TT8 | Case-insensitive corresponding upstream model prefixes; no FTMS | Speed (mph converted to km/h), incline, HR | Status-driven speed/incline buttons; no unsolicited belt start | `src/devices/solef80treadmill/solef80treadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `treadmill` |
| `walkingpad-r1`, `walkingpad-flat`, `walkingpad-re` (80) | Kingsmith / WalkingPad R1 Pro, A1/C2, RE | Case-insensitive upstream R1/KS prefixes; `RE` exact; excludes R2 names and FTMS-only variants | Speed | Speed only; flat walking variants capped at 6 km/h, R1 at 10 km/h; no incline | `src/devices/kingsmithr1protreadmill/kingsmithr1protreadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `treadmill` |
| `walkingpad-r2`, `walkingpad-g1`, `walkingpad-g1b`, `wp2-c*-s*` (90) | Kingsmith R2 / X21 / K12 / G1 / MXG | Case-insensitive upstream `KS-ST-K12PRO`, `KS-R1AC`, `KS-HC-R1AA/AC`, X21, MXG, G1 prefixes; advertised custom services select UUID variant; nondefault cipher overrides are manual; no FTMS | Speed, step rate if reported | Speed only; G1 capped at 6 km/h; no incline | `src/devices/kingsmithr2treadmill/kingsmithr2treadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified | `treadmill` |
| `ftms` (50) | FTMS bike, treadmill, cross trainer, step climber, stair climber, rower | Advertised Fitness Machine Service `0x1826` | Indoor Bike `0x2AD2`, Treadmill `0x2ACD`, Cross Trainer `0x2ACE`, Step Climber `0x2ACF`, Stair Climber `0x2AD0`, Rower `0x2AD1`: every FTMS data field of the profile, only when the machine sends it (see field table below) | Feature-gated power, resistance, incline, speed; Request Control handshake; power/resistance/incline honor supplied ranges; grade via bike simulation if needed. Speed is available via console/API and treadmill FTMS output | Local Bluetooth FTMS codec and `ftms_source.hpp` / `src/core/ftms_source.cpp` | Host byte-layout, feature/range/control tests; hardware unverified | Derived from input data characteristic/service data: `bike`, `treadmill`, `cross-trainer`, `step-climber`, `stair-climber` or `rower` |
| `sportstech-f15` (80) | **Sportstech F15 treadmill** | Case-insensitive `FS-` advertising prefix; F15-compatible names/service `0xFFF0`; excludes FTMS | Speed, incline, distance, elapsed time, calories, pulse; operation mode and its four state flags | Speed 0.8–12 km/h (0 stops), incline 0–15%; explicit start, stop-backed pause | syssi `esphome-treadmill-f15`, pinned revision below; `sportstech_f15.hpp/.cpp` | Upstream captured status and host control/UI tests; hardware unverified | `treadmill` |
| `cps` (40) | Cycling Power sensor | Service `0x1818` | Power, cadence derived from crank revolution/event deltas | None | Local Bluetooth CPS codec; `sensor_sources.hpp`, `src/core/sensor_sources.cpp`, `src/core/sensor_codec.cpp` | Host power/crank/wraparound tests; hardware unverified | `bike` |
| `csc` (30) | Cycling Speed and Cadence sensor | Service `0x1816` | Speed (fixed **2.105 m** wheel circumference), cadence from wheel/crank deltas | None | Local Bluetooth CSC codec; sensor sources/codec | Host wheel/crank/wraparound tests; hardware unverified | `bike` |
| `hrs` (20) | Heart Rate monitor | Service `0x180D` | Heart rate, 8- or 16-bit measurement | None | Local Bluetooth HRS codec; sensor sources/codec | Host 8/16-bit parser tests; hardware unverified | `bike` |

Vendor ports are GPL-derived from
[`cagnulein/qdomyos-zwift`](https://github.com/cagnulein/qdomyos-zwift),
Copyright Roberto Viola and contributors (GPL-3.0). Attribution is retained in
`include/core/protocols/` and `src/core/protocols/`. The original ports do not record a
revision; new batch-1 ports use the pinned revision below.
Discovery rules follow upstream `src/devices/bluetooth.cpp`. See [license and
attribution](../README.md#license-and-attribution) and [HD-005](hardware-decisions.md#hd-005-gpl-30-licensing-for-qdomyos-zwift-ports).

## Sportstech F15

Native GPL-3.0 port of [syssi's ESPHome component](https://github.com/syssi/esphome-treadmill-f15/tree/61172d6448e00d0a4eb5d70c3c39a3427c292041/components/treadmill_f15),
without ESPHome dependencies. Select **Sportstech F15 treadmill** in either UI;
the captured advertising name is `FS-46CEE4`. Generic `FFF0` alone is not
unique to F15 and must not select this control protocol for unrelated equipment.

BLE UUIDs (Bluetooth base `0000xxxx-0000-1000-8000-00805f9b34fb`):
service `FFF0`, notify `FFF1`, write-without-response `FFF2`. Subscribe first,
then send `02 51 51 03` immediately and every second for status/keep-alive.
Frames are `02 REGISTER PAYLOAD... XOR 03`; XOR covers register and payload.

The 17-byte status layout is:

```text
02 51 MODE SPEED INCLINE TIMElo TIMEhi DISTlo DISThi
CALlo CALhi UNKNOWN UNKNOWN HRlo HRhi XOR 03
```

`MODE`: 0 stopped, 2 starting, 3 running, 4 stopping. Speed is 0.1 km/h;
received incline is 0.1%; little-endian time is seconds, distance metres,
energy 0.1 kcal, pulse bpm. Short status responses provide mode and zero
speed/pulse, retaining previous distance/time/energy/incline as upstream does.
`01` is a start acknowledgement, not a telemetry sample. Bytes 11–12 are
undecoded upstream: **no step count or error code is fabricated**.

All six measurements use FTMS Treadmill Data `2ACD`; the existing codec derives
ramp angle from incline and leaves unavailable energy-rate fields unavailable.
Confirmed operation mode drives Training Status and
Fitness Machine Status and is also exposed in web `treadmillStatus` (mode ID,
text, and stopped/starting/running/stopping flags) and the HMI state badge.
Elapsed seconds carry the complete time value; the upstream formatted-time
sensor is just a derived display, not another measurement. Distance, time and
energy are selectable on the HMI dashboard.

Controls: start `02 53 01 00 00 00 00 00 52 03`, stop
`02 53 03 50 03`, combined targets
`02 53 02 SS II 00 00 00 00 00 00 00 XOR 03`.
`SS` is speed ×10; `II` is integer percent, truncating fractional incline as
upstream does. **Receive incline scaling differs from transmit scaling** in
the reference; this port preserves it pending real-device verification.
Changing one target preserves the other requested target, not a differently
scaled measurement. Speed limits are 0.8–12 km/h, incline 0–15%;
speed zero uses stop. Explicit start applies targets after the upstream six-second
countdown; stop/pause cancels that delayed write. Pairing/reconnecting never starts
the belt. Controls require fresh status.

FTMS `02` Set Target Speed, `03` Set Target Inclination, `07` Start/Resume and
`08` Stop/Pause pass through the core safety gate. The reference has no distinct
pause command: **Pause stops the F15 and Resume uses start**, not a guaranteed
preserved-workout hardware pause. Console/API `run-state` values are
0 stop, 1 start/resume, 2 pause. Keep the physical safety key/stop available;
hardware and Zwift interoperability remain unverified.

## Batch 1: BLE-only vendor protocols

Devices are assessed independently; an unsupported variant does not block
other manufacturers. **Peloton Bike / Bike+ and DFC hardware are excluded.**
No ANT+, USB, Wi-Fi, ADB, UDP, telnet, cloud or OCR input is implemented.

This assessment uses qdomyos-zwift revision
[`ea70d7ff64bc8bf508fc6401fd056211e05311b3`](https://github.com/cagnulein/qdomyos-zwift/tree/ea70d7ff64bc8bf508fc6401fd056211e05311b3),
not an assumption based on the device name:

- [`src/devices/pelotonbike/pelotonbike.cpp`](https://github.com/cagnulein/qdomyos-zwift/blob/ea70d7ff64bc8bf508fc6401fd056211e05311b3/src/devices/pelotonbike/pelotonbike.cpp)
  creates a `QUdpSocket` bound to UDP port **8003**.
  `pelotonOCRprocessPendingDatagrams()` parses semicolon-separated OCR metrics;
  there is no input GATT service, subscription, BLE frame decoder or BLE init
  sequence to port. `forceResistance()` is empty. Its virtual-bike interface is
  an **output**, not evidence of a native Peloton BLE input.
- The BLE workaround requires an accessory rather than just bridge firmware.
  [DFC's installation guide and compatibility FAQ](https://github.com/intelligenate/dfc/blob/c7c4a83bf9b444b554a1fdcd0f7ac8d3ae456ebb/docs/user-guide.md)
  describe inserting externally powered hardware into the original Bike's
  data cable and explicitly exclude **Bike+**. This serial-to-BLE accessory is
  not a native Peloton protocol that can be enabled by a bridge firmware update.
  Upstream discovery recognizes an exact, case-insensitive `DFC` name via
  [`src/devices/bluetooth.cpp`](https://github.com/cagnulein/qdomyos-zwift/blob/ea70d7ff64bc8bf508fc6401fd056211e05311b3/src/devices/bluetooth.cpp)
  and routes it to
  [`src/devices/stagesbike/stagesbike.cpp`](https://github.com/cagnulein/qdomyos-zwift/blob/ea70d7ff64bc8bf508fc6401fd056211e05311b3/src/devices/stagesbike/stagesbike.cpp),
  which parses standard Cycling Power Measurement notifications. This evidence
  does **not** add accessory support, Bike+ compatibility or Peloton resistance
  control to this batch.

Do not pair a native Peloton by forcing a vendor adapter, or infer control
capabilities from the upstream virtual output. OCR/UDP, Wi-Fi, cloud, USB and
ANT+ paths remain outside this BLE-only batch.

### Excluded transports and proposed batch 2

- Bowflex C6 and C7/VeloCore bikes advertising the existing `IC BIKE`, `C7-`
  (non-17-character) or `C9/C10` layouts retain the read-only `schwinn-ic4`
  adapter. Other genuine FTMS variants use `ftms`; a manufacturer name alone
  does not establish a new proprietary control protocol.
- Peloton Bike/Bike+ and DFC: explicitly excluded from this batch. The pinned
  evidence above explains the native BLE limitation; no DFC-specific matcher,
  accessory control or Peloton adapter is added.
- `nordictrackifitadbbike`: its constructor binds a UDP socket on port 8002.
  Other ADB/Wi-Fi/OCR iFit paths are likewise outside the BLE-only scope.
- ANT+ FE-C hardware is excluded; FE-C packets transported by a native BLE
  service are eligible and do not require ANT+ hardware.
- No distinct VeloCore proprietary BLE class/parser is present in the pinned
  upstream discovery/build inventory. VeloCore models using the existing
  Schwinn advertisement/Indoor Bike Data layout remain read-only through
  `schwinn-ic4`; other models require verified BLE evidence, not a guessed port.

A proposed batch 2 from the remaining upstream catalogue is
Domyos treadmills/rowers/ellipticals (`domyostreadmill`, `domyosrower`,
`domyoselliptical`), Echelon rower/stride/stair variants (`echelonrower`,
`echelonstride`, `echelonstairclimber`), Renpho bikes (`renphobike`) and FitShow
treadmills (`fitshowtreadmill`). These are audit candidates, not promised ports:
verify BLE transport and model-specific safety/control behavior first.

### Resource and validation notes

The vendor batch did not change the BLE queues. Machine-profile selection now
adds a one-byte output override in configuration schema v3 (maximum 252 bytes;
v1/v2 migrate with `auto`). Only the selected source protocol is instantiated;
inactive adapters contribute code and registry metadata, not one live protocol
object per device.

Baseline `main` CI run
[`38041095684`](https://github.com/manuelh83/smartfitness-bridge/actions/runs/38041095684)
reports **1,151,201 bytes flash** and **62,948 bytes static RAM**. A local
PlatformIO build could not download the `espressif32` platform
(`HTTPClientError`, before compilation). A firmware flash/RAM delta must be
measured from a completed build; host object sizes are not ESP32 size measurements.
Host tests use upstream byte layouts and literal commands, not hardware captures.
The audited iFit/Bowflex host `-Os` object totals about **120 KB** before final
registry expansion; init/poll hex literals occupy **81,801 bytes** and resistance
maps **7,168 bytes**. These are host/storage estimates, **not measured firmware
flash/RAM deltas**. Only one source object is allocated at runtime; manual profile
registry metadata and IDs also consume memory. ESP32 totals remain pending CI.
The iFit initialization deadline is 33 seconds with 35 seconds of startup grace;
the longest audited transcript is 71 writes (28.4 seconds nominal). Treadmill
handshakes request 90 seconds; the adapter caps any protocol grace at 120 seconds.
Neither startup grace nor subscription success fabricates telemetry.

The pairing UI offers `ifit-model-N` overrides with exact upstream model keys
as labels. Use a matching firmware profile when the generic `I_*` advertisement
cannot distinguish it. IDs use immutable pinned-table ordinals; existing entries
must not be reordered when adding profiles. Bowflex models with differing
service/frame layouts require their explicit profile rather than the T216 default.

#### Manual profile inventory

Each range lists model keys **in ordinal order**: the first key uses the first
`ifit-model-N` ID in that range. These are the exact pinned upstream setting keys,
not advertised names. Metrics, controls and source files follow the family table
above; all entries are **host-tested, not hardware-verified**.

| IDs | Family | FTMS output profile | Model keys in order |
|---|---|---|---|
| 1–7 | Bike | `bike` | `proformbike`, `freemotion_coachbike_b22_7`, `nordictrack_GX4_5_bike`, `nordictrack_gx_2_7`, `nordictrack_gx_44_pro`, `nordictrack_gx_4_5_pro`, `nordictrack_vr21` |
| 8–14 | Bike | `bike` | `proform_225_csx_PFEX32925_INT_0`, `proform_bike_225_csx`, `proform_bike_325_csx`, `proform_bike_325_csx_PFEX439210INT_0`, `proform_bike_PFEVEX71316_0`, `proform_bike_PFEVEX71316_1`, `proform_bike_sb` |
| 15–23 | Bike | `bike` | `proform_csx210`, `proform_cycle_trainer_300_ci`, `proform_cycle_trainer_400`, `proform_hybrid_trainer_PFEL03815`, `proform_studio`, `proform_studio_NTEX71021`, `proform_tdf_10`, `proform_tour_de_france_clc`, `proform_xbike` |
| 24–30 | Treadmill | `treadmill` | `proformtreadmill`, `nordictrack_10_treadmill`, `nordictrack_elite_800`, `nordictrack_incline_trainer_x7i`, `nordictrack_incline_trainer_x7i_netl18716_0`, `nordictrack_incline_trainer_x7i_ntl15010_0`, `nordictrack_s20_treadmill` |
| 31–37 | Treadmill | `treadmill` | `nordictrack_s20i_treadmill`, `nordictrack_s30_treadmill`, `nordictrack_series_7`, `nordictrack_t65s_83_treadmill`, `nordictrack_t65s_treadmill`, `nordictrack_t65s_treadmill_81_miles`, `nordictrack_t70_treadmill` |
| 38–44 | Treadmill | `treadmill` | `nordictrack_treadmill_commercial_le`, `nordictrack_treadmill_exp_5i`, `nordictrack_treadmill_ultra_le`, `nordictrack_tseries5_treadmill`, `norditrack_s25_treadmill`, `norditrack_s25i_treadmill`, `proform_2000_treadmill` |
| 45–51 | Treadmill | `treadmill` | `proform_505_cst_80_44`, `proform_595i_proshox2`, `proform_8_5_treadmill`, `proform_carbon_tl`, `proform_carbon_tl_PFTL59720`, `proform_carbon_tl_PFTL59722c`, `proform_carbon_tl_PFTL59723_6` |
| 52–58 | Treadmill | `treadmill` | `proform_carbon_tlx_treadmill`, `proform_carbon_tlx_v84_314_treadmill`, `proform_performance_300i`, `proform_performance_400i`, `proform_pro_1000_treadmill`, `proform_proshox2`, `proform_trainer_8_0` |
| 59–65 | Treadmill | `treadmill` | `proform_trainer_8_0_pftl59721_0`, `proform_trainer_8_0_pftl59721_int_0`, `proform_treadmill_105_cst`, `proform_treadmill_1500_pro`, `proform_treadmill_1800i`, `proform_treadmill_505_cst`, `proform_treadmill_575i` |
| 66–72 | Treadmill | `treadmill` | `proform_treadmill_705_cst`, `proform_treadmill_705_cst_V78_239`, `proform_treadmill_705_cst_V80_44`, `proform_treadmill_8_0`, `proform_treadmill_8_7`, `proform_treadmill_995i`, `proform_treadmill_9_0` |
| 73–79 | Treadmill | `treadmill` | `proform_treadmill_c700`, `proform_treadmill_c960i`, `proform_treadmill_cadence_lt`, `proform_treadmill_carbon_tls`, `proform_treadmill_cst_505_pftl59420_0`, `proform_treadmill_l6_0s`, `proform_treadmill_se` |
| 80–83 | Treadmill | `treadmill` | `proform_treadmill_sport_3_0`, `proform_treadmill_sport_70`, `proform_treadmill_sport_8_5`, `proform_treadmill_z1300i` |
| 84–90 | Cross-trainer / ellipticals | `cross-trainer` | `proformelliptical` (FreeStride), `nordictrackelliptical`, `nordictrack_elliptical_c7_5`, `proform_hybrid_trainer_xt`, `proformellipticaltrainer`, `nordictrack_se7i`, `nordictrack_elliptical_s700` |
| 91–96 | Bowflex treadmill | `treadmill` (read-only) | `bowflex-t216`, `bowflex-t216-alt`, `bowflex-t6`, `bowflex-btx116`, `bowflex-t8j`, `bowflex-t128` |
| 97 | Schwinn bike | `bike` (read-only) | `schwinn-170` |

Schwinn 170 opts into bounded GATT notification discovery (at most eight
subscriptions). Its init write is restricted to the two upstream characteristic
UUIDs `5ec4e520-9804-11e3-b4b9-0002a5d5c51b` and
`1717b3c0-9803-11e3-90e1-0002a5d5c51b`; the discovered endpoint is passed to the
pure-C++ protocol before connection initialization. No arbitrary GATT write
endpoint is selected and no control capability is exposed.

WalkingPad override IDs preserve both cipher and UUID layout across reconnect:
`walkingpad-r2` is classic cipher with standard service; alternate `wp2-c0-*`
IDs select the upstream-used UUID layouts. `wp2-c2-*` through `wp2-c5-*` select
upstream V2–V5 cipher tables, `wp2-c6-*` the G1 cipher, and `wp2-c7-*` MXG.
Suffix `s0` means standard service/characteristics; `s1c1` selects service
`00011234…` with `0001fed7/fed8`; `s2c2` selects service `00021234…` with
`0002fed7/fed8`. Nondefault ciphers must be explicitly selected
when advertisements do not distinguish them. These are BLE protocol variants,
not claims that every model can use every cipher.

## Important boundaries

- This is **not** the entire qdomyos-zwift catalogue. Echelon rower/stride/stair
  variants and other Domyos initialization variants are not implemented.
- Only one input is selected: CPS, CSC and HRS are alternative sources, not
  additional sensors merged with a trainer.
- Every metric is optional: a value the device does not send stays absent
  (`null` in the API, `--` on displays) and is never stored, forwarded or
  re-encoded as `0`. Resistance is the device's own level (FTMS / vendor
  scale) and differs from the normalized 0–100 % resistance **control target**.
  Vendor totals are not synthesized and elapsed time is never inferred from
  connection uptime.
- “Controllable” in the registry is a possibility, not a runtime guarantee:
  source subscription/init/Feature/Request Control must succeed first. Estimated
  vendor power/speed is not a calibrated physical measurement.
- Automatic FTMS output uses the input's machine profile: treadmill data for all
  treadmill adapters (including read-only Bowflex variants), cross trainer data
  for ellipticals, rower data for rowers; bikes and standalone sensors use bike
  data.
  CPS/HRS here are **inputs only**, not Garmin-compatible output services.
- Treadmill FTMS speed/incline commands remain capability-gated and obey core
  bounds (speed 0–100 km/h, incline −20–25%) plus each adapter's tighter limits.
  The output speed range currently exposes core bounds with 0.01 km/h resolution,
  not each source's universal negotiated range. Start never starts the belt and
  returns Operation Failed because there is no safe source start primitive;
  Stop/Pause requires an accepted speed-zero command or returns Operation Failed.
  The FTMS input handshake sends automatic Start/Resume only for confirmed bike
  inputs, not ambiguous machines or other profiles; stale treadmill
  telemetry disables controls until fresh data restores readiness.
  Disconnect/stale data never send stop/speed; physical controls and the safety
  key remain primary. See [FTMS output](../README.md#ftms-output-bluetooth-le).

## FTMS machine profiles and data fields

Input (`ftms` adapter) and output (FTMS server) use the same per-profile codec
(`include/core/ftms/ftms_codec.hpp`). Each profile struct holds every field of
its FTMS data characteristic as `std::optional` in the **native wire type**
(e.g. `uint16` 0.01 km/h, `sint16` 0.1 %, `uint24` m), so a field the machine
does not send is absent instead of 0. Decoding maps only present fields onto
bridge metrics (normalized units below); encoding sets only the flags of
present fields. Energy sub-fields that are unavailable are sent as the FTMS
"not available" values (`0xFFFF`/`0xFF`) and decode as absent. The second
member of an FTMS field pair that shares one flag (e.g. inclination + ramp
angle) is sent as 0 only when the pair is required by the spec and the bridge
has no value; ramp angle is derived from inclination and rower pace from speed
when missing.

Records longer than 20 bytes (default ATT MTU) are split into several
notifications as FTMS requires: all but the last set "More Data", and the
first field group (e.g. instantaneous speed / stroke rate) closes the record.

### Profile ↔ machine type

| Machine type | Data characteristic | Service-data type bit | Config / API name |
|---|---|---|---|
| Indoor bike | `0x2AD2` | 5 | `bike` |
| Treadmill | `0x2ACD` | 0 | `treadmill` |
| Cross trainer / elliptical | `0x2ACE` | 1 | `cross-trainer` |
| Step climber | `0x2ACF` | 2 | `step-climber` |
| Stair climber | `0x2AD0` | 3 | `stair-climber` |
| Rower | `0x2AD1` | 4 | `rower` |

Protocol → profile: vendor bikes, trainers and CPS/CSC/HRS sensors → `bike`;
treadmill protocols (iFit/Bowflex/Horizon/Spirit/Sole/WalkingPad) → `treadmill`;
iFit FreeStride / ellipticals (`ifit-freestride`, `ifit-elliptical`,
`ifit-el-trainer`, manual models 84–90) → `cross-trainer`; Concept2 PM5 →
`rower`; generic `ftms` → the data characteristic/service data the machine
exposes, including step and stair climbers.

### Data fields per profile

Metric units: speed km/h, distance m, cadence 1/min (pedal, step or stroke
rate), power W, heart rate bpm, incline %, ramp angle °, elevation m, energy
kcal (per hour / per minute), MET, time s, pace s/500 m, force N; counts are
unitless.

| FTMS field (type, resolution) | Bike | Treadmill | Cross trainer | Step climber | Stair climber | Rower | Bridge metric |
|---|---|---|---|---|---|---|---|
| Instantaneous speed (uint16, 0.01 km/h) | ✓ | ✓ | ✓ | | | | `speed` |
| Average speed (uint16, 0.01 km/h) | ✓ | ✓ | ✓ | | | | `averageSpeed` |
| Instantaneous cadence (uint16, 0.5 rpm) | ✓ | | | | | | `cadence` |
| Average cadence (uint16, 0.5 rpm) | ✓ | | | | | | `averageCadence` |
| Step per minute / average step rate (uint16, 1) | | | ✓ | ✓ | ✓ | | `cadence` / `averageCadence` |
| Stroke rate / average stroke rate (uint8, 0.5) | | | | | | ✓ | `cadence` / `averageCadence` |
| Stroke count (uint16) | | | | | | ✓ | `strokeCount` |
| Total distance (uint24, 1 m) | ✓ | ✓ | ✓ | | | ✓ | `distance` |
| Inclination (sint16, 0.1 %) + ramp angle (sint16, 0.1 °) | | ✓ | ✓ | | | | `incline`, `rampAngle` |
| Positive / negative elevation gain (uint16; treadmill 0.1 m, others 1 m) | | ✓ | ✓ | ✓ (positive) | ✓ (positive) | | `positiveElevationGain` / `negativeElevationGain` |
| Instantaneous / average pace (treadmill uint8 0.1 km/min; rower uint16 s/500 m) | | ✓ | | | | ✓ | `pace` / `averagePace` (rower: also `speed` / `averageSpeed`) |
| Floors (uint16) | | | | ✓ | ✓ | | `floors` |
| Step count (uint16) | | | | ✓ | | | `stepCount` |
| Stride count (uint16; cross trainer 0.1, stair climber 1) | | | ✓ | | ✓ | | `strideCount` |
| Resistance level (sint16; cross trainer 0.1, others 1) | ✓ | | ✓ | | | ✓ | `resistance` |
| Instantaneous / average power (sint16, 1 W) | ✓ | | ✓ | | | ✓ | `power` / `averagePower` |
| Force on belt (sint16, 1 N) + power output (sint16, 1 W) | | ✓ | | | | | `forceOnBelt`, `power` |
| Total energy / per hour (uint16) / per minute (uint8) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | `totalEnergy`, `energyPerHour`, `energyPerMinute` |
| Heart rate (uint8, 1 bpm) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | `heartRate` |
| Metabolic equivalent (uint8, 0.1) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | `metabolicEquivalent` |
| Elapsed / remaining time (uint16, 1 s) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | `elapsed`, `remainingTime` |
| Movement direction (cross trainer flag bit 15) | | | ✓ | | | | decoded, not a metric |

Control targets per output profile: speed (treadmill, cross trainer),
inclination (bike via simulation, treadmill, cross trainer), resistance and
power (all profiles except treadmill). The Feature characteristic only
announces fields the selected profile can carry and the source supports.

For the extension steps (`SourceProtocol`, precise registry priorities, byte-sample
host tests and attribution headers), see [adding new protocols](../README.md#supported-devices-and-new-protocols).
