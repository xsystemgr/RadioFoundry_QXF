# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors

import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    "inspect_image_header", Path(__file__).resolve().parents[1] / "inspect_image_header.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ImageHeaderTest(unittest.TestCase):
    def image(self, sp=0x20001000, reset=0x08004041):
        return struct.pack("<16I", sp, reset, *([0] * 14)) + bytes(64)

    def test_valid_relocated_image(self):
        report = module.inspect_image(self.image(), 0x08004000)
        self.assertTrue(all(report["checks"].values()))
        self.assertEqual(report["reset_file_offset"], 64)
        self.assertEqual(report["end_exclusive"], "0x08004080")

    def test_wrong_base(self):
        report = module.inspect_image(self.image(), 0x08000000)
        self.assertFalse(report["checks"]["reset_in_supplied_image"])
        self.assertIsNone(report["reset_file_offset"])

    def test_even_reset(self):
        report = module.inspect_image(self.image(reset=0x08004040), 0x08004000)
        self.assertFalse(report["checks"]["reset_thumb_bit"])

    def test_bad_stack(self):
        for sp in (0, 0x20001001):
            self.assertFalse(module.inspect_image(self.image(sp=sp), 0x08004000)
                             ["checks"]["sp_nonzero_and_word_aligned"])

    def test_reset_end_is_outside(self):
        report = module.inspect_image(self.image(reset=0x08004081), 0x08004000)
        self.assertFalse(report["checks"]["reset_in_supplied_image"])

    def test_invalid_input(self):
        for data, base in ((bytes(63), 0), (self.image(), -4),
                           (self.image(), 1), (self.image(), 0xFFFFFFFC)):
            with self.subTest(base=base, length=len(data)):
                with self.assertRaises(ValueError):
                    module.inspect_image(data, base)


if __name__ == "__main__":
    unittest.main()
