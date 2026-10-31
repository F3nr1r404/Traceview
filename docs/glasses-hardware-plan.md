# TraceView glasses: hardware plan (two-device product)

**Product:** TraceView glasses + M5Stack Tab5. No phone, no third device.

| Device | Job |
| --- | --- |
| **Tab5** | Truck data (J1939 via CAN Unit), schematic library, VIN, job selection, fault codes, live data, Wi-Fi hotspot |
| **Glasses** | Draw the overlay locally (steady when your head moves), track the head, lock onto the truck, voice + button |

The glasses render on their own processor. The Tab5 sends only small updates over Wi-Fi (see `firmware/shared/glasses_link.md`). Streaming video from the Tab5 would add enough delay to make the overlay swim.

---

## Step 2: bench prototype of the glasses brain (no optics yet)

Goal: a board on the bench that joins the Tab5 hotspot, shows the current step, and draws a test overlay that stays put when you rotate the board.

| Part | Why | Notes |
| --- | --- | --- |
| **M5Stack Stamp-P4** (SKU S013), about $13 | Glasses processor. ESP32-P4, 16 MB flash, 32 MB PSRAM, **MIPI-DSI display** and **MIPI-CSI camera** interfaces, 2.7 g | Same chip family as the Tab5, so code is shared |
| **Stamp-AddOn C6 for P4**, about $7 | Wi-Fi 6 + Bluetooth for the Stamp-P4 | Snaps onto its SDIO connector |
| **6-axis IMU breakout** (BMI270 class, I2C) | Head rotation tracking, fast and smooth | M5Stack sells IMU units. Pick one with a gyroscope; I2C to the Stamp-P4 |
| **Small SPI test LCD** (ST7789, 1.3"–2") | Bench display before the optics exist | Avoids MIPI bring-up on day one |
| **Momentary push button** | Glasses button: 1 press next, 2 presses back, hold panels | Any tactile switch |
| **I2S MEMS microphone** (or M5Stack PDM mic unit) | Voice commands on the glasses | Optional for step 2 |
| **USB-C power** for now | | Battery comes with the frame build |

Step 2 software (I'll write it next): join the `TraceView-XXXX` hotspot, follow the link spec, draw the step card and a wireframe test overlay, and use the IMU so the overlay stays fixed in space as you turn the board.

---

## Step 3: optics and the frame

This is the part M5Stack doesn't sell, and the biggest risk in the product. You need a **see-through display module**: a micro-OLED plus optics (birdbath or freeform prism) that puts the image out in front of you, focused near arm's length to a couple of metres.

Rough targets when shopping for a module:

- **Input: MIPI-DSI, 2-lane or less.** The Stamp-P4 has a 2-lane DSI port, so think about 720p class, not full HD.
- **See-through:** a clear view of the real truck through the combiner.
- **Brightness:** bright enough for a lit shop bay. More nits is better.
- **Field of view:** about 30–45°.
- **Monocular or binocular:** monocular (one eye) is cheaper and fits the "panels on one lens" design. Binocular gives depth.
- **The panel's datasheet and init sequence must be available.** Each panel needs its own startup code.

Frame build:
- **Z87.1+ impact rating with everything mounted.** Design for it from the start. The display and optics must sit behind a rated impact lens.
- **Battery on the back strap** (1-cell LiPo + charger + 5 V boost for the Stamp-P4), not near the eyes.
- **Weight on the face under ~120 g**, per the design doc.

---

## Step 4: locking onto the truck

- **Camera:** a MIPI-CSI camera module on the Stamp-P4's CSI connector.
- **Printed markers** on fixed points (battery box corner, frame rail end) give a solid lock for the work zone. This is the fallback the design doc already describes, and what's realistic on an ESP32-P4.
- **IMU** fills in between camera frames, so the overlay stays smooth.
- Markerless tracking like a phone's ARCore needs a more powerful processor. That's a later hardware revision.

---

## What carries over from the web prototype

- The vehicle package format (`traceview.vehicle-package/2`): parts, harness routes, 3D positions in mm.
- The job steps, voice commands, glasses button behavior, hide panels, panel lens.
- The J1939 decoder (`firmware/shared/j1939.h`).
