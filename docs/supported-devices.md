# Supported input devices

**Not verified on hardware.** “Supported” means a matcher and decoder exist,
not certification that a particular model/firmware works. Host vectors exercise
the protocol layouts; vendor vectors are synthetic samples based on upstream
parsers/command bytes, not validated equipment captures.

The table mirrors `src/protocols/registry.cpp`. Highest matching priority wins;
ties keep the first registered match. Pairing can override detection with a
registered id. Names are case-sensitive unless explicitly noted below.

| Id (priority) | Device / profile | Match rule | Metrics | Control | Source / attribution | Test status |
|---|---|---|---|---|---|---|
| `keiser-m3i` (90) | Keiser M3i | Name starts `M3` and manufacturer payload passes Keiser broadcast parser | Cadence, power, nonzero HR; gear decoded as resistance; no speed/distance forwarded | None; advertisement-only (no GATT connection) | qdomyos-zwift `src/devices/m3ibike/m3ibike.cpp`; `yesoul_keiser.hpp/.cpp` | Host broadcast/detection tests; hardware unverified |
| `echelon` (80) | Echelon Connect Sport / EX bike protocol | Name starts `ECH`; excludes `ECH-ROW`, `ECH-STRIDE`, `ECH-SC` case-insensitively; must not advertise FTMS | Cadence, resistance; estimated power and speed from upstream formulas | Normalized resistance 0–100% mapped to levels 1–32; requires notification subscriptions; initialization frames are queued without acknowledgement gating | qdomyos-zwift `src/devices/echelonconnectsport/echelonconnectsport.cpp`; `echelon.hpp/.cpp` | Host frame/init/control tests; hardware unverified |
| `domyos` (80) | Domyos / Decathlon bike, Chang Yow init variant | Name starts `Domyos-Bike`, excludes `DomyosBridge`, no FTMS advertisement | Speed, cadence, HR, resistance; estimated power | Normalized resistance 0–100% mapped to levels 1–15; requires initialization acknowledgement | qdomyos-zwift `src/devices/domyosbike/domyosbike.cpp`; `domyos.hpp/.cpp` | Host frame/init/control tests; hardware unverified |
| `schwinn-ic4` (70) | Schwinn IC4/IC8; Bowflex bikes with matching names | Case-insensitive `IC BIKE`, `C7-` when name length is **not 17**, or `C9/C10` prefix | FTMS Indoor Bike Data metrics (speed, cadence, power, HR as present); resistance decoded internally | None; forced read-only, bike-data-only FTMS wrapper | qdomyos-zwift `src/bluetooth.cpp` discovery and `src/devices/schwinnic4bike/schwinnic4bike.cpp` behavior; registry uses local FTMS codec | Host matcher/read-only tests; hardware unverified |
| `yesoul` (70) | Yesoul bike | Name starts `YESOUL`, case-insensitive `YS_G1M_`, or manufacturer prefix `7d 02 01 05 00 ff ff` | Cadence, power, resistance; speed derived from cadence; decoded distance not forwarded | None (upstream read-only behavior) | qdomyos-zwift `src/devices/yesoulbike/yesoulbike.cpp`; `yesoul_keiser.hpp/.cpp` | Host frame/init/detection tests; hardware unverified |
| `ifit-bike` (85) | BLE iFit / NordicTrack / ProForm bike | `I_EB`, `I_SB`, or case-insensitive `_IFIT_BIKE` name layout; FTMS preferred unless proprietary service required | Power, cadence, resistance; speed/incline only on profiles reporting them | Normalized resistance; incline only on compatible Studio profiles | `src/devices/proformbike/proformbike.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `ifit-treadmill` (85) | BLE iFit / NordicTrack / ProForm treadmill | `I_TL`, `I_IT`, or case-insensitive `_IFIT TREADMILL` layout; FTMS preferred unless proprietary service required | Speed, incline when reported | Bounded speed and incline; Cadence LT has no incline control | `src/devices/proformtreadmill/proformtreadmill.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `ifit-freestride` (85) | BLE ProForm FreeStride | Case-insensitive `I_FS` prefix | Speed, cadence, power, resistance as reported | None in upstream FreeStride path | `src/devices/proformelliptical/proformelliptical.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `ifit-elliptical` (85) | BLE NordicTrack elliptical | Case-insensitive `I_EL` prefix | Cadence/speed/resistance; incline/power/HR only in profiles with those fields | Normalized resistance; incline only on compatible profiles | `src/devices/nordictrackelliptical/nordictrackelliptical.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `ifit-el-trainer` (85) | BLE ProForm elliptical trainer | Case-insensitive `I_VE` prefix | Speed, cadence, resistance as reported | Normalized resistance | `src/devices/proformellipticaltrainer/proformellipticaltrainer.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `bowflex-t216` (85) | Bowflex T216 BLE treadmill | Case-insensitive `BOWFLEX T` prefix | Speed and incline; variant-specific frame layouts | None; upstream buttons cannot remotely set these targets | `src/devices/bowflext216treadmill/bowflext216treadmill.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `schwinn-170` (85) | Schwinn 170/270 BLE bike | Case-insensitive `SCHWINN 170/270` prefix | Speed, power, crank-derived cadence, resistance | None | `src/devices/schwinn170bike/schwinn170bike.cpp`; `ifit_bowflex.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `tacx-fec` (80) | Tacx Neo / Flux | Case-insensitive `TACX NEO` / `TACX FLUX` or custom service `6e40fec1-b5a3-f393-e0a9-e50e24dcca9e`; no FTMS | CPS power, CPS/CSC cadence and wheel-derived speed, optional HR | Target power and simulated grade via BLE FE-C tunnel; upstream resistance emulation maps 0–100% to 0–10% grade, not a physical brake level | `src/devices/tacxneo2/tacxneo2.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `elite-legacy` (80) | Elite legacy trainers | Case-insensitive `ELITETRAINER`, exact `KU` / `QD` / `RM`, or custom service `347b0001-7635-408b-8918-8ff3949ce592`; no FTMS | CPS power, CPS/CSC cadence and wheel-derived speed, optional HR | Power, brake resistance, simulation grade after timed proprietary initialization | `src/devices/stagesbike/stagesbike.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `elite-dr` (80) | Elite DR variant | Exact case-insensitive `DR`; no FTMS | CPS/CSC/HR as available | Power and brake resistance; no simulation grade | `src/devices/stagesbike/stagesbike.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `elite-misuro` (70) | Elite Misuro B+ | Case-insensitive `MISURO B+` prefix; no FTMS | CPS/CSC/HR as available | None; no brake motor claimed | `src/devices/stagesbike/stagesbike.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `wahoo-legacy` (80) | Wahoo KICKR without FTMS | Case-insensitive `KICKR SNAP`, `KICKR BIKE`, `KICKR ROLLR`, `KICKR CORE`, `KICKR MOVE `, `WAHOO KICKR`; excludes RUN/Headwind; no FTMS | CPS power, CPS/CSC cadence and wheel-derived speed, optional HR | Power, normalized brake resistance, simulation grade after acknowledged unlock/init | `src/devices/wahookickrsnapbike/wahookickrsnapbike.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `concept2-pm5` (80) | Concept2 PM5 rower | Case-insensitive `PM5` prefix with `ROW` suffix, or unnamed rowing service `ce060030-43e5-11e4-916c-0800200c9a66`; excludes Ski/Bike names and FTMS | Rowing speed, stroke rate (mapped to cadence), power, HR as available; individual or multiplexed status notifications | None; mechanical damper is not a controllable brake | Concept2 rowing characteristic layouts in `src/devices/concept2skierg/concept2skierg.cpp`, `src/devices/ftmsrower/ftmsrower.cpp`; `trainers_rower.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `horizon-tm`, `horizon-paragon` (80) | Horizon custom BLE / Paragon X | Case-insensitive Horizon/YPTM/HZ_T101/HZ_7.0AT/AFG SPORT/WLT2541 layouts; `PARAGON X` / `JFTMPARAGON` selects Paragon; no FTMS | Speed, incline | Speed/incline within model limits; connection never starts belt | `src/devices/horizontreadmill/horizontreadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `spirit-treadmill`, `spirit-xt485`, `spirit-xt800` (80) | Spirit XT385 / XT485 / XT800/900 | Case-insensitive corresponding `XT385`, `XT485`, `XT800` / `XT900` prefixes; no FTMS | Speed, incline as reported | Status-driven speed buttons; incline only on XT385; stops stepping on stale status or target crossing | `src/devices/spirittreadmill/spirittreadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `sole-treadmill`, `sole-f63`, `sole-f65`, `sole-tt8` (80) | Sole F80/F85/F89/S77/ST90/TRX7.5 / F63 / F65 / TT8 | Case-insensitive corresponding upstream model prefixes; no FTMS | Speed (mph converted to km/h), incline, HR | Status-driven speed/incline buttons; no unsolicited belt start | `src/devices/solef80treadmill/solef80treadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `walkingpad-r1`, `walkingpad-flat`, `walkingpad-re` (80) | Kingsmith / WalkingPad R1 Pro, A1/C2, RE | Case-insensitive upstream R1/KS prefixes; `RE` exact; excludes R2 names and FTMS-only variants | Speed | Speed only; flat walking variants capped at 6 km/h, R1 at 10 km/h; no incline | `src/devices/kingsmithr1protreadmill/kingsmithr1protreadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `walkingpad-r2`, `walkingpad-g1`, `walkingpad-g1b`, `wp2-c*-s*` (90) | Kingsmith R2 / X21 / K12 / G1 / MXG | Case-insensitive upstream `KS-ST-K12PRO`, `KS-R1AC`, `KS-HC-R1AA/AC`, X21, MXG, G1 prefixes; advertised custom services select UUID variant; nondefault cipher overrides are manual; no FTMS | Speed, step rate if reported | Speed only; G1 capped at 6 km/h; no incline | `src/devices/kingsmithr2treadmill/kingsmithr2treadmill.cpp`; `treadmills.hpp/.cpp`, pinned revision below | host-tested, not hardware-verified |
| `ftms` (50) | FTMS bike, treadmill, cross trainer, rower | Advertised Fitness Machine Service `0x1826` | Indoor Bike `0x2AD2`, Treadmill `0x2ACD`, Cross Trainer `0x2ACE`, Rower `0x2AD1`: speed/cadence/power/HR/incline where present; resistance decoded internally | Feature-gated power, resistance, incline, speed; Request Control handshake; power/resistance/incline honor supplied ranges; grade via bike simulation if needed. Speed is available via console/API, not FTMS output | Local Bluetooth FTMS codec and `ftms_source.hpp` / `src/ftms_source.cpp` | Host byte-layout, feature/range/control tests; hardware unverified |
| `cps` (40) | Cycling Power sensor | Service `0x1818` | Power, cadence derived from crank revolution/event deltas | None | Local Bluetooth CPS codec; `sensor_sources.hpp`, `src/sensor_sources.cpp`, `src/sensor_codec.cpp` | Host power/crank/wraparound tests; hardware unverified |
| `csc` (30) | Cycling Speed and Cadence sensor | Service `0x1816` | Speed (fixed **2.105 m** wheel circumference), cadence from wheel/crank deltas | None | Local Bluetooth CSC codec; sensor sources/codec | Host wheel/crank/wraparound tests; hardware unverified |
| `hrs` (20) | Heart Rate monitor | Service `0x180D` | Heart rate, 8- or 16-bit measurement | None | Local Bluetooth HRS codec; sensor sources/codec | Host 8/16-bit parser tests; hardware unverified |

Vendor ports are GPL-derived from
[`cagnulein/qdomyos-zwift`](https://github.com/cagnulein/qdomyos-zwift),
Copyright Roberto Viola and contributors (GPL-3.0). Attribution is retained in
`include/protocols/` and `src/protocols/`. The original ports do not record a
revision; new batch-1 ports use the pinned revision below.
Discovery rules follow upstream `src/devices/bluetooth.cpp`. See [license and
attribution](../README.md#license-and-attribution) and [HD-005](hardware-decisions.md#hd-005-gpl-30-licensing-for-qdomyos-zwift-ports).

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

A proposed batch 2 from the remaining upstream catalogue is
Domyos treadmills/rowers/ellipticals (`domyostreadmill`, `domyosrower`,
`domyoselliptical`), Echelon rower/stride/stair variants (`echelonrower`,
`echelonstride`, `echelonstairclimber`), Renpho bikes (`renphobike`) and FitShow
treadmills (`fitshowtreadmill`). These are audit candidates, not promised ports:
verify BLE transport and model-specific safety/control behavior first.

### Resource and validation notes

The configuration schema and BLE queues are unchanged. Only the selected source
protocol is instantiated; inactive adapters contribute code and registry metadata,
not one live protocol object per device.

Baseline `main` CI run
[`38041095684`](https://github.com/manuelh83/smartfitness-bridge/actions/runs/38041095684)
reports **1,151,201 bytes flash** and **62,948 bytes static RAM**. A local
PlatformIO build could not download the `espressif32` platform
(`HTTPClientError`, before compilation). A firmware flash/RAM delta must be
measured from a completed build; host object sizes are not ESP32 size measurements.
Host tests use upstream byte layouts and literal commands, not hardware captures.

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
- Resistance/gear is stored in protocol samples, but the public bridge metric
  set has no resistance metric: Dashboard/API expose the **control target**,
  not a live resistance measurement. Distance is also not forwarded into the
  bridge metric set by these sources.
- “Controllable” in the registry is a possibility, not a runtime guarantee:
  source subscription/init/Feature/Request Control must succeed first. Estimated
  vendor power/speed is not a calibrated physical measurement.
- FTMS output is always an indoor-bike peripheral, regardless of input profile.
  CPS/HRS here are **inputs only**, not Garmin-compatible output services.

For the extension steps (`SourceProtocol`, precise registry priorities, byte-sample
host tests and attribution headers), see [adding new protocols](../README.md#supported-devices-and-new-protocols).
