"""Answer candidates for the payment check 481#11 00 21, judged by the error list (cmd 03 -> 0x581):
the last byte of the 2nd answer frame is the active error (D2 = 210, 00 = none).
Per candidate: install rule(s) -> cmd 24 (clears 210, triggers checks at ~0 s and ~7 s) -> wait W s
-> query error list. HIT = no D2 after W s (error stayed away). Then confirm 30 s later.
args: set=basic | template="480:00 01 22 XX 00 00 00 00" xx=00-FF ; wait=20 ; ack=1 ; stop=1
"""
import time
def hexs(d): return " ".join(f"{x:02X}" for x in d)
def parse(spec):
    out = []
    for part in str(spec).split(","):
        part = part.strip()
        if not part: continue
        if "-" in part:
            a, b = part.split("-"); out += list(range(int(a, 16), int(b, 16) + 1))
        else: out.append(int(part, 16))
    return out

ACK = "400:00 01 09 00 00 00 00 00@1"
# (name, sends on 21, pre-sends before cmd 24)
BASIC = [
    ("control: no answer", None, []),
    ("ack only", [], []),
    ("22 00", ["480:00 01 22 00 00 00 00 00@3"], []),
    ("22 01", ["480:00 01 22 01 00 00 00 00@3"], []),
    ("22 02", ["480:00 01 22 02 00 00 00 00@3"], []),
    ("22 03", ["480:00 01 22 03 00 00 00 00@3"], []),
    ("22 80", ["480:00 01 22 80 00 00 00 00@3"], []),
    ("22 81", ["480:00 01 22 81 00 00 00 00@3"], []),
    ("22 short", ["480:00 01 22@3"], []),
    ("hdr01 22 00", ["480:01 01 22 00 00 00 00 00@3"], []),
    ("hdr10 22 00", ["480:10 01 22 00 00 00 00 00@3"], []),
    ("hdr11 22 00", ["480:11 01 22 00 00 00 00 00@3"], []),
    ("hello 180 01", ["180:00 00 01@3"], []),
    ("hello 180 01 + 22 01", ["180:00 00 01@3", "480:00 01 22 01 00 00 00 00@6"], []),
    ("pre-hello 180 01, 22 01", ["480:00 01 22 01 00 00 00 00@3"], ["00 00 01"]),
    ("like milk 52: 22 00 14 00 01 00", ["480:00 01 22 00 14 00 01 00@3"], []),
]

MATCH = str(args.get("match", "481:11 00 21")); W = float(args.get("wait", 20)); STOP = bool(int(args.get("stop", 1))); use_ack = bool(int(args.get("ack", 1)))
BASIC27 = [
    ("control: no answer", None, []),
    ("ack only", [], []),
] + [(f"28 {x:02X}", [f"480:00 01 28 {x:02X} 00 00 00 00@3"], []) for x in (0x00, 0x01, 0x02, 0x03, 0x04, 0x80, 0x81, 0x82, 0xFF)] + [
    ("28 short", ["480:00 01 28@3"], []),
    ("hdr01 28 01", ["480:01 01 28 01 00 00 00 00@3"], []),
    ("hdr10 28 01", ["480:10 01 28 01 00 00 00 00@3"], []),
    ("hdr11 28 01", ["480:11 01 28 01 00 00 00 00@3"], []),
    ("28 01 + hello", ["480:00 01 28 01 00 00 00 00@3", "180:00 00 01@6"], []),
]
if "template" in args:
    tpl = str(args["template"])
    cands = [(tpl.split(":", 1)[1].replace("XX", f"{x:02X}"), [tpl.replace("XX", f"{x:02X}") + "@3"], [])
             for x in parse(args.get("xx", "00-FF"))]
elif "27" in MATCH.split(":")[1]:
    cands = BASIC27
else:
    cands = BASIC

def err_active():
    send(0x480, "00 01 03 00 00 00 00 00")
    fr = collect(1.0)
    ans = [tuple(f.data) for f in fr if not f.tx and f.can_id == 0x581]
    if len(ans) < 2 or len(ans[1]) < 8:
        return None, " | ".join(hexs(a) for a in ans) or "no answer"
    return ans[1][7] != 0x00, hexs(ans[1])

have_rule = False
mode("active")
# optional fixed second rule, e.g. extra_match="481:11 00 21" extra_reply="00 01 22 00 00 00 00 00"
EXM = str(args.get("extra_match", "")).strip(); EXR = str(args.get("extra_reply", "")).strip()
if EXM:
    ex_parts = [f"send={ACK}"] + ([f"send=480:{EXR}@3"] if EXR else [])
    rule(f"add rx match={EXM} {' '.join(ex_parts)}")
    mark(f"RDETECT fixed extra rule: {EXM} -> ack + 480#{EXR or '(ack only)'}")
mark(f"RDETECT start match={MATCH} {len(cands)} candidates wait={W}")
act, raw = err_active()
mark(f"RDETECT initial error active={act} ({raw})")
for name, sends, pre in cands:
    # make sure the error is latched before each try (otherwise cmd 24 has nothing to clear)
    for _ in range(4):
        act, raw = err_active()
        if act: break
        collect(5.0)
    if not act:
        mark(f"RDETECT [{name}] skipped: error not active ({raw})"); result(name, False, "skipped, error not active"); continue
    if sends is None:
        if have_rule: rule("del r21"); have_rule = False
    else:
        have_rule = True
        parts = ([f"send={ACK}"] if use_ack else []) + [f"send={s}" for s in sends]
        rule(f"add r21 match={MATCH} {' '.join(parts)}")
    for p in pre:
        send(0x180, p)
    send(0x480, "00 01 24 00 00 00 00 00")
    fr = collect(W)
    act, raw = err_active()
    hit = act is False
    info = f"error after {W:.0f} s: {'ACTIVE' if act else ('GONE' if act is False else '?')} ({raw})"
    if hit:
        collect(30.0)
        act2, raw2 = err_active()
        info += f"; after +30 s: {'ACTIVE' if act2 else 'GONE'}"
        hit = act2 is False
    csv("rdetect", cand=name, hit=int(hit), info=info[:250])
    mark(f"RDETECT [{name}]: {info[:180]}")
    result(name, hit, info[:200])
    if hit and STOP:
        mark("RDETECT HIT - stopping"); break
if have_rule: rule("del r21")
if EXM: rule("del rx")
mark("RDETECT end")
