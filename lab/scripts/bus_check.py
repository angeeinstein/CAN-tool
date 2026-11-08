"""Quick pipeline/bus check (~10 s, read-only except one error-list query):
1. listen 5 s (LISTEN mode, no transmit): is the machine on the bus? (481#12 46 55 every ~2 s)
2. if yes: ACTIVE, query the error list once (cmd 03 param 00 -> 0x581), report active error code
args: query=1 (0 = listen only)
"""
def hexs(d): return " ".join(f"{x:02X}" for x in d)

mark("BUSCHECK start (listen 5 s)")
fr = collect(5.0)
rx = [f for f in fr if not f.tx]
polls = sum(1 for f in rx if f.can_id == 0x481 and tuple(f.data[:3]) == (0x12, 0x46, 0x55))
ids = sorted({f.can_id for f in rx})
on_bus = polls >= 1
info = f"{len(rx)} frames, milk polls {polls}, ids {' '.join(f'{i:03X}' for i in ids)}"
mark(f"BUSCHECK machine {'PRESENT' if on_bus else 'NOT SEEN'}: {info}")
csv("buscheck", step="listen", machine=int(on_bus), detail=info)
result("machine on bus", on_bus, info)

if on_bus and int(args.get("query", 1)):
    mode("active")
    send(0x480, "00 01 03 00 00 00 00 00")
    ans = [tuple(f.data) for f in collect(1.0) if not f.tx and f.can_id == 0x581]
    code = ans[1][7] if len(ans) >= 2 and len(ans[1]) >= 8 else None
    detail = " | ".join(hexs(a) for a in ans) or "no answer"
    state = "none" if code == 0 else (f"0x{code:02X} = {code}" if code is not None else "?")
    mark(f"BUSCHECK error list: active error {state}  ({detail})")
    csv("buscheck", step="errorlist", machine=1, detail=detail)
    result("error list", code is not None, f"active error {state}")
