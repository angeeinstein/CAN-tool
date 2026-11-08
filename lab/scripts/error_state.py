"""Can we read 'error 210 active' from the bus? Query the error list (cmd 03 param 00 -> answer on 0x581)
  Q1 now (error should be active), then cmd 24 (clears it), Q2 after 2 s (should be cleared),
  Q3 after 25 s (error back). If Q2 differs from Q1/Q3, the 581 answer is an automatic error detector.
args: reply="" (optional: answer 481:11 00 21 with 480#<reply> during the test, e.g. reply="00 01 22 00 00 00 00 00")
"""
import time
def hexs(d): return " ".join(f"{x:02X}" for x in d)

def query(label):
    send(0x480, "00 01 03 00 00 00 00 00")
    fr = collect(1.0)
    ans = [hexs(f.data) for f in fr if not f.tx and f.can_id == 0x581]
    has210 = any(a.endswith("D2") for a in ans)
    info = " | ".join(ans) or "no answer"
    mark(f"ERRSTATE {label}: D2={'yes' if has210 else 'no'}  {info}")
    csv("errstate", step=label, d2=int(has210), answer=info)
    return info

mode("active")
reply = str(args.get("reply", "")).strip()
if reply:
    rule(f"add r21 match=481:11 00 21 send=400:00 01 09 00 00 00 00 00@1 send=480:{reply}@3")
    mark(f"ERRSTATE answering 21 with 480#{reply}")
q1 = query("Q1 before reset")
send(0x480, "00 01 24 00 00 00 00 00")
mark("ERRSTATE cmd 24 sent")
collect(2.0)
q2 = query("Q2 2 s after reset")
collect(25.0)
q3 = query("Q3 27 s after reset")
result("Q2 differs from Q1", q2 != q1, f"Q1: {q1}  //  Q2: {q2}")
result("Q3 back like Q1", q3 == q1, f"Q3: {q3}")
if reply: rule("del r21")
