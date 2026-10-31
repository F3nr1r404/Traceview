# TraceView Tab5

The handheld half of the two-device product: **Tab5 + TraceView glasses**. It replaces both the CoreS3 bridge and the phone app.

| Tab | What it does |
| --- | --- |
| **Dashboard** | Active fault codes (SPN / FMI / module / count), MIL, amber and red stop lamps, live battery V, RPM, coolant, oil, speed. When SPN 168 is active, a **Start job** button appears |
| **Job** | Guided job "Slow crank / no crank" (same 6 steps as the TraceView web app). Start, Next, Previous. Every change goes to the glasses right away |
| **Glasses** | Hotspot name, password, address, connected glasses, hide/show panels, panel lens left/right |
| **Bus** | Listen-only status, bus speed, frame counts, **Demo mode**, **Rescan bus** |

## Hardware

- **M5Stack Tab5** (ESP32-P4 + ESP32-C6). Get the kit with the NP-F550 battery, or power it over USB-C.
- **M5Stack CANBus Unit (U085)** → Tab5 **Grove port** (Port A: G53 yellow, G54 white) with the included Grove cable.
- **9-pin Deutsch pigtail** (female socket plug, e.g. MEIRIYFA Type 2): socket **C → H**, socket **D → L** on the CAN Unit. Find the wires with a multimeter; the colors aren't standard. Don't connect truck +12 V or ground to the Tab5.

The CAN code is the same as the CoreS3 bridge: listen-only, auto 250/500 kbps, auto wire order. The decoder is shared (`../shared/j1939.h`) and unit-tested.

## Build and upload

1. VS Code + **PlatformIO IDE** extension.
2. **File → Open Folder** → this `tab5` folder. The `../shared` folder must stay next to it (it holds the decoder).
3. Plug the Tab5 into USB-C and click **→ Upload**. The first build downloads the ESP32-P4 Arduino platform (pioarduino) and M5Unified, which takes a while.
   - If upload can't find the board: hold the Tab5's BOOT button while pressing reset, then upload again.

## Test it without a truck or glasses

1. **Bus → Demo mode.** The Dashboard shows SPN 168 FMI 1 with the amber lamp, plus live values.
2. **Dashboard → Start job**, then use Next / Previous on the **Job** tab.
3. Connect a laptop to the Tab5's Wi-Fi (`TraceView-XXXX`, password `traceview1`) and run the glasses simulator:
   ```
   python3 ../tools/glasses_sim.py
   ```
   The laptop prints what the glasses would show. Type `n` / `p` / `h` + Enter to act as the glasses button and watch the Tab5 follow along.

The link spec is in [`../shared/glasses_link.md`](../shared/glasses_link.md).

## Status and known risks (v0.1)

Syntax-checked, **not yet built or run on a Tab5**. Things most likely to need a fix on the first build:

- **Wi-Fi:** the Tab5's radio is a separate ESP32-C6 chip on an SDIO link. The code powers it on through the IO expander and sets the SDIO pins with `WiFi.setPins(...)` (CLK 12, CMD 13, D0-D3 11/10/9/8, RST 15, from M5Stack's pin map). If the Glasses tab says Wi-Fi failed, that call or the C6 firmware version is the first place to look.
- **CAN on the P4:** uses the standard ESP-IDF TWAI driver on GPIO 53/54. Should work as-is.
- **Screen rotation:** set to landscape (`setRotation(3)`). If it comes up upside down, change it to `1`.

Send the build errors or a photo of the screen, and I'll fix them.
