# TraceView

AR safety glasses for diesel and heavy-truck techs. The glasses lay the vehicle's wiring and parts over the real truck, and voice filters what you see ("show me the starter system").

This repo holds the working web prototype, the product design doc, and the concept mockups.

## Live demo (GitHub Pages)

| Page | What it is |
| --- | --- |
| [`/` (index.html)](index.html) | **Live camera + WebXR AR build.** Your phone's camera acts as the glasses camera. Same overlays, panels and voice-style commands as the 3D demo. |
| [`/demo-3d/`](demo-3d/) | The earlier build on a 3D Cascadia model you can orbit, with the developer tech mode for part placement. |

### Using the live build

1. Open the Pages link on your phone (HTTPS is required for the camera).
2. **AR tracking (Android Chrome with ARCore):** tap **Start AR**. Point at the floor beside the truck.
   - Tap the ground under the **front tire's outer edge**.
   - Then tap under the **first drive tire**.
   - The overlay locks to the truck using the measured wheelbase and stays anchored as you walk. **Re-align** redoes the two taps.
3. **Camera mode (iPhone / any browser):** tap **Live camera**. Line the steer hub and first drive hub up with the two tap targets. The overlay is fixed to the screen, so hold the phone steady.
4. Use the chips or the command bar: `show me the starter system`, `isolate the starter relay`, `exploded view of the starter`, `start job`, `next step`, `show me where`, `clear`.

### Glasses HUD (v1.0)

- **Launch HUD** (toolbar or phone Settings → Glasses HUD) goes full screen with **no panels** except the vehicle card. Black is see-through on the glasses. The camera feed shows only until the overlay is locked; after that the screen is black except for the overlay.
- **Jobs are picked on the phone** (Job tab → Start job on glasses). The glasses never show a job start panel. They show only the current step.
- **Glasses button:** 1 press = next step, 2 presses = previous step, hold = hide/show panels. Use the round button in the demo, any Bluetooth clicker or keyboard (Space, Enter, arrow keys, Page Down), or tap the phone screen once the HUD is locked.
- **Voice:** "next step", "previous step" (or "back"), "hide panels", "show panels". Turn on **Hands-free** to keep the mic listening (Chrome, needs internet for speech recognition).
- **Vehicle card:** the loaded vehicle (year, model, VIN) always shows on the glasses, full size when idle and a one-line badge during a job.
- **Hide panels:** toolbar button, phone Settings, voice, or hold the glasses button. Hides the step and info panels; the vehicle badge stays.
- **Truck link:** Scan or Job tab → **Connect truck bridge** reads live data and active fault codes from the CoreS3 bridge (see Truck bridge below). A matching fault code offers **Start job**, and the glasses vehicle card shows the fault count.
- **HUD window:** phone Settings → Open HUD window (or add `?hud=1` to the URL) opens a HUD-only window that stays in sync with the phone app in the same browser. Use it when the glasses are a second screen (laptop, Samsung DeX). Pick **Overlay only (no camera)** if that device has no rear camera.

Nothing is recorded or captured. The camera feed stays on the device.

**Limits:** Safari on iPhone has no WebXR AR yet, so it uses camera mode. AR mode is one full-screen view, not the two-lens layout. Part positions are **estimates** for a 2026 Cascadia 126 day cab. Use the developer tech mode in `/demo-3d/` to save real measurements.

## Truck bridge (CoreS3 + CAN)

`firmware/traceview-bridge/` is a PlatformIO project for an **M5Stack CoreS3 + CANBus Unit (U085)**. It reads the truck's J1939 bus in **listen-only** mode (fault codes, lamps, battery volts, RPM, coolant, oil, speed) and sends it to the phone app over Bluetooth. In the phone app, **Scan** or **Job** → **Connect truck bridge** (Chrome on Android or a computer). Wiring and build steps are in [its README](firmware/traceview-bridge/README.md).

## Repo layout

```
index.html                 live camera + WebXR AR build (single file)
firmware/traceview-bridge/ CoreS3 J1939 truck bridge (PlatformIO)
demo-3d/                   3D-model build (three.js r160 + CC BY Cascadia model)
docs/
  TraceView-Design-Concept.md   product design doc
  TraceView-Design-Concept.pdf  same doc as PDF
images/
  mockup-product-sheet.png      "Mechanic Pro" product concept sheet
  mockup-workflow-hud.png       app + glasses HUD workflow concept
```

## Concept mockups

![Product sheet](images/mockup-product-sheet.png)
![Workflow HUD](images/mockup-workflow-hud.png)

## Data and credits

- Wiring and torque data are tagged by source in the app: public DTNA service bulletins (static.nhtsa.gov), Delco Remy instruction sheets, general shop practice, and demo/unverified entries. **These are not OEM schematics. Always confirm against the OEM procedure and a meter.**
- 3D model: "FREIGHTLINER CASCADIA 2020" (https://sketchfab.com/3d-models/freightliner-cascadia-2020-2dd8eb87823c4eacabfb9d145eff5450) by liebherr996litronic (https://sketchfab.com/liebherr996litronic), licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/).
- three.js (MIT), bundled in `demo-3d/lib/`.
