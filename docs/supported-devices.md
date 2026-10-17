# Supported input devices

**Not verified on hardware.** “Supported” means a matcher and decoder exist,
not certification that a particular model/firmware works. Host vectors exercise
the protocol layouts; vendor vectors are synthetic samples based on upstream
parsers/command bytes, not validated equipment captures.

The tables mirror `src/core/protocols/registry.cpp` (the table below and the
[batch 2](#batch-2-further-ble-only-vendor-protocols) families). Highest matching priority wins;
ties keep the first registered match. Pairing can override detection with a
registered id. Names are case-sensitive unless explicitly noted below.

The **FTMS output profile** column describes effective output with the `auto`
setting. A persisted override uses
`auto|bike|treadmill|cross-trainer|rower|step-climber|stair-climber`. Every
profile is sent natively with its own FTMS data characteristic (see
[FTMS machine profiles and data fields](#ftms-machine-profiles-and-data-fields)).
An override never adds missing source control capabilities.

FTMS input subscriptions follow all Fitness Machine Type bits in the current
advertisement's `0x1826` Service Data. For example, an indoor bike subscribes to
`0x2AD2` and Control Point `0x2AD9` (indicate), not treadmill/rower data.
Without Service Data, the last observed mask is retained only in RAM; if unknown,
only data characteristics actually present in GATT are subscribed. Feature/range
reads and control safety checks are unchanged. Every non-broadcast connection
attempt first checks for a fresh advertisement; `searching` is shown in both UIs.

| Id (priority) | Device / profile | Match rule | Metrics | Control | Source / attribution | Test status | FTMS output profile |
|---|---|---|---|---|---|---|---|
| `keiser-m3i` (90) | Keiser M3i | Name starts `M3` and manufacturer payload passes Keiser broadcast parser | Cadence, power, nonzero HR; gear decoded as resistance; no speed/distance forwarded | None; advertisement-only (no GATT connection) | qdomyos-zwift `src/devices/m3ibike/m3ibike.cpp`; `yesoul_keiser.hpp/.cpp` | Host broadcast/detection tests; hardware unverified | `bike` |
| `echelon` (80) | Echelon Connect Sport / EX bike protocol | Name starts `ECH`; excludes `ECH-ROW`, `ECH-STRIDE`, `ECH-SC` case-insensitively; must not advertise FTMS; names claimed by the higher-priority `echelon-rower`/`echelon-stride`/`echelon-stride4`/`echelon-stair` entries (batch 2) are not selected | Cadence, resistance; estimated power and speed from upstream formulas | Normalized resistance 0–100% mapped to levels 1–32; requires notification subscriptions; initialization frames are queued without acknowledgement gating | qdomyos-zwift `src/devices/echelonconnectsport/echelonconnectsport.cpp`; `echelon.hpp/.cpp` | Host frame/init/control tests; hardware unverified | `bike` |
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
| `sportstech-f15` (0, manual) | Sportstech F15, F31; Sportstech WalkingPad WP100, WP200; other FitHome/F37 OEM treadmills using this protocol | Manual pairing override only. The deniskie manifest advertises service `0xFFF0`, but it is generic and must not automatically match unrelated devices. Notify `0xFFF1`, write-without-response `0xFFF2`. All named models are reported compatible by their respective upstream projects, not hardware-verified in this bridge. | Full status frames: speed (0.1 km/h), integer incline (%), elapsed time (s), distance (m), calories (0.1 kcal), steps, and nonzero heart rate; running, paused and stopping states carry metrics. Error, safety, sleep and unknown states report speed 0. Status is polled every second (`02 51 51 03`); recognized and unknown status frames keep the connection alive. | Combined speed/incline frame `02 53 02 <speed×10> <incline %>`; speed/incline bounds queried with `50 02` / `50 03`, accepted up to 25.5 km/h and 25 % (protocol/core limits), with 0.8–12 km/h and 0–15 % fallbacks if no valid reply arrives. Speed/incline changes are allowed only while running; speed 0 sends stop `02 53 03` while starting, running or paused. The bridge never sends start, pause or light commands. | F15/F31: [`syssi/esphome-treadmill-f15`](https://github.com/syssi/esphome-treadmill-f15) (Apache-2.0), `components/treadmill_f15/` and `docs/protocol.md`, revision `61172d6448e00d0a4eb5d70c3c39a3427c292041`. WP100/WP200 and FitHome/F37 state/limit cross-check: [`deniskie/ha-sportstech-walkingpad-integration`](https://github.com/deniskie/ha-sportstech-walkingpad-integration) (MIT), `const.py`, `coordinator.py`, `manifest.json`, revision `f2f102710f3a7ca8c0671ea32316cb18b35274fd`; `sportstech.hpp/.cpp` | Host tests with upstream captured frame plus labeled synthetic vectors; all models hardware-unverified in this bridge | `treadmill` |
| `ftms` (50) | FTMS bike, treadmill, cross trainer, step climber, stair climber, rower | Advertised Fitness Machine Service `0x1826` | Indoor Bike `0x2AD2`, Treadmill `0x2ACD`, Cross Trainer `0x2ACE`, Step Climber `0x2ACF`, Stair Climber `0x2AD0`, Rower `0x2AD1`: every FTMS data field of the profile, only when the machine sends it (see field table below) | Feature-gated power, resistance, incline, speed; Request Control handshake; power/resistance/incline honor supplied ranges; grade via bike simulation if needed. Speed is available via console/API and treadmill FTMS output | Local Bluetooth FTMS codec and `ftms_source.hpp` / `src/core/ftms_source.cpp` | Host byte-layout, feature/range/control tests; hardware unverified | Derived from input data characteristic/service data: `bike`, `treadmill`, `cross-trainer`, `step-climber`, `stair-climber` or `rower` |
| `cps` (40) | Cycling Power sensor | Service `0x1818` | Power, cadence derived from crank revolution/event deltas | None | Local Bluetooth CPS codec; `sensor_sources.hpp`, `src/core/sensor_sources.cpp`, `src/core/sensor_codec.cpp` | Host power/crank/wraparound tests; hardware unverified | `bike` |
| `csc` (30) | Cycling Speed and Cadence sensor | Service `0x1816` | Speed (fixed **2.105 m** wheel circumference), cadence from wheel/crank deltas | None | Local Bluetooth CSC codec; sensor sources/codec | Host wheel/crank/wraparound tests; hardware unverified | `bike` |
| `hrs` (20) | Heart Rate monitor | Service `0x180D` | Heart rate, 8- or 16-bit measurement | None | Local Bluetooth HRS codec; sensor sources/codec | Host 8/16-bit parser tests; hardware unverified | `bike` |

Vendor ports are GPL-derived from
[`cagnulein/qdomyos-zwift`](https://github.com/cagnulein/qdomyos-zwift),
Copyright Roberto Viola and contributors (GPL-3.0). Attribution is retained in
`include/core/protocols/` and `src/core/protocols/`. The original ports do not record a
revision; new batch-1 ports use the pinned revision below.
Discovery rules follow upstream `src/devices/bluetooth.cpp`.
The Sportstech/FitHome port (`sportstech-f15`) is derived from
[`syssi/esphome-treadmill-f15`](https://github.com/syssi/esphome-treadmill-f15)
(Apache-2.0, compatible with GPL-3.0) at revision
`61172d6448e00d0a4eb5d70c3c39a3427c292041`; its attribution is kept in
`sportstech.hpp/.cpp`. Protocol states, limits, and metric offsets were also
cross-checked against [`deniskie/ha-sportstech-walkingpad-integration`](https://github.com/deniskie/ha-sportstech-walkingpad-integration)
(MIT) at revision `f2f102710f3a7ca8c0671ea32316cb18b35274fd`. Its
`manifest.json` matches `0xFFF0`, but this generic service is not used for
automatic matching. See [license and
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
- No distinct VeloCore proprietary BLE class/parser is present in the pinned
  upstream discovery/build inventory. VeloCore models using the existing
  Schwinn advertisement/Indoor Bike Data layout remain read-only through
  `schwinn-ic4`; other models require verified BLE evidence, not a guessed port.

The remaining BLE classes of the upstream catalogue are ported in
[batch 2](#batch-2-further-ble-only-vendor-protocols).

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

## Batch 2: further BLE-only vendor protocols

These families port the remaining BLE classes of qdomyos-zwift revision
[`ea70d7ff64bc8bf508fc6401fd056211e05311b3`](https://github.com/cagnulein/qdomyos-zwift/tree/ea70d7ff64bc8bf508fc6401fd056211e05311b3)
(classes or names that only exist later use
[`b1824db5379891d571a7ef2e219d20e6c7ec2c09`](https://github.com/cagnulein/qdomyos-zwift/tree/b1824db5379891d571a7ef2e219d20e6c7ec2c09),
recorded in the file header). Each family has its own header/source/test file and
is appended to `kFamilies` in `src/core/protocols/registry.cpp`:

| Family | Files | Upstream classes |
|---|---|---|
| FitShow | `fitshow.hpp/.cpp` | `fitshowtreadmill`, `snodebike`, `fitplusbike`, `fitplusrower` |
| Toorx / TRX App Gate | `toorx.hpp/.cpp` | `trxappgateusbtreadmill`, `trxappgateusbbike`, `trxappgateusbelliptical`, `trxappgateusbrower` (BLE despite the name) |
| Domyos (non-bike) | `domyos_ext.hpp/.cpp` | `domyostreadmill`, `domyoselliptical`, `domyosrower` |
| Echelon (non-bike) | `echelon_ext.hpp/.cpp` | `echelonrower`, `echelonstride`, `echelonstairclimber` |
| Nautilus / Bowflex | `nautilus.hpp/.cpp` | `nautilusbike`, `nautiluselliptical`, `nautilustreadmill`, `bowflextreadmill` |
| Sportstech / SportPlus / Pafers | `sportsplus.hpp/.cpp` | `sportstechbike`, `sportstechelliptical`, `sportstechrower`, `sportsplusbike`, `sportsplusrower`, `pafersbike`, `pafersrower`, `paferstreadmill` |
| Other treadmills | `vendor_treadmills.hpp/.cpp` | `truetreadmill`, `technogymmyruntreadmill` (BLE), `lifespantreadmill`, `octanetreadmill`, `ziprotreadmill`, `eslinkertreadmill`, `speraxtreadmill`, `focustreadmill`, `deeruntreadmill`, `activiotreadmill`, `wahookickruntreadmill`, `lifefitnesstreadmill`, `shuaa5treadmill` |
| Other bikes | `vendor_bikes.hpp/.cpp` | `flywheelbike`, `solebike`, `kettlerc12bike`, `technogymbike`, `inspirebike`, `keepbike`, `mcfbike`, `xcxbike`, `chronobike`, `volavabike`, `ultrasportbike`, `skandikawiribike`, `pitpatbike`, `freebeatboombike`, `mepanelbike`, `apexbike`, `npecablebike`, `cycleopsphantombike`, `renphobike`, `lifespanbike`, `horizongr7bike` (FTMS path), `cscbike` name rules |
| Other ellipticals / rowers / steppers | `vendor_cardio.hpp/.cpp` | `proformrower`, `bhfitnesselliptical`, `ypooelliptical`, `soleelliptical`, `octaneelliptical`, `smartrowrower`, `sunnyfitstepper`, `concept2skierg` |

Upstream branches that depend on a qdomyos-zwift user setting are registered as
**manual-only** entries (priority 0): they never match automatically and are
selected with the pairing override. Generic name prefixes (`HT`, `IC`, `AF`,
`LF`, `MD`, `CARE`, …) only match with upstream's exact extra conditions
(length, exclusions, services); otherwise they are manual-only. Except where a
row says otherwise, advertisements carrying FTMS `0x1826` stay with `ftms`.
Descriptor order is the persisted-id order: append only.

Treadmill safety applies to every family: the bridge never sends upstream
start-belt frames, stop frames on disconnect or stale data, or speed/incline
targets without fresh telemetry from a belt that is already running. Where
upstream's speed path is ambiguous (units, implicit start) speed control is not
exposed. The Domyos treadmill omits upstream's connect-time
`f0 c8 01 b9` ("set speed and incline to 0") frame so that connecting cannot
change a running belt; the Domyos elliptical/rower still send it.

All vectors are **synthetic samples in the upstream byte layout**; no entry in
this batch is verified on hardware.

| Id (priority) | Device / profile | Match rule | Metrics | Control | Source / attribution | Test status | FTMS output profile |
|---|---|---|---|---|---|---|---|
| `domyos-tm` (80) | Domyos / Decathlon treadmill (Chang Yow console) | Case-sensitive name prefix `Domyos`; excludes `DomyosBr*`, case-insensitive `DOMYOS-BIK*` (bike entry) and `DOMYOS-ROW*`, `Domyos-EL*`; must not advertise FTMS | Speed, incline, nonzero HR; device kcal/distance not forwarded (upstream only logs them); no power | Speed (0 < v ≤ 22 km/h, 0.1 km/h) and incline (0–20 %, 0.5 % steps, negative clamps to 0) via the combined speed/incline frame, only while the belt reports speed > 0 with data ≤ 2 s old; init never starts the belt, console start/stop buttons ignored, nothing sent on disconnect/stale data | qdomyos-zwift `src/devices/domyostreadmill/domyostreadmill.cpp`; `domyos_ext.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `domyos-el` (80) | Domyos / Decathlon elliptical (Chang Yow console) | Case-sensitive name prefix `Domyos-EL`, excludes `DomyosBridge`; must not advertise FTMS (FTMS models use `ftms`) | Speed, cadence, resistance, incline, device total kcal, nonzero HR; no power/distance | Normalized resistance 0–100% mapped to levels 1–15; incline 0–15 (upstream inclination frame) | qdomyos-zwift `src/devices/domyoselliptical/domyoselliptical.cpp`; `domyos_ext.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `domyos-row` (80) | Domyos / Decathlon rower (Chang Yow console) | Case-insensitive name prefix `DOMYOS-ROW`, excludes `DomyosBridge`; must not advertise FTMS (FTMS models use `ftms`) | Stroke rate, stroke count, 500 m pace and speed derived from it (upstream formula), resistance, device total kcal, nonzero HR; no power/distance | Normalized resistance 0–100% mapped to levels 1–15 | qdomyos-zwift `src/devices/domyosrower/domyosrower.cpp`; `domyos_ext.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `echelon-rower` (85) | Echelon Row / Row-S / Row-7S (rower) | Name starts `ECH-ROW`, `ROWSPORT`, `ROW-7S-` or `ROW-S` (case-insensitive); not advertising FTMS 0x1826 | Cadence (strokes/min), pace, speed (1800/pace, upstream formula, not 5 s averaged), resistance, estimated power (upstream watt table) | Resistance 1–32 (`f0 b1 01 <lvl> <sum>`) | qdomyos-zwift `echelonrower` (GPL-3.0, Roberto Viola and contributors) @ ea70d7f | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `echelon-stride` (85) | Echelon Stride / Stride 6S (treadmill) | Name starts `ECH-STRIDE`, `ECH-UK-`, `ECH-FR-`, `STRIDE` (not `STRIDE4`) or `ECH-SD-SPT` (case-insensitive); not `ECH-EC-SPT`; not advertising FTMS 0x1826 | Speed (console mph × 1.60934, upstream default), incline, heart rate | Incline 0–15 % (`f0 b1 01 <pct> <sum>`); no speed/start/stop | qdomyos-zwift `echelonstride` (GPL-3.0, Roberto Viola and contributors) @ ea70d7f | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `echelon-stride4` (86) | Echelon Stride 4 (treadmill) | Name starts `STRIDE4` (case-insensitive); not advertising FTMS 0x1826 | Speed (d1 frame, mph × 1.60934), incline | Incline 0–15 %; no speed/start/stop | qdomyos-zwift `echelonstride` STRIDE4 branch (GPL-3.0, Roberto Viola and contributors) @ ea70d7f | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `echelon-stair` (85) | Echelon stair climber | Name starts `ECH-EC-SPT` (case-insensitive); not advertising FTMS 0x1826 | Step count, positive elevation gain, cadence (steps/min), speed (cadence/3.2, upstream formula) | None (read-only) | qdomyos-zwift `echelonstairclimber` (GPL-3.0, Roberto Viola and contributors) @ ea70d7f | Host tests with synthetic upstream-layout frames; hardware unverified | `stair-climber` |
| `fitshow-tm` (80) | FitShow treadmill (FFF0/FFE0/AE00 vendor UART) | Case-sensitive `FS-` (excl. case-insensitive `FS-YK-`), case-insensitive `TR510-T`, `TUNTURI T80-` (these three even with FTMS, as upstream); without FTMS 0x1826: `SW` + length 14 without `(`/`)` (excl. `SWALK LITE-`), `SW` + length 15 with first `-` at index 10, case-insensitive `WINFITA`, `SW-BLE`, case-sensitive `BF70` | Speed (0 when not running), incline, device elapsed time, step count (when nonzero), HR (when nonzero), RSC 0x2A53 cadence; no distance/energy (upstream ignores the device values) | Speed and incline (`53 02 <speed×10> <incline>`) only while the last status (≤2 s old) is RUNNING, clamped to the limits reported by `50 02/03/05`; incline rounded to 1 %; never starts/stops/pauses the belt; commands retried 3× until echoed | qdomyos-zwift `src/devices/fitshowtreadmill/fitshowtreadmill.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `fitshow-noblepro` (80) | NoblePro Connect treadmill (FitShow protocol) | Case-insensitive `NOBLEPRO CONNECT`, no FTMS advertisement | As `fitshow-tm`; incline reported in 0.5 % device units ÷ 2 | As `fitshow-tm`; incline in 0.5 % steps, sent ×2 | qdomyos-zwift `src/devices/fitshowtreadmill/fitshowtreadmill.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `fitshow-tm-mi` (manual) | FitShow treadmill with mph console (upstream setting `fitshow_treadmill_miles`) | Manual pairing only | As `fitshow-tm`, speed converted mph→km/h | As `fitshow-tm`, speed sent in mph×10 | qdomyos-zwift `src/devices/fitshowtreadmill/fitshowtreadmill.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `fitshow-anyrun` (manual) | FitShow treadmill, AnyRun layout (upstream setting `fitshow_anyrun`) | Manual pairing only | As `fitshow-tm`; elapsed as min:s, big-endian step count | As `fitshow-tm` | qdomyos-zwift `src/devices/fitshowtreadmill/fitshowtreadmill.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `snode-bike` (80) | S-Node / `TF-` FTMS bike (upstream `snodebike`) | Case-insensitive `TF-` excluding `TF-T` (FTMS advertisement allowed, as upstream); `FS-` S-Node bikes (upstream setting `snode_bike`) by manual pairing | FTMS Indoor Bike Data (speed, cadence, power, HR, resistance as present) | None; read-only bike-data-only FTMS wrapper (upstream only forwards virtual-bike FTMS writes) | qdomyos-zwift `src/devices/snodebike/snodebike.cpp`; registry uses local FTMS codec | Host descriptor tests; hardware unverified | `bike` |
| `fitplus-bike` (manual) | FitPlus `FS-` bike (upstream setting `fitplus_bike`) | Manual pairing only | Speed, cadence (14-byte frames); no power (upstream estimates from HR) | None (upstream sends no resistance for this layout) | qdomyos-zwift `src/devices/fitplusbike/fitplusbike.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `fitplus-etappe` (80) | Virtufit Etappe 2.0i (FitPlus); `FS-` bikes with upstream setting `virtufit_etappe` by manual pairing | Case-sensitive `X100-`, no FTMS advertisement | Resistance, cadence, power (0 W at 0 rpm); no speed (upstream derives it from power) | Resistance 0–100 % → levels 1..max (24, or 8–32 reported by `02 41 02`), upstream lookup table | qdomyos-zwift `src/devices/fitplusbike/fitplusbike.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `fitplus-merach` (80) | Merach MRK bikes (FitPlus) | Case-sensitive `MRK-`, no FTMS; excludes upstream's earlier `MRK-R11S-`, `MRK-E`, `MRK-T`, `MRK-S26S-`, `MRK-S26C-`, `MRK-S28-`, `MRK-S38-`, `MRK-R28-`, `MRK-CRYDN-`, `MRK-R06-`, `MRK-R15-` (case-insensitive) | Resistance, cadence, power (0 W at 0 rpm) | Resistance 0–100 % → levels 1–24, upstream lookup table | qdomyos-zwift `src/devices/fitplusbike/fitplusbike.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `fitplus-h9110` (80) | H9110 Osaka bike (FitPlus) | Case-insensitive `H9110 OSAKA`, no FTMS | Resistance, cadence, power (0 W at 0 rpm) | Resistance 0–100 % → levels 1–32, upstream lookup table | qdomyos-zwift `src/devices/fitplusbike/fitplusbike.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `fitplus-sx600` (manual) | Sportstech SX600 (upstream setting `sportstech_sx600`) | Manual pairing only | With FTMS 0x2AD2: speed, cadence, resistance (÷10), power, energy, HR; otherwise FitPlus 15-byte resistance/cadence/power | Resistance 0–100 % → levels 1–32, SX600 lookup table | qdomyos-zwift `src/devices/fitplusbike/fitplusbike.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `fitplus-rower` (80) | Merach R28 rower (FitPlus) | Case-insensitive `MRK-R28-` (FTMS advertisement allowed: upstream documents the R28 advertises FTMS but uses FFF0) | Stroke rate (cadence), resistance, power (0 W at 0 spm); no speed/distance (upstream derives them) | Resistance 0–100 % → levels 1–24, upstream lookup table | qdomyos-zwift `src/devices/fitplusrower/fitplusrower.cpp`; `fitshow.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `nautilus-bike` (80) | Nautilus bike (B628/B616) | Case-insensitive `NAUTILUS B` prefix; no FTMS advertisement | Speed, power, cadence, nonzero HR, resistance as reported (0.01 mph speed; B616 service `f755c9cf` reports 0.01 km/h) | None (upstream sends only the init frame) | qdomyos-zwift `src/devices/nautilusbike/nautilusbike.cpp`; `nautilus.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `nautilus-ellip` (80) | Nautilus elliptical (E616/E618, `NAUTILUS 616`) | Case-insensitive `NAUTILUS E`, `NAUTILUS M` (except `NAUTILUS M7`) or `NAUTILUS 616` prefix; no FTMS | Speed, cadence, nonzero HR, resistance per service variant | None | qdomyos-zwift `src/devices/nautiluselliptical/nautiluselliptical.cpp`; `nautilus.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `nautilus-m7` (80) | Nautilus M7 elliptical | Case-insensitive `NAUTILUS M7` prefix; no FTMS | Speed, cadence, power, HR, resistance (split 0x31/0x5e frames on variant-1 services) | None | qdomyos-zwift `src/devices/nautiluselliptical/nautiluselliptical.cpp`; `nautilus.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `nautilus-tm` (80) | Nautilus / Schwinn treadmill | Case-insensitive `NAUTILUS T` (except `NAUTILUS T628`), `SCHWINN T` or `SCHWINN 570T` prefix; no FTMS | Speed, incline | None; read-only (upstream speed/incline setters are empty) | qdomyos-zwift `src/devices/nautilustreadmill/nautilustreadmill.cpp`; Schwinn names from `src/devices/bluetooth.cpp` at b1824db; `nautilus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` (read-only) |
| `nautilus-t628` (80) | Nautilus T628 treadmill | Case-insensitive `NAUTILUS T628` prefix; no FTMS | Speed only (upstream T628 workaround ignores the incline byte) | None | qdomyos-zwift `src/devices/nautilustreadmill/nautilustreadmill.cpp`; `nautilus.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` (read-only) |
| `nautilus-el-v2` (0) | Nautilus elliptical, service `7df7a3f7` | Manual selection only (upstream 5th service fallback; exceeds plan subscription limit) | Speed (mph frames), cadence, nonzero HR, resistance | None | qdomyos-zwift `src/devices/nautiluselliptical/nautiluselliptical.cpp` (bt_variant 2); `nautilus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `nautilus-tm-688a` (0) | Nautilus treadmill, service `688a15a0` | Manual selection only (service fallback added upstream at b1824db) | Speed, incline | None; read-only | qdomyos-zwift `src/devices/nautilustreadmill/nautilustreadmill.cpp` at b1824db; `nautilus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` (read-only) |
| `bowflex-tm` (0) | Bowflex treadmill (legacy `bowflextreadmill` class) | Manual selection only (upstream never instantiates this class; `BOWFLEX T` names go to `bowflex-t216`) | Speed, incline, console kcal | None; read-only | qdomyos-zwift `src/devices/bowflextreadmill/bowflextreadmill.cpp`; `nautilus.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` (read-only) |
| `nautilus-b616-mi` (0) | Nautilus B616 bike with mph console | Manual selection only (upstream `miles_unit` setting on B616 service) | As `nautilus-bike`, B616 speed treated as 0.01 mph | None | qdomyos-zwift `src/devices/nautilusbike/nautilusbike.cpp`; `nautilus.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `sportstech-bike` (80) | Sportstech bike (ESX500/ESX600 class, AF7019) | Case-insensitive `EW-BK` prefix, or `AF` prefix with name length exactly 7; no FTMS advertisement | Speed, cadence, packet power (0 W at 0 rpm), kcal, resistance (byte 15), nonzero HR | Resistance 0–100% → level 0–23 carried in the 200 ms keep-alive poll (`f2 c3 07 04 <lvl> … <0xbe+lvl>`); paced init (4 frames, reply or 300 ms) | qdomyos-zwift `src/devices/sportstechbike/sportstechbike.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `sportstech-ellip` (80) | Sportstech elliptical | Case-insensitive `EW-EP-` prefix; no FTMS advertisement | Speed, cadence, packet power, kcal, resistance (byte 15), nonzero HR | Resistance 0–100% → level 0–23 in the keep-alive poll | qdomyos-zwift `src/devices/sportstechelliptical/sportstechelliptical.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `sportstech-rower` (80) | Sportstech rower | Case-insensitive `EW-ST-` prefix; no FTMS advertisement | Speed, stroke rate, packet power, kcal, resistance, nonzero HR | None (upstream always polls level 0) | qdomyos-zwift `src/devices/sportstechrower/sportstechrower.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `sportsplus-bike` (80) | SportsPlus bike (CARDIOFIT, HT non-numeric) | Case-insensitive `CARDIOFIT` prefix, or `HT` prefix with length 10 and non-numeric suffix (upstream default `ftms_bike` setting); not EW-BK/AF; no FTMS advertisement | Speed (`0x20` frames), cadence = speed×10+12 (upstream estimate), power (`0x30` frames), kcal, nonzero HR | Resistance 0–100% → level 0–24 in the 200 ms poll `20 01 <lvl> 00 <0x21+lvl>` (starts at level 1 like upstream) | qdomyos-zwift `src/devices/sportsplusbike/sportsplusbike.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `sportsplus-ht` (80) | SportsPlus HT bike | Case-insensitive `HT` + 8 digits (length 10); no FTMS advertisement | BCD speed, cadence from upstream raw-speed mapping (+12), BCD power, nonzero HR | Resistance 0–100% → level 0–24 in the poll | qdomyos-zwift `src/devices/sportsplusbike/sportsplusbike.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `carefit-bike` (80) | Carefitness CV-351 bike | Name contains `CARE` (case-insensitive) with length exactly 11; not EW-BK/AF; no FTMS advertisement | BCD cadence; speed = 0.375×cadence and power from upstream's 24×6 watt table (estimates); nonzero HR | Resistance 0–100% → level 0–24 in the poll (written to FFF2) | qdomyos-zwift `src/devices/sportsplusbike/sportsplusbike.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `sp-ht-9600ie` (0) | SportsPlus HT-9600iE bike | Manual pairing only (upstream `sp_ht_9600ie` setting) | Speed, cadence = speed×2.685 (upstream estimate), power, kcal, nonzero HR | Resistance 0–100% → level 0–24 in the poll | qdomyos-zwift `src/devices/sportsplusbike/sportsplusbike.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `sportsplus-rower` (80) | SportsPlus / Carefitness / VMAX / XM Fitness rower | Name contains `CARE` with length ≥ 12, or `VMAX` prefix (case-insensitive); not an earlier rule; no FTMS advertisement | BCD stroke rate; speed = 0.375×rate and power via Concept2 pace formula (upstream estimates); nonzero HR | Resistance 0–100% → level 0–24 in the poll; write endpoint FFF2 or FFE2 (XM Fitness init) found by GATT discovery | qdomyos-zwift `src/devices/sportsplusrower/sportsplusrower.cpp`, `src/devices/rower.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `pafers-bike` (80) | Pafers bike | Case-insensitive `PAFERS_` prefix (upstream default: pafers_treadmill/pafers_rower off); no FTMS advertisement | Speed (integer km/h), cadence, resistance, power | Resistance 0–100% → level 1–24 (`55 11 01 <lvl>`), sent only when it differs from the reported level | qdomyos-zwift `src/devices/pafersbike/pafersbike.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `pafers-rower` (0) | Pafers rower | Manual pairing only (upstream `pafers_rower` setting) | Speed, stroke rate, resistance, power | Resistance 0–100% → level 1–24 | qdomyos-zwift `src/devices/pafersrower/pafersrower.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `pafers-treadmill` (0) | Pafers treadmill | Manual pairing only (upstream `pafers_treadmill` setting) | Speed, incline | Speed 1–22 km/h and incline 0–15 % (integers, upstream speed→400 ms→incline pair) only while the belt runs (≥1 km/h) with data <2 s old; never sends start; console stop event is acknowledged with stop | qdomyos-zwift `src/devices/paferstreadmill/paferstreadmill.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `pafers-iboxster` (0) | BH i.Boxster Plus (Pafers treadmill variant) | Manual pairing only (upstream `pafers_treadmill_bh_iboxster_plus` setting) | Speed, incline | Same as `pafers-treadmill`; variant init frames | qdomyos-zwift `src/devices/paferstreadmill/paferstreadmill.cpp`; `sportsplus.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `id` (priority) | Device / profile | Match rule | Metrics | Control | Source / attribution | Test status | FTMS output profile |
| `trx-tm` (80) | Toorx / V-RUN / K80 treadmill (TRX App Gate, FFF0) | Case-sensitive `TOORX` or `V-RUN`, case-insensitive `K80_` | Speed, incline (19-byte reassembled frames) | Incline 0–15 % (`f0 ac 01 d3 <inc+1> 64 64 sum`, integer %) only with fresh (≤2 s) telemetry; no speed control; never starts/stops the belt | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-irun` (80) | i-Running / iConsole+ treadmill (ISSC, FFF0 fallback) | Case-insensitive `I-RUNNING`, `I-CONSOLE+`, `ICONSOLE+` | Speed, incline | As `trx-tm` | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-reebok` (80) | Reebok treadmill (TRX, ISSC, FFF0 fallback) | Case-insensitive `REEBOK` | Speed, incline | As `trx-tm` | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-adidas` (80) | Adidas treadmill (TRX, FFF0) | Case-insensitive `ADIDAS ` (with space) | Speed, incline | As `trx-tm`, address `5b d3`, write with response | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-dknrun` (80) | DKN Run treadmill (ISSC + second notify) | Case-insensitive `DKN RUN`, name shorter than 11 characters (longer names go to FTMS as upstream) | Speed, incline (18-byte frames) | As `trx-tm`, write with response | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-30` (manual) | Toorx treadmill, Toorx 3.0 protocol (upstream setting `toorx_3_0`) | Manual pairing only | Speed, incline | As `trx-tm` | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-jtx` (manual) | JTX Fitness Sprint treadmill (upstream setting `jtx_fitness_sprint_treadmill`) | Manual pairing only | Speed, incline | As `trx-tm` | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-dkn` (manual) | DKN Endurun treadmill (upstream setting `dkn_endurun_treadmill`) | Manual pairing only | Speed, incline | As `trx-tm`, write with response | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-flow` (manual) | Flow Fitness DTM2000i treadmill (upstream setting `flow_fitness_runner_dtm2000i`, `TREADMILL…` names) | Manual pairing only | Speed, incline (`f0 b0/b2 2e d3` frames; ACKs ignored) | As `trx-tm`, address `2e d3` | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-tm-fr30` (manual) | Reebok FR30 treadmill (upstream setting `reebok_fr30_treadmill`) | Manual pairing only | Speed, incline | As `trx-tm`, address `32 d3` | qdomyos-zwift `src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `trx-bike-tc` (80) | TRX App Gate `TC###` bike (ISSC) | Case-insensitive `TC` + 3 digits (exactly 5 characters), address not starting `E8` | Speed, cadence, power (0 at 0 rpm), resistance, energy, HR (when nonzero) | Resistance 0–100 % → levels 1–32 (`f0 a6 01 01 <lvl+1> sum`) | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-tun` (80) | Tunturi bike (TRX) | Case-insensitive `TUN ` | Cadence, power, resistance, HR; speed estimated from cadence (×0.374, as upstream) | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-fhw` (80) | FitHiWay bike | Case-insensitive `FITHIWAY` or `FIT HI WAY` | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-vfit` (80) | VirtuFit bike (TRX; `VIFHTR2.1` names need upstream `toorx_bike`, pair manually) | Case-insensitive `VIRTUFIT` | As `trx-bike-tc` | Resistance levels 1–32, address `1e 01` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-irun` (80) | iBiking+ / RI009R bike (i-Running protocol; iConsole protocol when the second notify is absent, as upstream) | Case-insensitive `IBIKING+` or containing `RI009R`, address not starting `E8` | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-adc1` (80) | Adidas C1 bike | Case-insensitive `ADIDAS` followed by 4+ digits only | As `trx-bike-tc` | Resistance levels 1–32, address `5b 01` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-chy` (80) | Changyow bike (TRX) | Address starting `E8` and name `TC###`, `IBIKING+`, containing `RI009R` or `PASYOU-` | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-icon` (manual) | Toorx / iConsole+ / CR011R bike (upstream setting `toorx_bike` with `TOORX`, `ICONSOLE+`, `I-CONSOLE+`, `…CR011R…` names) | Manual pairing only | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-casall` (manual) | Casall `I-CONSOIE+` bike (upstream setting `toorx_bike`) | Manual pairing only | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-reebok` (manual) | Reebok bike (TRX, upstream setting `toorx_bike` with `REEBOK` names) | Manual pairing only | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-dkn` (manual) | DKN Motion bike (upstream setting `toorx_bike` with `DKN MOTION` names) | Manual pairing only | As `trx-bike-tc` | Resistance levels 1–32 (address `02 01` on the ISSC fallback service) | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-fal` (manual) | FAL-SPORTS bike (upstream setting `toorx_bike` with `FAL-SPORTS` names) | Manual pairing only | As `trx-bike-tun` | Resistance levels 1–32, address `1b 01` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-hertz` (manual) | Hertz XR-770 bike (upstream setting `hertz_xr_770`) | Manual pairing only | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-enerfit` (manual) | Enerfit SPX 9500 bike (upstream setting `enerfit_SPX_9500`) | Manual pairing only | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-hammer` (manual) | Hammer Speed Bike S (upstream setting `enerfit_SPX_9500` with `IBIKING+` names) | Manual pairing only | Cadence, HR; speed estimated from cadence (as upstream) | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-jll` (manual) | JLL IC400 bike (upstream setting `jll_IC400_bike`) | Manual pairing only | As `trx-bike-hammer` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-fytter` (manual) | Fytter RI-08 bike (upstream setting `fytter_ri08_bike`) | Manual pairing only | Cadence, power, HR; speed estimated from cadence (as upstream) | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-asviva` (manual) | Asviva bike (upstream setting `asviva_bike`) | Manual pairing only | As `trx-bike-hammer` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-hop` (manual) | Hop-Sport HS-090H bike (upstream setting `hop_sport_hs_090h_bike`) | Manual pairing only | As `trx-bike-tc` | Resistance levels 1–32, address `3f 01` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-srx500` (manual) | Toorx SRX 500 bike (upstream setting `toorx_bike_srx_500`) | Manual pairing only | As `trx-bike-tc` | Resistance levels 1–32, address `23 01` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-bike-taurua` (manual) | Taurua IC90 bike (upstream setting `taurua_ic90`) | Manual pairing only | As `trx-bike-tc` | As `trx-bike-tc` | qdomyos-zwift `src/devices/trxappgateusbbike/trxappgateusbbike.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `bike` |
| `trx-ell` (80) | FAL-SPORTS elliptical (TRX, FFF0; JTX Fitness layout on ISSC, as upstream) | Case-insensitive `FAL-SPORTS` | Speed, cadence, resistance, power (JTX layout: cadence, resistance, speed = cadence × 0.33 as upstream default) | Resistance 0–100 % → levels 1–15, address `35 01` (`01 01` for JTX) | qdomyos-zwift `src/devices/trxappgateusbelliptical/trxappgateusbelliptical.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `trx-ell-toorx` (manual) | Toorx elliptical (upstream setting `iconsole_elliptical` with `TOORX` names) | Manual pairing only | Speed, cadence, resistance, power | Resistance levels 1–32, address `23 01` | qdomyos-zwift `src/devices/trxappgateusbelliptical/trxappgateusbelliptical.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `trx-ell-dct` (manual) | iConsole+ elliptical, DCT2000i (ISSC) / Taurus FX9.9 (FFF0 fallback) (upstream setting `iconsole_elliptical` with `I-CONSOLE+` names) | Manual pairing only | Speed, cadence, resistance, power | Resistance levels 1–15, address `01 01` | qdomyos-zwift `src/devices/trxappgateusbelliptical/trxappgateusbelliptical.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `trx-row` (manual) | iConsole+ rower (upstream setting `iconsole_rower` with `I-CONSOLE+` names) | Manual pairing only | Speed, stroke rate, resistance, power | Resistance 0–100 % → levels 1–15 (`f0 a6 35 01 …`) | qdomyos-zwift `src/devices/trxappgateusbrower/trxappgateusbrower.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `trx-row-mrk` (manual) | Mearch NovaRow R50 rower (upstream setting `iconsole_rower` with `MRK-R11S-` names) | Manual pairing only | Elapsed, strokes, stroke rate, distance, energy, HR, power, resistance (no speed: upstream formula has broken units) | As `trx-row` | qdomyos-zwift `src/devices/trxappgateusbrower/trxappgateusbrower.cpp`; `toorx.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `flywheel` (80) | Flywheel / Life Fitness IC8 bike, IndoorBike | `Flywheel` | Power, cadence, speed, resistance (brake level) | None | qdomyos-zwift flywheelbike.cpp (stream id 12) | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `lifefit-ic5` (0) | Life Fitness IC5 bike, IndoorBike | Manual only (upstream: `life_fitness_ic5` setting) | Power, cadence, speed, resistance | None | flywheelbike.cpp (IC5 init) | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `sole-bike` (80) | Sole LCB/LCR bike, IndoorBike | `LCB`, `LCR` | Power, cadence, speed, resistance | None | solebike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `sole-r92` (80) | Sole R92 recumbent, IndoorBike | `R92` | Power, cadence, speed, resistance | None | solebike.cpp (r92 path) | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `kettler-c12` (80) | Kettler C12 / Ergo C12, IndoorBike | `KETTLER C12`, `ERGO C12` | Power, speed, distance, resistance, cadence (CPS/CSC) | None | kettlerc12bike.cpp (638af000 service) | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `technogym` (80) | Technogym MyCycling, IndoorBike | `MYCYCLING` | Power, cadence (CPS) | Resistance 1..100, target power | technogymbike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `inspire` (80) | Inspire IC bike, IndoorBike | `IC` and exactly 8 chars (not `IC BIKE`) | Cadence, resistance, estimated power/speed | None | inspirebike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `keep-bike` (80) | Keep bike, IndoorBike | `KEEP_BIKE_`, `KEEP_CC_` | Power, cadence, resistance | None | keepbike.cpp (legacy protocol) | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `mcf-bike` (80) | MCF bike, IndoorBike | `MCF-` | Power, cadence, speed, resistance | None | mcfbike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `xcx-bike` (80) | XCX bike, IndoorBike | `XCX-` (also with FTMS, as upstream) | Power, cadence, speed, distance, energy, resistance | None | xcxbike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `chrono-bike` (80) | Chrono bike, IndoorBike | `CHRONO ` | Power, cadence, speed | None | chronobike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `volava` (80) | Volava bike, IndoorBike | `XQ` and exactly 12 chars | Power, cadence, resistance, estimated speed | None | volavabike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `ultrasport` (80) | Ultrasport X-Bike, IndoorBike | `X-BIKE` | Power, heart rate | None | ultrasportbike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `skandika-wiri` (80) | Skandika Wiri, IndoorBike | `BFCP`, or `HT` and exactly 12 chars | Power, cadence, speed, energy, heart rate, resistance | Resistance 1..32 | skandikawiribike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `skandika-x2000` (80) | Skandika X-2000, IndoorBike | `HT` and exactly 11 chars | Cadence, speed, energy, heart rate, resistance | Resistance 1..32 | skandikawiribike.cpp (X-2000 path) | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `pitpat-bike` (80) | PitPat bike, IndoorBike | `PITPAT-S` | Power, cadence, resistance, speed (+distance/energy/HR on legacy) | Resistance 1..32 | pitpatbike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `freebeat-boom` (80) | Freebeat Boom, IndoorBike | `FEAQ51A_` | Cadence, resistance, estimated power/speed | Resistance 1..100 | freebeatboombike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `mepanel` (80) | MEPANEL bike, IndoorBike | `MEPANEL` | Power, cadence, speed, heart rate | None | mepanelbike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `apex-bike` (80) | Apex bike, IndoorBike | `WLT8266BM` | Cadence, resistance, table power, estimated speed | None | apexbike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `npe-cable` (80) | NPE CABLE bike, IndoorBike | `>CABLE`, `MD`+7 chars, `BIKE`+6 chars | FTMS/CPS/CSC/HR metrics, NUS resistance for `BIKE ` names | None | npecablebike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `cycleops` (80) | CycleOps Phantom / Magnus, IndoorBike | `INDOORCYCLE`, `MAGNUS ` | Power, cadence, HR, FTMS bike data | Target power (ERG) | cycleopsphantombike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `csc-bike` (80) | Joroto BK / BGYM CSC bike, IndoorBike | `JOROTO-BK-`, `BGYM`+8 chars | Cadence | None | cscbike.cpp (bluetooth.cpp special names) | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `lifespan-bike` (0) | LifeSpan bike, IndoorBike | Manual only (upstream: `lifespan_bike` setting; `LIFESPAN` belongs to the treadmill family) | Power, cadence, speed, distance, energy, resistance | None | lifespanbike.cpp | Host tests with synthetic upstream-layout frames; hardware unverified | `indoor bike` |
| `renpho` (80) | Renpho bike (FTMS without advertised 0x1826), IndoorBike | `RQ`+5 chars, `R-Q`+>6 chars, `SCH130` | FTMS indoor bike data | FTMS control (FtmsSource) | renphobike.cpp (standard FTMS) | Host tests (matcher/create); FTMS codec covered by generic tests; hardware unverified | `indoor bike` |
| `horizon-gr7` (80) | Horizon GR7 (FTMS path), IndoorBike | `JFIC` | FTMS indoor bike data | FTMS control (FtmsSource) | horizongr7bike.cpp (FTMS path only) | Host tests (matcher/create); hardware unverified | `indoor bike` |
| `proform-rower` (80), `proform-750r` (0), `proform-sport-rl` (0) | ProForm / NordicTrack iFit rower; 750R and Sport RL variants are manual-only (upstream settings `proform_rower_750r` / `proform_rower_sport_rl`) / rower | Case-insensitive `I_RW` prefix; when FTMS `0x1826` is advertised, also requires the iFit service `00001533-1412-efde-1523-785feabcd123` | Stroke rate (cadence), speed and pace from the 500 m split, power, resistance level, distance | Resistance 1-24 through the model's upstream frame table, sent in the poll cycle (750R / Sport RL add an unlock frame) | `src/devices/proformrower/proformrower.cpp`; `vendor_cardio.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `bh-elliptical` (80) | BH Fitness elliptical / cross trainer | Case-insensitive `B01_` prefix | Indoor Bike Data `0x2AD2`: cadence (raw/2), speed estimated as cadence/10 (as upstream), distance, resistance, power, kcal, HR; HR `0x2A37` | Resistance 1-72 as an FTMS bike-simulation grade `(level-6)*33` (upstream) | `src/devices/bhfitnesselliptical/bhfitnesselliptical.cpp`; `vendor_cardio.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `ypoo-u3` (80), `ypoo-ftms-el` (80), `ypoo-sch411` (80), `ypoo-feier` (80), `ypoo-sole-e25` (80), `ypoo-true-el` (80), `ypoo-iconsole` (0), `gymstick-gx60` (0) | YPOO U3 / Merach MRK-E; FTMS-layout ellipticals (Schwinn 590E, E35 with FTMS, Kettler, Cardiopower EEGO, MyElliptical, MX-AS, SF-&lt;number&gt;); Schwinn 411/510E; FEIER; Sole E25 with FTMS; TRUE elliptical; iConsole and Gymstick GX6.0 (manual) / cross trainer | Case-insensitive upstream prefixes `YPOO-U3-`, `MRK-E` / `SCH_590E`, `KETTLER `, `CARDIOPOWER EEGO`, `MYELLIPTICAL `, `MX-AS `, `SF-`+positive number, `E35`+FTMS / `SCH411/510E` / `FEIER-EM-` / `E25`+FTMS / `TRUE ELLIPTICAL `+3 or more digits | Cross Trainer Data `0x2ACE` interpreted per upstream variant (speed or average speed, distance, cadence, stride count, incline, resistance, power, HR, METs); Indoor Bike Data `0x2AD2`; HR `0x2A37`; iConsole 15-byte frames (speed, cadence) | FTMS variants: request control (+ start for FEIER), resistance `04 <level*10>` (levels 1-25), inclination (not TRUE). U3/iConsole: proprietary `02 44 05 L 00 39+L 03` (1-72); Gymstick delta frame | `src/devices/ypooelliptical/ypooelliptical.cpp`; `vendor_cardio.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `sole-elliptical` (80), `xterra-xe88` (80), `sole-e55` (0) | Sole E25/E35/E55/E95/E95S/E98/E98S/XG400 elliptical, Xterra XE88, Sole E55 init variant (manual, upstream setting `sole_elliptical_e55`) / cross trainer | Case-insensitive prefixes `E95S`, `E25`, `E35`, `E55`, `E95`, `E98`, `XG400` / `XE88`; no FTMS | Speed (mph converted to km/h), kcal, power, cadence, HR; resistance/level status frames | Resistance stepped one level per console status acknowledgement (1-20; XE88 1-32); gives up after 3 s with no status | `src/devices/soleelliptical/soleelliptical.cpp`; `vendor_cardio.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `octane-q37` (80) | Octane Q37 elliptical / cross trainer | Case-insensitive `Q37` prefix; no FTMS | Cadence; speed estimated from cadence (upstream formula); resistance; HR | None (upstream has no control) | `src/devices/octaneelliptical/octaneelliptical.cpp`; `vendor_cardio.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `cross-trainer` |
| `smartrow` (80) | SmartRow rower sensor / rower | Case-insensitive `SMARTROW` prefix, excluding `SMARTROWER`; no FTMS | Distance, power, stroke rate, stroke count, pace (s/500 m), speed; V3 KEYLOCK handshake | None (upstream forceResistance is a no-op) | `src/devices/smartrowrower/smartrowrower.cpp`; `vendor_cardio.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `sunnyfit-stepper` (80) | Sunny Health & Fitness stepper / step climber | Case-insensitive `SF-S` prefix; no FTMS | Step rate (cadence), step count, elevation gain estimated as 0.2 m per step (upstream) | None | `src/devices/sunnyfitstepper/sunnyfitstepper.cpp`; `vendor_cardio.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `step-climber` |
| `concept2-skierg` (80) | Concept2 PM5 SkiErg / rower | Case-insensitive `PM5` prefix with `SKI` suffix; no FTMS | Multiplexed PM5 status `ce060080`: distance, speed, stroke rate, power, HR, kcal, stroke count, drag factor (as resistance) | None | `src/devices/concept2skierg/concept2skierg.cpp`; `vendor_cardio.hpp/.cpp`, pinned revision below | Host tests with synthetic upstream-layout frames; hardware unverified | `rower` |
| `true-tm`, `assault-tm`, `wdway-tm` (80) | TRUE / Assault / WDWAY treadmills | `TRUE*` except `TRUE TREADMILL <3+ digits>`, `TRUE ELLIPTICAL <3+ digits>`, `TRUE 1000`; `TREADMILL*` without RSC 0x1814; `ASSAULT TREADMILL `; `WDWAY` with exactly 8 characters; no FTMS | Speed, incline (TRUE/WDWAY) | None (upstream read-only) | `src/devices/truetreadmill/truetreadmill.cpp`; `vendor_treadmills.hpp/.cpp`, revision `ea70d7ff64bc8bf508fc6401fd056211e05311b3` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `technogym-myrun` (80) | Technogym MyRun (BLE), Merach U3 | `MYRUN `, `MERACH-U3` (FTMS allowed, as upstream) | FTMS treadmill data (speed, incline, energy, HR, …), RSC cadence | Authenticated FTMS Request Control at connect; speed `02` 0.1–22 km/h, incline `03` 0–15 % only while running; speed 0 sends FTMS Stop `08 01`; never sends Start | `src/devices/technogymmyruntreadmill/technogymmyruntreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `lifespan-tm` (80) | LifeSpan treadmills | `LIFESPAN`; no FTMS | Speed, steps (polled every 500 ms) | None (upstream never sends speed) | `src/devices/lifespantreadmill/lifespantreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `octane-zr7`, `octane-zr8` (80) | Octane Zero Runner ZR7 / ZR8 | `ZR7` / `ZR8`; no FTMS | Speed from pace; ZR8 also cadence (fragment reassembly, upstream zero-cadence stop rules) | None (upstream read-only) | `src/devices/octanetreadmill/octanetreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `zipro-tm` (80) | ZiPro treadmills | `RZ_TREADMIL`; no FTMS | Speed, incline, HR | Speed 0.1–22 km/h and incline 0–15 % via the forced poll frame, only while running; speed 0 sends stop ×3; never sends start | `src/devices/ziprotreadmill/ziprotreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `esanglinker` (80), `eslinker-costa` (0, manual) | ESangLinker; ESLinker Costaway | `ESANGLINKER`, no FTMS; Costaway manual only | Speed (mph frames converted) | Speed 0.1–22 km/h only while running; speed 0 sends stop tape; handshake/pair replies; the upstream init start-tape frame is omitted | `src/devices/eslinkertreadmill/eslinkertreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `sperax-tm` (80) | Sperax walking pads | `SPERAX_RM01`, `SPERAX_RM-02`; no FTMS | Speed, steps | Speed 0.1–6.0 km/h from the verbatim upstream frame table, only while running; speed 0 sends stop | `src/devices/speraxtreadmill/speraxtreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `focus-tm` (80) | Focus treadmills | `EW-TM-`; no FTMS | Speed, incline | Combined speed/incline frame (0.1–22 km/h, 0–15 %) only while running; speed 0 sends stop; never sends start | `src/devices/focustreadmill/focustreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `deerrun-pitpat` (80) | DeerRun / PitPat treadmills (FBA0 variant) | `PITPAT-T`; no FTMS | Speed, incline | Combined speed/incline frame (0.1–22 km/h, 0–15 %) only while running; speed 0 sends stop; the "pitpat start" init frame is omitted | `src/devices/deeruntreadmill/deerruntreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `activio-tm` (80), `activio-v460`, `activio-zt2500`, `activio-umay` (0, manual) | Activio / RunnerT; FitFiu MC-V460, Zero ZT2500, Umay S100 variants | `RUNNERT`, no FTMS; variants manual only (upstream settings) | Speed, HR, incline (not V460) | Speed 0.1–22 km/h, incline 0–15 % (not V460) only while running; speed 0 sends stop; never sends start | `src/devices/activiotreadmill/activiotreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `kickr-run` (80) | Wahoo KICKR RUN | `KICKR RUN` (FTMS allowed, as upstream) | Belt speed (vendor telemetry), incline (FTMS) | Speed 0.1–22 km/h (vendor command; the paddle must confirm), incline −3–15 % via FTMS, only while running; speed 0 sends vendor speed 0; never sends FTMS Start/Resume | `src/devices/wahookickruntreadmill/wahookickruntreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests with synthetic upstream-layout frames; hardware unverified | `treadmill` |
| `lifefitness-tm` (80) | Life Fitness treadmills | `LF` with name length 15 or 18 (FTMS allowed, as upstream) | FTMS treadmill data | None (upstream controls are commented out) | `src/devices/lifefitnesstreadmill/lifefitnesstreadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests; hardware unverified | `treadmill` |
| `shuaa5-tm` (80) | ShuaA5 treadmills (FTMS) | `ZW-` without FTMS advertisement (FTMS-advertising devices use generic `ftms`) | FTMS treadmill data | FTMS speed/incline only on fresh data while running; never sends Start | `src/devices/shuaa5treadmill/shuaa5treadmill.cpp`; `vendor_treadmills.hpp/.cpp` | Host tests; hardware unverified | `treadmill` |

### Batch 2: not ported

- **Bluetooth Classic (RFCOMM/SPP):** `toorxtreadmill`, `iconsolebike`,
  `iconceptbike`, `iconceptelliptical`, `technogymmyruntreadmillrfcomm`.
- **Covered by standard services:** `bkoolbike` telemetry (`cps`/`csc`; its
  control service UUID is not in the pinned source), `zwifthubbike` (not present
  at the pinned revision; Zwift Hub uses `ftms`), FTMS-advertising variants
  such as `BIKZU_`, `DKN RUN…` (11+ characters), `ZW-`, `TRUE TREADMILL nnn`,
  `FS-YK-`, Domyos FTMS consoles.
- **Licence:** `kineticinroadbike` depends on Kinetic's proprietary
  SmartControl code ("All rights reserved").
- **Unsafe or unusable on this transport:** ESLinker Cadenza (upstream acts as the
  motor console and starts the belt), ESLinker Rhythm Fun / Ypoo Mini Change
  (init/keep-alive frames share the set-speed layout), DeerRun FFF0/Superun
  variants, Life Fitness proprietary/T5 init (undiscovered endpoints, 202-byte
  frame), Technogym MyRun ASCII console init, Keep bike's newer protocol (frames
  over 32 bytes), Horizon GR7 custom `fff3/fff4` path (HR-estimated power).
- **Accessories without fitness telemetry** are out of scope for the input
  registry: Wahoo Headwind, Elite Aria, FitMetria FanFit, Elite Sterzo/Rizer/Square,
  Zwift Click/Play, SRAM AXS, ThinkRider and CYCPLUS BC2 controllers, Stryd/Zwift
  RunPod, CORE, Moxy, Crossrope and SmartSpin2k.
- Upstream-only estimates (weight-based kcal/watts, distance integrated from
  speed) are not ported; see the Metrics column of each row.

## Important boundaries

- This is **not** the entire qdomyos-zwift catalogue: non-BLE transports,
  accessories and the classes listed under
  [batch 2: not ported](#batch-2-not-ported) are not implemented.
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
has no value; ramp angle is derived from inclination (and inclination from ramp
angle) and rower pace from speed when missing.

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
treadmill protocols (iFit/Bowflex/Horizon/Spirit/Sole/WalkingPad/Sportstech) → `treadmill`;
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
