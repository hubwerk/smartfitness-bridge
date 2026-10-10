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
| `ftms` (50) | FTMS bike, treadmill, cross trainer, rower | Advertised Fitness Machine Service `0x1826` | Indoor Bike `0x2AD2`, Treadmill `0x2ACD`, Cross Trainer `0x2ACE`, Rower `0x2AD1`: speed/cadence/power/HR/incline where present; resistance decoded internally | Feature-gated power, resistance, incline, speed; Request Control handshake; power/resistance/incline honor supplied ranges; grade via bike simulation if needed. Speed is available via console/API, not FTMS output | Local Bluetooth FTMS codec and `ftms_source.hpp` / `src/ftms_source.cpp` | Host byte-layout, feature/range/control tests; hardware unverified |
| `cps` (40) | Cycling Power sensor | Service `0x1818` | Power, cadence derived from crank revolution/event deltas | None | Local Bluetooth CPS codec; `sensor_sources.hpp`, `src/sensor_sources.cpp`, `src/sensor_codec.cpp` | Host power/crank/wraparound tests; hardware unverified |
| `csc` (30) | Cycling Speed and Cadence sensor | Service `0x1816` | Speed (fixed **2.105 m** wheel circumference), cadence from wheel/crank deltas | None | Local Bluetooth CSC codec; sensor sources/codec | Host wheel/crank/wraparound tests; hardware unverified |
| `hrs` (20) | Heart Rate monitor | Service `0x180D` | Heart rate, 8- or 16-bit measurement | None | Local Bluetooth HRS codec; sensor sources/codec | Host 8/16-bit parser tests; hardware unverified |

Vendor ports are GPL-derived from
[`cagnulein/qdomyos-zwift`](https://github.com/cagnulein/qdomyos-zwift),
Copyright Roberto Viola and contributors (GPL-3.0). Attribution is retained in
`include/protocols/` and `src/protocols/`; no upstream commit revision is recorded.
Discovery rules follow upstream `src/bluetooth.cpp`. See [license and
attribution](../README.md#license-and-attribution) and [HD-005](hardware-decisions.md#hd-005-gpl-30-licensing-for-qdomyos-zwift-ports).

## Batch 1: stopped at Peloton feasibility check

The requested order requires stopping and documenting the first infeasible
device. **Peloton Bike / Bike+ cannot be added as a native BLE input from the
reviewed upstream implementation.** Consequently, this batch adds no adapters;
items 2–8 are deferred, not declared technically infeasible or host-tested.
The supported registry table above is unchanged.

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
  which parses standard Cycling Power Measurement notifications. A compatible
  accessory advertising `0x1818` can use the existing read-only `cps` adapter;
  this does **not** add Bike+ compatibility or Peloton resistance control.
  No accessory has been hardware-verified with this bridge.

Do not pair a native Peloton by forcing a vendor adapter, or infer control
capabilities from the upstream virtual output. OCR/UDP, Wi-Fi, cloud, USB and
ANT+ paths remain outside this BLE-only batch.

### Skipped / deferred batch-1 inventory

Source paths below are relative to the pinned upstream revision. Deferred
entries identify research candidates only: no match rules, telemetry units,
connection sequences or control support have been certified for them.

| Order | Device / variant | Status and reason | Upstream source / next audit target |
|---|---|---|---|
| 1 | Peloton Bike / Bike+ | Unsupported as native BLE: upstream input is OCR over UDP; BLE workaround needs extra hardware. Ordered batch stops here. | `src/devices/pelotonbike/pelotonbike.cpp`; accessory discovery in `src/devices/bluetooth.cpp` and `src/devices/stagesbike/stagesbike.cpp` |
| 2 | iFit / NordicTrack / ProForm bikes, treadmills and ellipticals | BLE variants deferred because of the item-1 stop. `nordictrackifitadbbike` is excluded: its constructor binds a UDP socket on port 8002, not BLE. Wi-Fi/telnet/ADB paths are not eligible. | `src/devices/proformbike/proformbike.cpp`, `src/devices/proformtreadmill/proformtreadmill.cpp`, `src/devices/nordictrackelliptical/nordictrackelliptical.cpp`, `src/devices/nordictrackifitadbbike/nordictrackifitadbbike.cpp` |
| 3 | Bowflex / Schwinn C6/IC extensions, T216 treadmill, Schwinn 170 | Deferred because of the item-1 stop. Existing `schwinn-ic4` matching remains unchanged; no broader model support is claimed. | `src/devices/schwinnic4bike/schwinnic4bike.cpp`, `src/devices/bowflext216treadmill/bowflext216treadmill.cpp`, `src/devices/schwinn170bike/schwinn170bike.cpp` |
| 4 | Tacx Neo / Flux FE-C-over-BLE and non-FTMS Elite trainers | Deferred because of the item-1 stop. Proprietary BLE transport and per-model control still need auditing; ANT+ is excluded. | `src/devices/tacxneo2/tacxneo2.cpp`; Elite discovery in `src/devices/bluetooth.cpp` and `src/devices/stagesbike/stagesbike.cpp` |
| 5 | Legacy Wahoo KICKR without FTMS | Deferred because of the item-1 stop; proprietary control is not implemented. Standard FTMS variants continue to use `ftms`. | `src/devices/wahookickrsnapbike/wahookickrsnapbike.cpp` |
| 6 | Concept2 PM5 rower BLE | Deferred because of the item-1 stop; proprietary rowing/CSAFE notifications are not implemented. | `src/devices/csaferower/csaferower.cpp`, `src/devices/bluetooth.cpp` |
| 7 | Horizon / Spirit / Sole BLE treadmills | Deferred because of the item-1 stop; no new proprietary speed/incline control is claimed. | `src/devices/horizontreadmill/horizontreadmill.cpp`, `src/devices/spirittreadmill/spirittreadmill.cpp`, `src/devices/solef80treadmill/solef80treadmill.cpp` |
| 8 | Kingsmith / WalkingPad treadmills | Deferred because of the item-1 stop; model-specific framing/init/control still need auditing. | `src/devices/kingsmithr1protreadmill/kingsmithr1protreadmill.cpp`, `src/devices/kingsmithr2treadmill/kingsmithr2treadmill.cpp` |

### Resumption and proposed batch 2

Resume items 2–8 above only after agreeing to skip the Peloton blocker or to
target a specific external BLE accessory. Each new adapter still needs the
pure-C++ port, attribution, upstream-layout host vectors, malformed-frame tests,
registry conflict tests and truthful capabilities required by the guide below.
Successful ports must be labelled **“host-tested, not hardware-verified”**;
none of the deferred entries qualifies for that label yet.

After that backlog, a proposed batch 2 from the remaining upstream catalogue is
Domyos treadmills/rowers/ellipticals (`domyostreadmill`, `domyosrower`,
`domyoselliptical`), Echelon rower/stride/stair variants (`echelonrower`,
`echelonstride`, `echelonstairclimber`), Renpho bikes (`renphobike`) and FitShow
treadmills (`fitshowtreadmill`). These are audit candidates, not promised ports:
verify BLE transport and model-specific safety/control behavior first.

**Resource impact of this stop:** no compiled source, registry, dependencies,
web assets or configuration changed; additional firmware flash and static RAM
are **0 bytes**. This is a documentation-only result, not a measured size
estimate for the deferred protocols.

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
