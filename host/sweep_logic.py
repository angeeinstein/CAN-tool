"""Pure logic for reply sweeps (no device access) - unit tested in host/tests/test_sweep_logic.py."""

from __future__ import annotations

from typing import Iterable, Optional, Sequence


def starts_with(data: Sequence[int], prefix: Sequence[int]) -> bool:
    return len(data) >= len(prefix) and all(a == b for a, b in zip(data, prefix))


class PollIntervalChecker:
    """Checks the interval of a periodic poll (e.g. 481 11 00 27 every 5.12 s).

    - The interval is measured from the last poll OR from the last "timer reset" frame, whichever came
      later (the machine restarts its 5.12 s poll timer after sending 481 11 00 21).
    - For `suppress_s` seconds after a "suppress" frame (481 12 46 57 .. milk command) intervals are not
      checked (the reference time is still updated).
    - feed() returns a hit reason or None; overdue() reports a missing poll.
    """

    def __init__(self, can_id: int = 0x481, poll: Sequence[int] = (0x11, 0x00, 0x27),
                 resets: Iterable[Sequence[int]] = ((0x11, 0x00, 0x21),),
                 suppress: Iterable[Sequence[int]] = ((0x12, 0x46, 0x57),), suppress_s: float = 3.0,
                 min_s: float = 4.5, max_s: float = 6.0):
        self.can_id = can_id
        self.poll = tuple(poll)
        self.resets = [tuple(r) for r in resets]
        self.suppress = [tuple(s) for s in suppress]
        self.suppress_us = int(suppress_s * 1e6)
        self.min_s, self.max_s = min_s, max_s
        self.reset()

    def reset(self) -> None:
        """Forget the reference (e.g. after a pause); the next poll is not checked."""
        self.last_ref_us: Optional[int] = None
        self.suppress_until_us = -1
        self._overdue_reported = False

    def _suppressed(self, t_us: int) -> bool:
        return t_us < self.suppress_until_us

    def feed(self, can_id: int, data: Sequence[int], t_us: int) -> Optional[str]:
        if can_id != self.can_id:
            return None
        if any(starts_with(data, s) for s in self.suppress):
            self.suppress_until_us = max(self.suppress_until_us, t_us + self.suppress_us)
            return None
        if any(starts_with(data, r) for r in self.resets):
            self.last_ref_us = t_us
            self._overdue_reported = False
            return None
        if not starts_with(data, self.poll):
            return None
        reason = None
        if self.last_ref_us is not None and not self._suppressed(t_us):
            dt = (t_us - self.last_ref_us) / 1e6
            if not (self.min_s <= dt <= self.max_s):
                reason = f"poll interval {dt:.2f} s (expected {self.min_s}-{self.max_s} s)"
        self.last_ref_us = t_us
        self._overdue_reported = False
        return reason

    def overdue(self, now_us: int) -> Optional[str]:
        """Reports once if no poll/reset came for longer than max_s (outside a suppression window)."""
        if self.last_ref_us is None or self._overdue_reported or self._suppressed(now_us):
            return None
        dt = (now_us - self.last_ref_us) / 1e6
        if dt > self.max_s:
            self._overdue_reported = True
            return f"no poll for {dt:.2f} s (expected every {self.min_s}-{self.max_s} s)"
        return None
