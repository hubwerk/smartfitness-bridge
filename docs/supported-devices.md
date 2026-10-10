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
