"""Generic reply sweep: answer a trigger frame with a list of candidate replies and watch for reactions.

Run (from host/):
  py franke_can.py socket://192.168.29.195:3333 script scripts/sweep_reply.py
  py franke_can.py COM17 script scripts/sweep_reply.py start=312          # begin at candidate 312 (1-based)
  py franke_can.py ... script scripts/sweep_reply.py resume=1               # continue after the last tested one
  py franke_can.py ... script scripts/sweep_reply.py list=1                 # only print the candidate list
  Other args: per=2 (triggers per candidate), pause_on_hit=0 (keep going after a hit), limit=50 (stop after N)

How it works
- The answer is a firmware RULE ("sweep"): trigger -> ACK after 1 ms, candidate after 3 ms. The rule engine
  answers with exact timing; this script only swaps the rule for each candidate.
- Each candidate stays active for PER_CANDIDATE triggers, then the next one.
- Hit detection (any of):
    a) poll interval < INTERVAL_MIN or > INTERVAL_MAX (normal: 5.12 s), or no poll for > INTERVAL_MAX.
       Measured from the last 481 11 00 27 OR 481 11 00 21, whichever came later (the machine restarts
       its poll timer after the once-per-minute 21). Not checked for 3 s after a milk command 481 12 46 57.
       Logic + unit tests: host/sweep_logic.py, host/tests/test_sweep_logic.py
    b) a received frame whose ID+data is not in KNOWN_FRAMES
    c) a frame on the trigger ID with the trigger's first two bytes but a command byte not in KNOWN_CMDS
- On a hit: the candidate stays active, a marker "SWEEP HIT ..." is set and the script asks what to do.
- Every tested candidate is written to Messungen/<log>_sweep.csv; progress is kept in scripts/state/
  so "resume=1" continues after a stop, crash or reboot.
- Every candidate change sets a marker "SWEEP <n>/<total> <candidate>", so the log shows what was active.

Device-specific values (Franke FM850) live here at the top, not in the firmware.
"""
import itertools

from sweep_logic import PollIntervalChecker

# ---------------------------------------------------------------- configuration
TRIGGER_ID = 0x481
TRIGGER_DATA = "11 00 27"            # poll to the payment system (every 5.12 s)
ACK_ID, ACK_DATA, ACK_DELAY_MS = 0x400, "00 01 09 00 00 00 00 00", 1
REPLY_ID, REPLY_DELAY_MS = 0x480, 3

# candidates: template with variable byte positions Bn, each with its list of values
TEMPLATE = "B0 01 28 B3 00 00 00 00"
VARS = {"B0": [0x00, 0x01, 0x10, 0x11], "B3": range(0x100)}
EXTRA_TEMPLATES = [("B0 01 28", {"B0": [0x00, 0x01, 0x10, 0x11]})]   # e.g. 3-byte variants

PER_CANDIDATE = int(args.get("per", 2))
INTERVAL_MIN = float(args.get("interval_min", 4.5))   # seconds between triggers; outside = hit
INTERVAL_MAX = float(args.get("interval_max", 6.0))
TIMER_RESET_DATA = ["11 00 21"]                # on TRIGGER_ID: restarts the machine's poll timer
SUPPRESS_DATA = ["12 46 57"]                   # on TRIGGER_ID: milk command -> no interval check ...
SUPPRESS_S = float(args.get("suppress_s", 3.0))   # ... for this many seconds
KNOWN_CMDS = {0x21, 0x27}                      # command byte (3rd byte) on the trigger ID
KNOWN_FRAMES = [                               # ID:data, "**" = any byte; length must match exactly
    "481:12 46 55",
    "446:00 01 09 00 00 00 00 00",
    "4C6:00 01 56 ** ** 02 00 **",
    "401:00 46 09",
    "401:00 00 09",
    "481:11 00 27",
    "481:11 00 21",
    "481:12 46 51 06 08 00 00 00",
    "4C6:00 01 52 00 14 00 01 00",
    "481:12 46 57 ** ** **",
    "4C6:00 01 58 ** 02 00 00 00",
    "181:00 00 03",
    "181:00 00 01",
    "481:12 46 00 00 00 00 00 00",
]


# ---------------------------------------------------------------- helpers
def expand(template, variables):
    toks = template.split()
    names = [t for t in toks if t in variables]
    for values in itertools.product(*(variables[n] for n in names)):
        sub = dict(zip(names, values))
        yield " ".join(f"{sub[t]:02X}" if t in sub else t for t in toks)


