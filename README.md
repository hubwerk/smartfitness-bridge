# ESP32 Smart Fitness Bridge

[![CI](https://github.com/manuelh83/smartfitness-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/manuelh83/smartfitness-bridge/actions/workflows/ci.yml)

The bridge reads supported fitness equipment over Bluetooth Low Energy (BLE) and
forwards its measurements to a training app or display using Bluetooth FTMS or
ANT+ FE-C. It runs on an ESP32-S3 DevKitC-1 N16R8 (web interface) or an ELECROW
CrowPanel 1.28" (local display).

> **Testing status:** Wahoo KICKR FTMS input → BLE FTMS output → MyWhoosh has
> been verified on both board variants. Wahoo KICKR input → ANT+ FE-C output →
> Garmin fenix 6 Pro has been verified on the web variant with a USB ANT+ stick.
> Other listed equipment and output combinations have not been verified on
> hardware; “implemented” means the bridge has a host-tested protocol decoder.

## Build and flash

Install Git and PlatformIO Core (`python -m pip install -U platformio`), or use
the PlatformIO IDE extension in VS Code. Connect the board over USB, then run
these commands from the project directory:

```sh
git clone https://github.com/manuelh83/smartfitness-bridge.git
cd smartfitness-bridge
pio run -e esp32s3dev -t upload     # ESP32-S3 DevKitC-1, web interface
pio run -e crowpanel128 -t upload   # ELECROW CrowPanel 1.28", local display
```

PlatformIO downloads the toolchain and libraries on the first build and selects
the available serial port automatically. To choose a port explicitly, add
`--upload-port <port>` (for example, `COM3` or `/dev/ttyACM0`). The project uses
the board's 16 MB flash layout; do not select a different partition scheme.

## Quick start

### Web variant

1. After flashing, connect to the Wi-Fi access point named `SmartFitness-XXXX`.
   Its initial password is `smartfitness`; change it before normal use.
2. Open `https://192.168.4.1/` and accept the browser warning for the
   self-signed certificate. In **Settings**, enter your home Wi-Fi details and
   save/restart, or continue using the access point.
3. Wake the fitness device, disconnect other apps from it, then use
   **Devices → Scan** and **Pair**. Check the dashboard for a connection and
   live measurements.
4. Choose Bluetooth FTMS or ANT+ FE-C output in **Settings**. For ANT+ on the
   web variant, connect a supported USB ANT+ stick through the USB host port.
   The CrowPanel requires an externally wired UART ANT module and a custom build
   with its UART pins configured; that hardware path is not yet verified.
5. Connect your training app or display to the bridge and start a workout from
   that app. The bridge does not start treadmill belts.

### CrowPanel HMI variant

1. Wake the fitness device and disconnect other apps from it.
2. Use the rotary control or touchscreen to open **Devices**, scan, select the
   device, and pair. Confirm that the dashboard shows live measurements.
3. Open **Settings** and select the output protocol. ANT+ on the CrowPanel
   requires an externally wired UART ANT module and a custom build with its
   UART pins configured; that hardware path is not yet verified. The CrowPanel
   USB ANT+ path is not recommended.
4. Connect your training app or display to the bridge and start the workout
   from that app. The HMI variant does not use Wi-Fi.

The web interface has no authentication; anyone on its Wi-Fi network can
control the bridge. Change the default access-point password, and do not expose
the bridge to an untrusted or public network.

The bridge reconnects to its saved input after reboot. Use **Disconnect** or
**Unpair** from Devices when needed. For safety, treadmill controls are only
available when the device is already running and reporting fresh data.

## Hardware variants and accessories

The bridge works with either board using Bluetooth FTMS; no extra peripheral is
needed for that output. ANT+ FE-C requires an external ANT radio. Affiliate links
are provided for convenience and are not an endorsement.

| | Web | HMI |
|---|---|---|
| **Board (PlatformIO environment)** | [ESP32-S3 DevKitC-1 N16R8](https://link.amazon/B04sCBCRH) (`esp32s3dev`) | [ELECROW CrowPanel 1.28" ESP32-S3 Rotary Display](https://link.amazon/B0eHDSltG) (`crowpanel128`) |
| **Base peripherals** | USB data cable for flashing; no extra hardware for BLE FTMS | USB data cable for flashing; display and controls are integrated; no extra hardware for BLE FTMS |
| **Optional ANT+ hardware and affiliate links** | [Coospo ANT+ USB stick](https://link.amazon/B0dalQtFe) and [USB-C to USB-A OTG adapter (LogiLink AU0055)](https://link.amazon/B01z26VJ2). USB-stick ANT+ output is hardware-verified on this variant. | ANT+ via UART requires the same [Coospo ANT+ module](https://link.amazon/B0dalQtFe) as the web variant, plus this [UART adapter](https://link.amazon/B0ikuHkY1), wiring, and a custom build with UART pins configured. This setup is not hardware-verified. The USB-stick path on the CrowPanel is **not recommended or verified**. |
| **Example** | <img src="docs/hmi/web/01-dashboard-desktop.png" alt="Web variant dashboard" width="480"> | <img src="docs/hmi/hmi/01-dashboard-bike.png" alt="CrowPanel HMI dashboard" width="240"> |

Use the USB-C connection to flash the CrowPanel. Do not assume its USB port can
host an ANT+ stick; see the [hardware notes](docs/developer-guide.md#crowpanel-usb-host-is-not-recommended-schematic-verification-required)
before planning any ANT+ wiring or accessories.

## Supported equipment by fitness profile

These are BLE input protocols. Measurements are forwarded using the selected
output protocol; see the [detailed device guide](docs/supported-devices.md) for
exact match rules, per-protocol fields, model profiles, and limitations.
“FTMS tested” and “ANT+ tested” below mean an end-to-end hardware test with
that input profile, not just a successful host test.

### Indoor bike / bike trainer

| Manufacturer / model | BLE protocol | Transmitted values / services | Controllable values | Implementation | FTMS output tested | ANT+ output tested |
|---|---|---|---|---|---|---|
| Standard FTMS bikes | FTMS (`0x1826`, Indoor Bike Data `0x2AD2`) | Fields sent by the device: speed, cadence, power, heart rate, resistance, energy, distance, time, and related bike fields | Feature/range-gated power, resistance, and simulation incline | Implemented; host-tested | Wahoo KICKR → MyWhoosh, verified | Wahoo KICKR → Garmin fenix 6 Pro, verified |
| Keiser M3i | BLE advertisement / manufacturer data | Cadence, power, heart rate, resistance | None | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Echelon Connect Sport / EX | BLE vendor GATT | Cadence, resistance, estimated power and speed | Resistance | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Domyos / Decathlon bikes | BLE vendor GATT | Speed, cadence, heart rate, resistance, estimated power | Resistance | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Schwinn IC4/IC8 and matching Bowflex bikes | BLE FTMS data | Bike data as reported, including speed, cadence, power, and heart rate | None (read-only) | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Yesoul bikes | BLE advertisement / vendor data | Cadence, power, resistance; speed derived from cadence | None | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| iFit / NordicTrack / ProForm bikes | BLE FTMS or vendor GATT | Power, cadence, resistance; profile-dependent speed and incline | Resistance; incline on compatible profiles | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Tacx Neo / Flux and Elite (legacy, DR, Misuro B+) trainers | BLE vendor GATT / Cycling Power and Speed-Cadence services | Power, cadence, speed, optional heart rate | Power, resistance or simulated grade (model-dependent) | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Wahoo KICKR legacy trainers | BLE Cycling Power / Speed-Cadence and vendor GATT | Power, cadence, speed, optional heart rate | Power, resistance, simulated grade | Implemented; host-tested | Not separately verified | Not separately verified |
| Nautilus / Schwinn 170/270 / Bowflex bikes | BLE vendor GATT | Model-dependent speed, cadence, power, resistance, and heart rate | Model-dependent resistance or read-only | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Sportstech / SportsPlus / Carefitness / Pafers bikes (incl. Sportstech SX600) | BLE vendor GATT | Speed, cadence, power, resistance, and heart rate as reported | Model-dependent resistance | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| FitPlus / S-Node / Merach / Virtufit / H9110 Osaka bikes | BLE FTMS or vendor GATT | Model-dependent speed, cadence, power, resistance, and heart rate | Model-dependent; S-Node is read-only | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Toorx / TRX App Gate bikes, including i-Running / iConsole+, Reebok, Adidas, DKN, Tunturi, Casall, FAL-SPORTS, FitHiWay, Hertz, Enerfit, Hammer, JLL, Fytter, Asviva, Hop-Sport, Taurua, Changyow | BLE vendor GATT (TRX App Gate) | Model-dependent speed, cadence, power, resistance, and heart rate | Model-dependent resistance | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Flywheel, Life Fitness (IC5/IC8), Sole, Kettler, Technogym, Inspire, Keep, MCF, XCX, Chrono, Volava, Ultrasport, Skandika, PitPat, Freebeat, MEPANEL, Apex, NPE CABLE, CycleOps, Joroto/BGYM, Renpho, Horizon GR7, LifeSpan bikes | BLE vendor GATT or FTMS; see device guide | Model-dependent speed, cadence, power, resistance, and heart rate | Model-dependent resistance or read-only | Implemented; host-tested | Not hardware-verified | Not hardware-verified |

### Treadmill

| Manufacturer / model | BLE protocol | Transmitted values / services | Controllable values | Implementation | FTMS output tested | ANT+ output tested |
|---|---|---|---|---|---|---|
| Standard FTMS treadmills | FTMS (`0x1826`, Treadmill Data `0x2ACD`) | Fields sent by the device, including speed, incline, pace, distance, heart rate, and time | Feature/range-gated speed and incline | Implemented; host-tested | Not separately verified | Not separately verified |
| iFit / NordicTrack / ProForm treadmills | BLE FTMS or vendor GATT | Speed and profile-dependent incline | Bounded speed and incline where supported | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Bowflex T216 and treadmill profiles | BLE vendor GATT | Speed and incline | Read-only on T216 profiles | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Domyos / Decathlon treadmills | BLE vendor GATT | Speed, incline, optional heart rate | Speed and incline only while already running | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Echelon Stride / Stride 4 | BLE vendor GATT | Speed, incline, optional heart rate | Incline; no remote speed/start/stop | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| FitShow / FitPlus / NoblePro treadmills | BLE vendor GATT | Speed, incline, and model-dependent time, steps, and heart rate | Speed and incline only while already running | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Horizon (incl. Paragon X), Spirit, Sole, and Kingsmith / WalkingPad treadmills | BLE vendor GATT | Speed and model-dependent incline | Model-dependent speed/incline; no belt start on connect | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Sportstech / FitHome treadmills (F15, F31, WP100, WP200) | BLE vendor GATT (`0xFFF0`); manual pairing only | Speed and incline as reported | Model-dependent speed/incline; safety-gated | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Toorx / TRX App Gate treadmills, including V-RUN, K80, i-Running, Reebok, Adidas, DKN, JTX, Flow Fitness | BLE vendor GATT (TRX App Gate) | Speed, incline, and model-dependent distance, time, and heart rate | Model-dependent speed/incline | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Nautilus / Schwinn / Bowflex treadmills (T628, 688a service) | BLE vendor GATT | Speed and incline | Model-dependent | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Pafers / BH i.Boxster treadmills | BLE vendor GATT; manual pairing | Speed and incline as reported | Model-dependent | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| TRUE, Assault, WDWAY, Technogym MyRun, LifeSpan, Octane (Zero Runner ZR7/ZR8), ZiPro, ESLinker, Sperax, Focus, DeerRun/PitPat, Activio, Wahoo KICKR RUN treadmills | BLE vendor GATT or FTMS; see device guide | Model-dependent speed, incline, and optional heart rate | Model-dependent; safety-gated | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Life Fitness / ShuaA5 treadmills | BLE FTMS data | Speed, incline, and fields reported by the device | Life Fitness read-only; ShuaA5 model-dependent | Implemented; host-tested | Not hardware-verified | Not hardware-verified |

### Cross trainer / elliptical

| Manufacturer / model | BLE protocol | Transmitted values / services | Controllable values | Implementation | FTMS output tested | ANT+ output tested |
|---|---|---|---|---|---|---|
| Standard FTMS cross trainers | FTMS (`0x1826`, Cross Trainer Data `0x2ACE`) | Fields sent by the device, including speed, cadence, resistance, incline, power, energy, and heart rate | Feature/range-gated speed, incline, resistance, and power | Implemented; host-tested | Not separately verified | Not separately verified |
| iFit / NordicTrack / ProForm ellipticals (incl. FreeStride) | BLE vendor GATT | Cadence, speed, resistance; profile-dependent incline, power, and heart rate | Resistance; incline on compatible profiles | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Domyos / Echelon / Nautilus ellipticals | BLE vendor GATT | Model-dependent speed, cadence, resistance, and heart rate | Resistance or read-only, depending on model | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Sportstech / Toorx / TRX App Gate (iConsole+, FAL-SPORTS, JTX) ellipticals | BLE vendor GATT; see device guide | Model-dependent speed, cadence, power, resistance, and heart rate | Model-dependent resistance/incline | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| BH, YPOO / Merach, Sole, Xterra XE88, Octane Q37, TRUE, FEIER, Schwinn 411/510E, Gymstick ellipticals | BLE vendor GATT or FTMS; see device guide | Model-dependent speed, cadence, power, resistance, and heart rate | Model-dependent resistance or read-only | Implemented; host-tested | Not hardware-verified | Not hardware-verified |

### Step climber

| Manufacturer / model | BLE protocol | Transmitted values / services | Controllable values | Implementation | FTMS output tested | ANT+ output tested |
|---|---|---|---|---|---|---|
| Standard FTMS step climbers | FTMS (`0x1826`, Step Climber Data `0x2ACF`) | Fields sent by the device, including step rate, step count, elevation, floors, energy, and heart rate | Feature/range-gated resistance and power | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Sunny Fit stepper | BLE vendor GATT | Stepper measurements supported by its protocol | Model-dependent | Implemented; host-tested | Not hardware-verified | Not hardware-verified |

### Stair climber

| Manufacturer / model | BLE protocol | Transmitted values / services | Controllable values | Implementation | FTMS output tested | ANT+ output tested |
|---|---|---|---|---|---|---|
| Standard FTMS stair climbers | FTMS (`0x1826`, Stair Climber Data `0x2AD0`) | Fields sent by the device, including step rate, stride count, elevation, floors, energy, and heart rate | Feature/range-gated resistance and power | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Echelon stair climber | BLE vendor GATT | Step count, elevation gain, cadence, and derived speed | None (read-only) | Implemented; host-tested | Not hardware-verified | Not hardware-verified |

### Rower

| Manufacturer / model | BLE protocol | Transmitted values / services | Controllable values | Implementation | FTMS output tested | ANT+ output tested |
|---|---|---|---|---|---|---|
| Standard FTMS rowers | FTMS (`0x1826`, Rower Data `0x2AD1`) | Fields sent by the device, including stroke rate/count, speed, pace, distance, resistance, power, energy, and heart rate | Feature/range-gated resistance and power | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Concept2 PM5 rower / SkiErg | BLE vendor GATT | Speed, stroke rate, power, heart rate, elapsed time, distance, pace, and stroke count as available | None; mechanical damper is not controlled by the bridge | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| iFit / ProForm / NordicTrack rowers | BLE vendor GATT | Stroke rate, speed/pace, power, resistance, and distance as reported | Resistance on compatible profiles | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Echelon / Domyos / FitPlus (Merach R28) / Sportstech rowers | BLE vendor GATT | Model-dependent stroke rate, speed/pace, power, resistance, and heart rate | Model-dependent resistance | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| SportsPlus / Carefitness / VMAX / Pafers rowers | BLE vendor GATT | Stroke rate, speed/pace, power, resistance, and heart rate as reported | Model-dependent resistance | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Toorx / TRX App Gate rowers (iConsole+, Merach NovaRow R50) | BLE vendor GATT | Model-dependent stroke rate, speed/pace, power, and resistance | Model-dependent resistance | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| SmartRow | BLE vendor GATT | Stroke rate, power, distance, and pace as reported | None | Implemented; host-tested | Not hardware-verified | Not hardware-verified |

### Fitness sensors

| Manufacturer / model | BLE protocol | Transmitted values / services | Controllable values | Implementation | FTMS output tested | ANT+ output tested |
|---|---|---|---|---|---|---|
| Cycling Power sensors | Cycling Power Service (`0x1818`) | Power and cadence when crank data is present | None | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Cycling Speed and Cadence sensors | CSC Service (`0x1816`) | Speed and cadence when wheel/crank data is present | None | Implemented; host-tested | Not hardware-verified | Not hardware-verified |
| Heart rate monitors | Heart Rate Service (`0x180D`) | Heart rate | None | Implemented; host-tested | Not hardware-verified | Not hardware-verified |

Entries marked manual in the device guide (for example Sportstech / FitHome
treadmills) are selected in the adapter list while pairing.

### Not supported

Peloton/DFC, Bluetooth Classic (RFCOMM), ANT+/USB/Wi-Fi/ADB inputs, accessories
without fitness data, and Kinetic (licensed protocol) are not supported. See
[the supported-device guide](docs/supported-devices.md) for details.

FTMS characteristics include only fields supported by the selected profile and
present in the incoming data. ANT+ FE-C output requires an external ANT radio;
the USB-stick path is hardware-verified only on the web variant. The CrowPanel
UART ANT path and ANT+ sensor channels are not hardware-verified. See the
[developer guide](docs/developer-guide.md) for architecture, APIs, detailed
protocol data, development instructions, and hardware decisions.

## License

The firmware is GPL-3.0-or-later; see [`LICENSE`](LICENSE) and the
[developer guide](docs/developer-guide.md#license-and-attribution) for
attribution and further details.
