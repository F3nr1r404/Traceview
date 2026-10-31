#!/usr/bin/env python3
"""TraceView glasses simulator: test the Tab5 hotspot from a laptop before the glasses exist.

1. Join the Tab5's Wi-Fi (TraceView-XXXX, password traceview1).
2. Run:  python3 glasses_sim.py            (or: python3 glasses_sim.py 192.168.4.1 7070)
3. It prints what the glasses would show. Type a key + Enter to act like the glasses button:
     n = next step (1 press)   p = previous step (2 presses)   h = hide/show panels (hold)
     v <words> = send a voice command, e.g.  v start job      q = quit
No extra packages needed (Python 3.8+).
"""
import json, socket, sys, threading

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 7070
state = {"job": None, "ui": {"panels": 1, "side": "right"}, "live": {}, "dm1": {}, "veh": {}}


def show():
    j, ui, lv, dm = state["job"], state["ui"], state["live"], state["dm1"]
    print("\n" + "=" * 60)
    veh = state["veh"].get("name", "")
    print(f" GLASSES VIEW   {veh}   panels={'ON' if ui.get('panels') else 'HIDDEN'} ({ui.get('side')} lens)")
    codes = dm.get("codes") or []
    if codes:
        print(" ⚠ " + ", ".join(f"SPN {c['spn']} FMI {c['fmi']}" for c in codes))
    if ui.get("panels") and j:
        if j.get("active"):
            print(f" STEP {j['i'] + 1} OF {j['n']}: {j['step']}   [highlight {j['comp']}]")
            for k, s in enumerate(j.get("sub", []), 1):
                print(f"   {k}. {s}")
            if j.get("torque"):
                print(f"   torque: {j['torque']}")
        else:
            print(" Job complete" if j.get("complete") else " No job running (start it on the Tab5)")
    if lv:
        print(f" bat {lv.get('bat')} V   rpm {lv.get('rpm')}   coolant {lv.get('cool')} F")
    print("=" * 60 + "\n[n]ext  [p]rev  [h]ide panels  v <voice>  [q]uit > ", end="", flush=True)


def reader(sock):
    buf = b""
    while True:
        data = sock.recv(4096)
        if not data:
            print("\nTab5 closed the connection."); return
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try:
                m = json.loads(line)
            except ValueError:
                continue
            t = m.get("t")
            if t == "hello":
                print(f"Connected to {m.get('name')} (protocol v{m.get('v')})")
            elif t in ("job", "ui", "veh"):
                state[t] = m; show()
            elif t in ("live", "dm1"):
                changed = t == "dm1" and m.get("codes") != state["dm1"].get("codes")
                state[t] = m
                if changed: show()


def main():
    print(f"Connecting to {host}:{port} ...")
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(None)
    threading.Thread(target=reader, args=(sock,), daemon=True).start()
    send = lambda obj: sock.sendall((json.dumps(obj) + "\n").encode())
    for raw in sys.stdin:
        cmd = raw.strip()
        if cmd == "q": break
        elif cmd == "n": send({"t": "btn", "a": "next"})
        elif cmd == "p": send({"t": "btn", "a": "prev"})
        elif cmd == "h": send({"t": "btn", "a": "panels"})
        elif cmd.startswith("v "): send({"t": "voice", "text": cmd[2:]})
    sock.close()


if __name__ == "__main__":
    main()
