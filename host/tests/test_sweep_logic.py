"""Unit tests for the sweep hit detection.  Run from host/:  py -m unittest discover tests"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from sweep_logic import PollIntervalChecker  # noqa: E402

POLL = (0x11, 0x00, 0x27)
CHECK = (0x11, 0x00, 0x21)
MILK = (0x12, 0x46, 0x57, 0x01, 0x01, 0x32)
S = 1_000_000


def us(seconds: float) -> int:
    return int(round(seconds * S))


class PollIntervalTest(unittest.TestCase):
    def setUp(self):
        self.c = PollIntervalChecker()

    def feed(self, data, t):
        return self.c.feed(0x481, data, us(t))

    def test_real_sequence_with_21_is_not_a_hit(self):
        # measured on the machine: the poll timer restarts after 481 11 00 21
        self.assertIsNone(self.feed(POLL, 41.54))
        self.assertIsNone(self.feed(CHECK, 45.22))
        self.assertIsNone(self.feed(POLL, 50.33))          # 5.11 s after the 21, 8.79 s after the last 27
        self.assertIsNone(self.c.overdue(us(50.40)))

    def test_regular_polls(self):
        t = 10.0
        for _ in range(5):
            self.assertIsNone(self.feed(POLL, t))
            t += 5.12

    def test_too_short_and_too_long(self):
        self.feed(POLL, 0.0)
        self.assertIn("interval", self.feed(POLL, 3.0))
        self.assertIn("interval", self.feed(POLL, 3.0 + 6.5))

    def test_first_poll_is_never_a_hit(self):
        self.assertIsNone(self.feed(POLL, 123.0))

    def test_milk_command_suspends_checking_for_3s(self):
        self.feed(POLL, 0.0)
        self.feed(MILK, 2.0)
        self.assertIsNone(self.feed(POLL, 4.0))            # 4.0 s would be a hit, but suppressed until 5.0
        self.assertIsNone(self.feed(POLL, 4.0 + 5.12))     # normal again, measured from the poll at 4.0
        self.assertIn("interval", self.feed(POLL, 9.12 + 2.0))   # suppression over -> detected again

    def test_overdue_reported_once(self):
        self.feed(POLL, 0.0)
        self.assertIsNone(self.c.overdue(us(5.9)))
        self.assertIn("no poll", self.c.overdue(us(6.5)))
        self.assertIsNone(self.c.overdue(us(7.0)))          # only once
        self.assertIsNone(self.feed(CHECK, 7.5))            # a 21 restarts the timer
        self.assertIsNone(self.c.overdue(us(12.0)))

    def test_overdue_suppressed_after_milk_command(self):
        self.feed(POLL, 0.0)
        self.feed(MILK, 5.0)
        self.assertIsNone(self.c.overdue(us(7.0)))          # within 3 s after the milk command

    def test_other_ids_and_data_ignored(self):
        self.feed(POLL, 0.0)
        self.assertIsNone(self.c.feed(0x4C6, list(POLL), us(1.0)))
        self.assertIsNone(self.feed((0x12, 0x46, 0x55), 2.0))
        self.assertIsNone(self.feed(POLL, 5.12))

    def test_reset_forgets_reference(self):
        self.feed(POLL, 0.0)
        self.c.reset()
        self.assertIsNone(self.feed(POLL, 1.0))            # after a pause the first poll is not judged


if __name__ == "__main__":
    unittest.main()
