"""Known frames of the Franke FM850 CAN bus (spec section 8).

Node 0x01 = coffee machine, 0x46 = milk controller (Foam Master), 0x00 = payment system (assumed).
Node n sends on 0x480+n and acknowledges on 0x400+n. Reply code = request code + 1.

Keep this table in sync with decode() in web/index.html.
"""

from __future__ import annotations

from typing import Callable, Optional, Sequence

# Functions used with command 0x57 (byte 3)
FUNCTIONS = {
    0x01: "milk pump 1",
    0x05: "Y42 air/milk",
    0x0B: "Y46 warm milk",
    0x0C: "Y45 temperature switch",
    0x0D: "Y38 milk supply 1",
    0x10: "Y40 milk supply 2",
    0x11: "function 0x11 (unknown)",
    0xFF: "all off",
}

NODE_NAMES = {0x01: "coffee machine", 0x46: "milk controller", 0x00: "payment system"}


def _h(b: int) -> str:
    return f"{b:02X}"


def _cmd_57(d: Sequence[int]) -> str:
    if len(d) < 6:
        return "command -> milk controller"
    func = FUNCTIONS.get(d[3], f"function {_h(d[3])}")
    onoff = {1: "ON", 0: "OFF"}.get(d[4], f"EE={_h(d[4])}")
    return f"command: {func} {onoff}, value {d[5]} %"


def _status_56(d: Sequence[int]) -> str:
    if len(d) < 8:
        return "status reply (short)"
    if (d[3], d[4]) == (0x01, 0x00):
        pump = "pump off"
    elif (d[3], d[4]) == (0x02, 0x01):
        pump = "pump running"
    else:
        pump = f"SS PP={_h(d[3])} {_h(d[4])}"
    return f"status reply: {pump}, fridge {d[7]} °C"


# (id, data prefix with None = any byte, description or function(data) -> str)
Entry = tuple[int, tuple[Optional[int], ...], "str | Callable[[Sequence[int]], str]"]

KNOWN: list[Entry] = [
    (0x481, (0x12, 0x46, 0x55), "status poll -> milk controller"),
    (0x481, (0x12, 0x46, 0x57), _cmd_57),
    (0x481, (0x12, 0x46, 0x51), "query 0x51 -> milk controller (meaning unknown)"),
    (0x481, (0x12, 0x46, 0x00), "init/reset milk controller (assumed)"),
    (0x481, (0x11, 0x00, 0x27), "poll -> payment system"),
    (0x481, (0x11, 0x00, 0x21), "availability check -> payment system (once per minute, also on screen change)"),
    (0x4C6, (0x00, 0x01, 0x56), _status_56),
    (0x4C6, (0x00, 0x01, 0x58), lambda d: f"command acknowledged ({_h(d[3]) if len(d) > 3 else '?'})"),
    (0x4C6, (0x00, 0x01, 0x52), "reply to query 0x51"),
    (0x446, (0x00, 0x01, 0x09), "receipt ack from milk controller"),
    (0x401, (0x00, 0x46, 0x09), "receipt ack from coffee machine"),
    (0x181, (0x00, 0x00, 0x03), "machine state 03"),
    (0x181, (0x00, 0x00, 0x01), "machine state 01 (ready)"),
    (0x181, (), "machine state"),
    (0x400, (), "ack from node 0x00 (payment system)"),
    (0x480, (), "message from node 0x00 (payment system)"),
]


def decode(can_id: int, data: Sequence[int]) -> Optional[str]:
    """Returns a human readable meaning, or None if the frame is unknown."""
    for kid, prefix, desc in KNOWN:
        if kid != can_id or len(data) < len(prefix):
            continue
        if all(p is None or data[i] == p for i, p in enumerate(prefix)):
            return desc(data) if callable(desc) else desc
    return None


def frame_type(can_id: int, data: Sequence[int]) -> str:
    """Grouping key for statistics: ID plus the matched known-frame prefix (or 'unknown')."""
    for kid, prefix, _ in KNOWN:
        if kid == can_id and len(data) >= len(prefix) and all(p is None or data[i] == p for i, p in enumerate(prefix)):
            pre = " ".join(_h(p) for p in prefix if p is not None)
            return f"{can_id:03X} {pre}".strip()
    return f"{can_id:03X} (unknown)"
