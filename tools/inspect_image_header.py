#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Report a raw Cortex-M image's identity and vector plausibility without disassembly."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect_image(data, base):
    if len(data) < 64:
        raise ValueError("image must contain at least the 16 core vector words")
    if base < 0 or base > 0xFFFFFFFF or base % 4:
        raise ValueError("base must be an aligned 32-bit address")
    end = base + len(data)
    if end > 0x100000000:
        raise ValueError("image extends beyond the 32-bit address space")
    words = struct.unpack_from("<16I", data)
    reset = words[1]
    entry = reset & ~1
    return {
        "size_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "supplied_base": f"0x{base:08X}",
        "end_exclusive": f"0x{end:08X}",
        "initial_sp": f"0x{words[0]:08X}",
        "core_vectors": [f"0x{word:08X}" for word in words],
        "reset_entry": f"0x{entry:08X}",
        "reset_file_offset": entry - base if base <= entry < end else None,
        "checks": {
            "sp_nonzero_and_word_aligned": words[0] != 0 and words[0] % 4 == 0,
            "reset_thumb_bit": bool(reset & 1),
            "reset_in_supplied_image": base <= entry < end,
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--base", required=True, type=lambda value: int(value, 0),
                        help="candidate load address; not inferred or authenticated")
    args = parser.parse_args()
    try:
        report = inspect_image(args.image.read_bytes(), args.base)
    except (OSError, ValueError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")
    print(json.dumps(report, indent=2))
    return 0 if all(report["checks"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
