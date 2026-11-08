"""Does answering the payment check 481#11 00 21 with 480#00 01 22 00 ... clear error 210?
Phase A (baseline, no answer):  cmd 24 -> watch 25 s  (expected: checks at ~0.2 s and ~7 s, error back ~16 s)
Phase B (answer 22 00):         rule on, cmd 24 -> watch B s (default 120)
Phase C (answer removed):       rule off -> watch C s (default 120): when does the error come back?
Watch the display; set markers "weg" / "da". The script prints every payment check with its time.
args: reply="00 01 22 00 00 00 00 00"  b=120  c=120  ack=1
"""
import time
REPLY = str(args.get("reply", "00 01 22 00 00 00 00 00"))
B = float(args.get("b", 120)); C = float(args.get("c", 120))
use_ack = bool(int(args.get("ack", 1)))

def hexs(d): return " ".join(f"{x:02X}" for x in d)

def watch(seconds, label, t0):
    """collect in 1 s slices so every event is printed live"""
    end = time.time() + seconds
    checks = []; other = {}
    while time.time() < end:
        for f in collect(min(1.0, max(0.05, end - time.time()))):
            if f.tx: continue
            d = tuple(f.data)
            if f.can_id == 0x481 and d[:3] == (0x11, 0x00, 0x21):
                dt = f.wall - t0; checks.append(round(dt, 1))
                log(f"[{label}] payment check 21 at +{dt:.1f} s")
            elif f.can_id == 0x481 and d[:3] == (0x11, 0x00, 0x27):
                pass
            elif f.can_id in (0x446, 0x4C6, 0x181) or (f.can_id == 0x401 and d[:2] in ((0, 0x46), (0, 0))) \
                    or (f.can_id == 0x481 and d and d[0] == 0x12):
                pass
            else:
                k = f"{f.can_id:03X}#{hexs(d)}"
                if k not in other: log(f"[{label}] NEW {k} at +{f.wall - t0:.1f} s")
                other[k] = other.get(k, 0) + 1
    gaps = [round(b - a, 1) for a, b in zip(checks, checks[1:])]
    info = f"checks at {checks}, gaps {gaps}" + (f", other {other}" if other else "")
    mark(f"TEST22 {label} summary: {info[:200]}")
    csv("test22", phase=label, checks=str(checks), gaps=str(gaps), other=str(other)[:200])
    return checks

mode("active")
mark("TEST22 phase A: baseline, NO answer, sending cmd 24 - error should vanish and come back after ~16 s")
t0 = time.time(); send(0x480, "00 01 24 00 00 00 00 00")
a = watch(25, "A", t0)
result("A baseline", len(a) >= 2, f"checks {a}")

parts = (["send=400:00 01 09 00 00 00 00 00@1"] if use_ack else []) + [f"send=480:{REPLY}@3"]
rule(f"add r21 match=481:11 00 21 {' '.join(parts)}")
mark(f"TEST22 phase B: answering 21 with 480#{REPLY}, sending cmd 24 - WATCH THE DISPLAY")
t0 = time.time(); send(0x480, "00 01 24 00 00 00 00 00")
b = watch(B, "B", t0)
result("B with answer", True, f"checks {b}")

rule("del r21")
mark("TEST22 phase C: answer removed - when does the error come back?")
c = watch(C, "C", time.time())
result("C answer removed", True, f"checks {c}")
mark("TEST22 end")