def parse_known(spec):
    id_s, data = spec.split(":")
    return int(id_s, 16), [None if b == "**" else int(b, 16) for b in data.split()]


KNOWN = [parse_known(s) for s in KNOWN_FRAMES]
TRIG_PAT = make_pattern(TRIGGER_DATA)


def is_known(f):
    for kid, kdata in KNOWN:
        if f.can_id == kid and len(f.data) == len(kdata) and all(k is None or k == b for k, b in zip(kdata, f.data)):
            return True
    return False


def is_trigger(f):
    return matches(f, TRIGGER_ID, TRIG_PAT)


trig_bytes = [int(b, 16) for b in TRIGGER_DATA.split()]

candidates = list(expand(TEMPLATE, VARS))
for tpl, var in EXTRA_TEMPLATES:
    candidates += list(expand(tpl, var))
total = len(candidates)

if args.get("list"):
    for i, c in enumerate(candidates, 1):
        print(f"{i:5} {c}")
    exit()

# ---------------------------------------------------------------- start position
if "start" in args:
    first = int(args["start"]) - 1
elif args.get("resume"):
    first = int(state.get("next_index", 0))
else:
    first = 0
first = max(0, min(first, total))
limit = int(args.get("limit", total))
pause_on_hit = bool(args.get("pause_on_hit", True))
log(f"{total} candidates, starting at {first + 1}, {PER_CANDIDATE} triggers each "
    f"(~{(total - first) * PER_CANDIDATE * 5.12 / 3600:.1f} h)")

mode("active")
mark(f"SWEEP START at {first + 1}/{total}")
drain()
checker = PollIntervalChecker(
    TRIGGER_ID, trig_bytes,
    resets=[[int(b, 16) for b in d.split()] for d in TIMER_RESET_DATA],
    suppress=[[int(b, 16) for b in d.split()] for d in SUPPRESS_DATA], suppress_s=SUPPRESS_S,
    min_s=INTERVAL_MIN, max_s=INTERVAL_MAX)
tested = 0

try:
    for idx in range(first, total):
        if tested >= limit:
            log(f"limit of {limit} candidates reached")
            break
        cand = candidates[idx]
        rule(f"add sweep match={TRIGGER_ID:03X}:{TRIGGER_DATA} "
             f"send={ACK_ID:03X}:{ACK_DATA}@{ACK_DELAY_MS} send={REPLY_ID:03X}:{cand}@{REPLY_DELAY_MS}")
        mark(f"SWEEP {idx + 1}/{total} {cand}")
        status(f"candidate {idx + 1}/{total}: {cand}")

        triggers, hit = 0, None
        while triggers < PER_CANDIDATE and not hit:
            f = next_frame(timeout=1.0)
            if f is None:   # quiet bus: only check whether the poll is overdue
                hit = checker.overdue(now_us() or 0)
                continue
            hit = checker.feed(f.can_id, f.data, f.dev_us) or checker.overdue(f.dev_us)
            if hit:
                break
            if is_trigger(f):
                triggers += 1
            elif f.can_id == TRIGGER_ID and len(f.data) >= 3 and list(f.data[:2]) == trig_bytes[:2] \
                    and f.data[2] not in KNOWN_CMDS:
                hit = f"new command byte {f.data[2]:02X} on {TRIGGER_ID:03X}: {f.data_str}"
            elif not is_known(f):
                hit = f"unknown frame {f.id_str}#{f.data_str} ({f.meaning or 'unknown'})"

        csv("sweep", utc=utc_iso(dev.wall_of(now_us() or 0)), index=idx + 1, candidate=cand,
            triggers_seen=triggers, hit=1 if hit else 0, reason=hit or "")
        state["next_index"] = idx + 1
        state["last_candidate"] = cand
        tested += 1

        if hit:
            mark(f"SWEEP HIT {idx + 1}/{total} {cand}: {hit}")
            result(f"hit {cand}", True, hit)
            if pause_on_hit:
                ans = ask(f"HIT with {cand} ({hit}). The candidate stays active. "
                          f"[c]ontinue sweep / [s]top here: ").lower()
                if ans.startswith("s"):
                    log(f"stopped at hit; resume later with start={idx + 2} or resume=1")
                    break
                drain()
                checker.reset()   # the interval across the pause is not meaningful
    else:
        log("sweep complete")
        mark("SWEEP COMPLETE")
finally:
    status(f"sweep ended at candidate {state.get('next_index', first)}/{total}")
