# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure result-contract counterexamples; never starts QEMU."""
import unittest
from test_z9_producer_journal import accepted_result


class JournalReceiptContract(unittest.TestCase):
    def test_complete_success(self):
        self.assertTrue(accepted_result([dict(passed=True, owned_qemu_stopped=True)], 1, True, True, True))

    def test_failure_or_missing_case(self):
        for cases in ([], [dict(passed=False, owned_qemu_stopped=True)],
                      [dict(passed=True, owned_qemu_stopped=False)], [dict(passed=True)]):
            with self.subTest(cases=cases):
                self.assertFalse(accepted_result(cases, 1, True, True, True))

    def test_integrity_and_equivalence_are_required(self):
        cases = [dict(passed=True, owned_qemu_stopped=True)]
        for flags in ((False, True, True), (True, False, True), (True, True, False)):
            with self.subTest(flags=flags):
                self.assertFalse(accepted_result(cases, 1, *flags))

    def test_optimistic_non_boolean_fields_rejected(self):
        self.assertFalse(accepted_result([dict(passed=1, owned_qemu_stopped=True)], 1, True, True, True))
        self.assertFalse(accepted_result([dict(passed=True, owned_qemu_stopped=True)], 1, 'yes', True, True))


if __name__ == '__main__': unittest.main()
