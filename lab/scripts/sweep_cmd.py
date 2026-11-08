"""Sweep the command byte: 480#00 01 XX PP 00 00 00 00, one command at a time, quiet bus only.
Records every answer (581 or anything unusual) and whether the machine ACKed (401 00 00 09).
Stops immediately if the 481#10 80 flood starts (then restart the machine).
args: cmds=00-FF (default, 03 skipped), param=0, period=1.5, skip=03
"""
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

def is_flood(frames):
    # a single 481#10 80 ... is a normal answer (cmd 05); a flood is many of them
    return sum(1 for f in frames if not getattr(f, "tx", False) and f.can_id == 0x481 and f.data and f.data[0] == 0x10) >= 5

def acked(frames):
    return any(not getattr(f, "tx", False) and f.can_id == 0x401 and bytes(f.data[:3]) == b"\x00\x00\x09" for f in frames)

def interesting(frames):
    out = {}
    for f in frames:
        if getattr(f, "tx", False): continue
        d = bytes(f.data)
        if f.can_id in (0x446, 0x4C6): continue
        if f.can_id == 0x401 and d[:2] in (b"\x00\x46", b"\x00\x00"): continue
        if f.can_id == 0x481 and d and d[0] in (0x11, 0x12): continue
        if f.can_id == 0x181 and d in (b"\x00\x00\x03", b"\x00\x00\x01"): continue
        k = f"{f.can_id:03X}#{hexs(d)}"
        out[k] = out.get(k, 0) + 1
    return out

CMDS = parse(args.get("cmds", "00-FF"))
SKIP = set(parse(args.get("skip", "03")))
PARAM = int(args.get("param", 0))
PERIOD = float(args.get("period", 1.5))

mode("active")
if is_flood(collect(3.0)):
    mark("SWEEP aborted: flood running"); result("quiet bus", False, "restart machine first")
else:
    mark(f"SWEEP CMD start cmds={args.get('cmds','00-FF')} param={PARAM:02X}")
    for c in CMDS:
        if c in SKIP: continue
        data = f"00 01 {c:02X} {PARAM:02X} 00 00 00 00"
        send(0x480, data)
        fr = collect(PERIOD)
        hits = interesting(fr)
        ack = acked(fr)
        desc = ", ".join(f"{k} x{n}" for k, n in hits.items())
        csv("cmd", cmd=f"{c:02X}", param=f"{PARAM:02X}", ack=int(ack), reply=desc[:300])
        if hits:
            mark(f"SWEEP cmd {c:02X}: ack={int(ack)} {desc[:150]}")
            result(f"cmd {c:02X}", bool(hits), f"ack={int(ack)} {desc[:200] or 'no reply'}")
        if is_flood(fr):
            mark(f"SWEEP STOP: flood after cmd {c:02X}"); result("flood", False, f"after cmd {c:02X} - restart machine")
            break
    mark("SWEEP CMD end")
