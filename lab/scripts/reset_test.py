"""Which command clears error 210? Sends 480#00 01 XX 00 00 00 00 00 one at a time and watches
for an EXTRA payment check: 481#11 00 21 without the minute-check frames (181#00 00 03/01) around it.
args: cmds=22,23,24,25  wait=20 (s per command)
"""
import time

def parse(spec):
    out = []
    for part in str(spec).split(","):
        part = part.strip()
        if not part: continue
        if "-" in part:
            a, b = part.split("-"); out += list(range(int(a, 16), int(b, 16) + 1))
        else:
            out.append(int(part, 16))
    return out

def hexs(d): return " ".join(f"{b:02X}" for b in d)

def extra_checks(frames, t0):
    rx = [f for f in frames if not getattr(f, "tx", False)]
    minute = [f.wall for f in rx if f.can_id == 0x181]
    out = []
    for f in rx:
        if f.can_id == 0x481 and bytes(f.data[:3]) == b"\x11\x00\x21":
            if not any(abs(f.wall - m) < 1.0 for m in minute):
                out.append(f.wall - t0)
    return out

def unusual(frames):
    seen = {}
    for f in frames:
        if getattr(f, "tx", False): continue
        d = bytes(f.data)
        if f.can_id in (0x446, 0x4C6, 0x181): continue
        if f.can_id == 0x401 and d[:2] in (b"\x00\x46", b"\x00\x00"): continue
        if f.can_id == 0x481 and d and d[0] in (0x11, 0x12): continue
        k = f"{f.can_id:03X}#{hexs(d)}"; seen[k] = seen.get(k, 0) + 1
    return ", ".join(f"{k} x{n}" for k, n in seen.items())

CMDS = parse(args.get("cmds", "22,23,24,25"))
WAIT = float(args.get("wait", 20))
mode("active")
mark(f"RESET TEST start cmds={args.get('cmds','22,23,24,25')} - watch the display")
for c in CMDS:
    mark(f"RESET send cmd {c:02X}")
    send(0x480, f"00 01 {c:02X} 00 00 00 00 00")
    t0 = time.time()
    fr = collect(WAIT)
    ex = extra_checks(fr, t0)
    other = unusual(fr)
    info = (f"extra 11 00 21 after {', '.join(f'{x:.1f}s' for x in ex)}" if ex else "no extra check") + (f"; other: {other}" if other else "")
    csv("reset", cmd=f"{c:02X}", extra_check=len(ex), detail=info[:300])
    mark(f"RESET cmd {c:02X}: {info[:150]}")
    result(f"cmd {c:02X}", bool(ex), info[:200])
mark("RESET TEST end")
