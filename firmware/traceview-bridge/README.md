# TraceView Truck Bridge (M5Stack CoreS3)

Reads the truck's J1939 data bus and sends live data and active fault codes to the TraceView phone app over Bluetooth.

**Listen-only.** The CAN controller runs in listen-only mode. The bridge never transmits on the truck's bus, not even acknowledgements.

## Parts

| Part | SKU | Notes |
| --- | --- | --- |
| M5Stack CoreS3 | K128 | The bridge: touchscreen, Bluetooth, battery |
| M5Stack CANBus Unit (CA-IS3050G) | U085 | Isolated CAN transceiver. Comes with a Grove cable |
| 9-pin Deutsch J1939 pigtail | — | **Female (socket) plug** with locking sleeve, to bare wires, like a scan-tool cable. The truck's port has the pins. Green Type 2 fits both green and black ports. Example: MEIRIYFA Type 2 J1939 pigtail, 30 cm |

## Wiring

```
Truck 9-pin Deutsch            CANBus Unit (U085)        CoreS3
  pin C  CAN-H  ───────────►  H  (screw terminal)
  pin D  CAN-L  ───────────►  L  (screw terminal)
  pin A  ground      (not connected)
  pin B  +12 V       (not connected)
                               Grove port ─── Grove cable ───►  PORT C (blue, G17/G18)
```

- Use **Port C** (blue). Port A is the CoreS3's I2C port.
- Don't wire truck +12 V or ground to the CoreS3. It runs on its own battery or USB-C.
- Pigtail wire colors are not standard. Find CAN-H and CAN-L with a multimeter on continuity: probe **socket C** (letters are molded on the face) and find the wire that beeps, then do the same for **socket D**. Tape off the unused wires.
- Confirm pins C/D against the truck's wiring diagram before plugging in.
- If the Grove wires are swapped inside the unit, the firmware finds that by itself.

## Build and upload (about 10 minutes the first time)

1. Install **VS Code** and the **PlatformIO IDE** extension.
2. In VS Code, **File → Open Folder**, then pick this `traceview-bridge` folder.
3. Plug the CoreS3 into the computer with USB-C.
4. Click the **→ Upload** arrow in the blue status bar at the bottom. PlatformIO downloads the ESP32 tools and the M5Unified library, builds, and flashes.
   - If the upload can't find the board, hold the CoreS3's reset button for about 3 s until the green light comes on (download mode), then upload again.

## Using it

1. Key on. Plug the pigtail into the 9-pin port.
2. The header shows **SCANNING**, then the bus speed and frames per second (e.g. `250k 312/s`). It tries 250 and 500 kbps on its own.
3. Tabs:
   - **FAULTS:** active codes (SPN, FMI, which module, count) and the MIL / amber / red stop / protect lamps.
   - **LIVE:** battery volts, RPM, coolant °F, oil psi, road speed, and the VIN if the truck broadcasts it.
   - **BUS:** bus speed, frame counts, **Demo mode**, **Rescan bus**.
4. On the phone, open TraceView in **Chrome (Android or computer)**. In the Scan or Job tab, tap **Connect truck bridge** and pick **TraceView-Bridge**. iPhone Safari can't use Bluetooth from a web page.
5. When an active code matches a TraceView job (the demo job covers SPN 168, battery potential), the phone offers **Start job**.

**No truck handy?** BUS tab → **Demo mode**, or tap **Demo data** on the phone. The bridge makes fake engine traffic with SPN 168 FMI 1 (amber lamp), which runs through the same decoder.

## Bluetooth protocol

Service `7a1c0001-5e2b-4b8e-9c1a-74726163ee01`

| Characteristic | UUID | Direction |
| --- | --- | --- |
| TX | `7a1c0002-…ee01` | Notify. JSON lines, sent in 20-byte chunks, each line ends with `\n` |
| RX | `7a1c0003-…ee01` | Write: `demo on`, `demo off`, `rescan` |

```json
{"t":"st","bus":"ok","baud":250000,"fps":312,"demo":0}
{"t":"live","rpm":650,"bat":13.90,"cool":185,"oil":49,"mph":0,"vin":""}
{"t":"dm1","lamps":{"mil":0,"rsl":0,"awl":1,"pl":0},"codes":[{"sa":0,"spn":168,"fmi":1,"oc":3}]}
```

## What it decodes

| PGN | Name | Used for |
| --- | --- | --- |
| 65226 | DM1 | Active fault codes + lamps (single frame and multi-packet BAM) |
| 61444 | EEC1 | Engine speed |
| 65271 | VEP1 | Battery potential (SPN 168) |
| 65262 | ET1 | Coolant temperature |
| 65263 | EFL/P1 | Oil pressure |
| 65265 | CCVS1 | Road speed |
| 65260 | VI | VIN (only if the truck broadcasts it; the bridge never requests it) |

The decoder (`../shared/j1939.h`, shared with the Tab5 firmware) has no hardware dependencies and is unit-tested on a PC (`../shared/test`). Keep the `shared` folder next to this one when you build.

## Status

v0.1 prototype. The decoder passes unit tests. The full firmware has been syntax-checked but **not yet built or run on a CoreS3**. Expect the first build to need small fixes.
