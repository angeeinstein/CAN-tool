"""Payment-system emulation, test plan steps 2 + 3 (spec section 9).

Run:  py franke_can.py COM7 script scripts/test_kasse.py
      py franke_can.py socket://franke-can.local:3333 script scripts/test_kasse.py

Available functions (injected by franke_can.py):
  mode(m)  rule(text)  send(frame | id, data)  cmd("#...")
  wait_for(id, data='11 00 ??', timeout=1.0)  collect(seconds, id, data)  intervals(frames)
  log(msg)  result(name, ok, details)  ask(question)  sleep(s)
The script ends with #stop (LISTEN, rules disabled) unless --keep-active is given.
Everything (frames, commands, results) is written to Messungen/can_<date>_<time>_test_kasse.*
"""

POLL = "11 00 27"      # 0x481 poll to the payment system, every 5.12 s
REQUEST = "11 00 21"   # 0x481 request (drink tapped / "make ready")

# Response variants for the poll (step 3). Name -> list of "send=" actions.
VARIANTS = {
    "ack_only": "send=400:00 01 09@1",
    "ack_28": "send=400:00 01 09@1 send=480:00 01 28@3",
    "ack_28_credit1": "send=400:00 01 09@1 send=480:00 01 28 01@3",
    "ack_28_credit5": "send=400:00 01 09@1 send=480:00 01 28 00 00 00 00 01@3",
}

# ---- step 1: passive baseline --------------------------------------------------------
mode("listen")
cmd("#rule clear")
log("LISTEN: measuring poll interval for 12 s")
polls = collect(12, 0x481, POLL)
iv = intervals(polls)
result("baseline poll seen", len(polls) >= 2, f"{len(polls)} polls, intervals ms: {[round(x) for x in iv]}")

# ---- step 2: ACTIVE without rules (ACK only) ------------------------------------------
mode("active")
log("ACTIVE without rules: the machine should behave exactly as before")
polls = collect(12, 0x481, POLL)
result("active, no rules: polls continue", len(polls) >= 2, f"{len(polls)} polls")

# ---- step 3: answer the poll with different variants ------------------------------------
for name, actions in VARIANTS.items():
    log(f"variant {name}: {actions}")
    rule(f"add poll match=481:{POLL} {actions}")
    # the rule answers the next poll inside the firmware; we only observe
    got = wait_for(0x481, POLL, timeout=7)
    if not got:
        result(f"{name}: poll received", False, "no poll within 7 s")
        continue
    observed = collect(15)
    new_ids = sorted({f.id_str for f in observed} - {"481", "4C6", "446", "401", "181"})
    poll_iv = intervals([f for f in observed if f.can_id == 0x481 and f.data[:3] == (0x11, 0x00, 0x27)])
    answer = ask(f"[{name}] Are the yellow drinks active / did the display change? (y/n, comment)")
    result(name, answer.lower().startswith("y"),
           f"poll intervals ms {[round(x) for x in poll_iv]}, new IDs {new_ids}, operator: {answer}")
    cmd("#rule del poll")

cmd("#stats")
