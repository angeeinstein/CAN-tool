"""Quiet-bus probe for the new 0x581 channel. Read-only requests, one at a time.
Step 1: command 03 param 00 on 0x480 (gave the 581 answer containing D2 = 210).
Step 2: CANopen-style SDO upload requests on 0x601 (read device type, name, versions, identity).
Stops early if the 481#10 80 flood appears.
"""
def hexs(d): return " ".join(f"{b:02X}" for b in d)

def is_flood(frames):
    return any(f.can_id == 0x481 and f.data and f.data[0] == 0x10 and not getattr(f, "tx", False) for f in frames)

def interesting(frames):
    out = []
    for f in frames:
        if getattr(f, "tx", False): continue
        if f.can_id in (0x446, 0x4C6, 0x401): continue
        if f.can_id == 0x481 and f.data and f.data[0] in (0x11, 0x12): continue
        out.append(f"{f.can_id:03X}#{hexs(f.data)}")
    return out

REQS = [("cmd03 p00", 0x480, "00 01 03 00 00 00 00 00")]
for idx, sub, name in [(0x1000,0,"device type"),(0x1008,0,"device name"),(0x1009,0,"hw version"),
                       (0x100A,0,"sw version"),(0x1018,1,"vendor id"),(0x1018,2,"product code"),
                       (0x1018,3,"revision"),(0x1018,4,"serial")]:
    REQS.append((f"SDO {idx:04X}.{sub} {name}", 0x601, f"40 {idx & 0xFF:02X} {idx >> 8:02X} {sub:02X} 00 00 00 00"))

only = args.get("only", "")
mode("active")
base = collect(3.0)
if is_flood(base):
    mark("PROBE aborted: flood already running"); result("quiet bus", False, "flood running - restart machine first")
else:
    for label, cid, data in REQS:
        if only and only not in label: continue
        mark(f"PROBE {label}: {cid:03X}#{data}")
        send(cid, data)
        fr = collect(1.5)
        hits = interesting(fr)
        csv("probe", label=label, id=f"{cid:03X}", data=data, reply=" | ".join(hits)[:300])
        result(label, bool(hits), " | ".join(hits)[:200] or "no reply")
        if is_flood(fr):
            mark("PROBE stop: flood started"); break
