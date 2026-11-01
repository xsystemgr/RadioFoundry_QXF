# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Synthetic ELF tests: reservations count even when they have no file bytes."""

import struct
import unittest

import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "inspect_elf_memory", Path(__file__).resolve().parents[1] / "inspect_elf_memory.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
inspect_regions = module.inspect_memory


def inspect_memory(data, budget, reserve=0):
    return inspect_regions(data, budget, reserve, ram_regions=[(0x20000000, 0x20020000), (0x10000000, 0x10010000)])


def executable():
    names = b"\0.shstrtab\0.text\0.data\0.bss\0.ccmram\0.debug_info\0"
    payload = bytearray(512)
    struct.pack_into("<16sHHIIIIIHHHHHH", payload, 0,
                     b"\x7fELF\x01\x01\x01" + bytes(9), 2, 40, 1,
                     0, 0, 128, 0, 52, 0, 0, 40, 7, 1)
    payload[420:420 + len(names)] = names
    records = [(0, 0, 0, 0, 0, 0), (1, 3, 0, 0, 420, len(names)),
               (11, 1, 6, 0x08002800, 408, 4),
               (17, 1, 3, 0x20000000, 412, 4),
               (23, 8, 3, 0x20000004, 9999, 100),
               (28, 8, 3, 0x10000000, 9999, 200),
               (36, 1, 0, 0, 416, 4)]
    for index, record in enumerate(records):
        struct.pack_into("<10I", payload, 128 + index * 40, *record, 0, 0, 1, 0)
    return payload


class MemoryTests(unittest.TestCase):
    def test_counts_separate_ram_and_nobits_but_not_code_or_debug(self):
        report = inspect_memory(executable(), 400)
        self.assertEqual(report["allocated_ram_bytes"], 304)
        self.assertEqual([s["name"] for s in report["sections"]], [".ccmram", ".bss", ".data"])

    def test_exact_budget_and_reserve(self):
        self.assertTrue(inspect_memory(executable(), 320, 16)["fits_aggregate_budget"])
        report = inspect_memory(executable(), 319, 16)
        self.assertFalse(report["fits_aggregate_budget"])
        self.assertEqual(report["remaining_bytes"], -1)

    def test_readonly_ram_is_counted_and_writable_flash_is_excluded(self):
        data = executable()
        struct.pack_into("<I", data, 128 + 2 * 40 + 8, 3)
        struct.pack_into("<I", data, 128 + 3 * 40 + 8, 2)
        self.assertEqual(inspect_memory(data, 400)["allocated_ram_bytes"], 304)

    def test_crossing_region_and_overlapping_intervals(self):
        for regions in [[(0x20000000, 0x20000002)], [(0, 4), (2, 6)], []]:
            with self.assertRaises(ValueError):
                inspect_regions(executable(), 400, ram_regions=regions)

    def test_invalid_budgets(self):
        for budget, reserve in [(0, 0), (-1, 0), (100, -1)]:
            with self.assertRaises(ValueError):
                inspect_memory(executable(), budget, reserve)

    def test_rejects_non_elf_and_other_class_or_endianness(self):
        for data in [b"", bytes(512), b"\x7fELF\x02\x01\x01" + bytes(505),
                     b"\x7fELF\x01\x02\x01" + bytes(505)]:
            with self.assertRaises(ValueError):
                inspect_memory(data, 400)

    def test_rejects_relocatable_object(self):
        data = executable()
        struct.pack_into("<H", data, 16, 1)
        with self.assertRaises(ValueError):
            inspect_memory(data, 400)

    def test_rejects_truncated_tables_and_file_backed_sections(self):
        for data in [executable()[:400], executable()[:450]]:
            with self.assertRaises(ValueError):
                inspect_memory(data, 400)
        data = executable()
        struct.pack_into("<I", data, 128 + 3 * 40 + 20, 99999)
        with self.assertRaises(ValueError):
            inspect_memory(data, 400)


if __name__ == "__main__":
    unittest.main()
