# TraceView glasses link (v1)

How the **Tab5** and the **glasses** talk. The glasses firmware and the laptop simulator (`tools/glasses_sim.py`) both follow this spec.

## Connection

- The Tab5 runs a Wi-Fi hotspot named `TraceView-XXXX` (the last 4 characters come from its MAC address). Password: `traceview1`. Change it in `tab5/src/main.cpp`.
- The glasses join the hotspot and open a **TCP** connection to `192.168.4.1:7070`.
- Both directions send **JSON lines**: one JSON object per line, ending with `\n`.
- The Tab5 accepts up to 4 glasses or simulators at once.

The glasses render the overlay themselves, so only small messages cross the link.

## Tab5 → glasses

| `t` | When | Fields |
| --- | --- | --- |
| `hello` | On connect | `v` protocol version, `name` hotspot name |
| `veh` | On connect | `name` vehicle, `vin` (from the bus if broadcast) |
| `job` | On connect + every change | `active` 0/1, `complete` 0/1, `title`, `i` step index (0-based), `n` step count, `step` title, `sub` [3 lines], `comp` part id to highlight (`BAT`, `G1`, `STR`, `ALT`…), `torque` |
| `ui` | On connect + every change | `panels` 0/1 (hide panels), `side` `left`/`right` (panel lens) |
| `live` | Every 1 s | `rpm`, `bat`, `cool` (°F), `oil` (psi), `mph`, `vin`. `null` = no recent data |
| `dm1` | Every 1 s | `lamps` {`mil`,`rsl`,`awl`,`pl`}, `codes` [{`sa`,`spn`,`fmi`,`oc`}] |

```json
{"t":"hello","v":1,"name":"TraceView-3A7F"}
{"t":"job","active":1,"title":"Slow crank / no crank","i":1,"n":6,"step":"Inspect main ground (GNDP)","sub":["Disconnect batteries first (SB 54-318)","Clean stud and ring eyes to bare metal","Torque ring eye to 26 ft-lb"],"comp":"G1","torque":"26 ft-lb"}
{"t":"ui","panels":1,"side":"right"}
```

## Glasses → Tab5

| Message | Meaning |
| --- | --- |
| `{"t":"btn","a":"next"}` | Button single press: next step |
| `{"t":"btn","a":"prev"}` | Button double press: previous step |
| `{"t":"btn","a":"panels"}` | Button hold: hide or show panels |
| `{"t":"voice","text":"next step"}` | Recognized voice command. Also `previous step`, `hide panels`, `show panels`, `start job` |

Plain words (`next`, `prev`, `hide panels`) also work, which is handy for testing with a terminal.

The Tab5 is the single source of truth: it changes the step, then sends the new `job` line to every connected pair of glasses.
