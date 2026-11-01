#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Compare allocated ELF32 sections in selected address regions with a proposed aggregate RAM budget.

This is a capacity experiment, not a linker, address-map check or flash approval.
It includes NOLOAD/NOBITS reservations and excludes debug sections. Heap/stack
reservations already present in the ELF must not also be passed as extra reserve.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect_memory(data, ram_bytes, extra_reserve=0, *, ram_regions):
    if ram_bytes <= 0 or extra_reserve < 0:
        raise ValueError("RAM budget must be positive and extra reserve nonnegative")
    if not ram_regions or any(start < 0 or end <= start or end > 0x100000000 for start, end in ram_regions):
        raise ValueError("supply valid RAM address intervals")
    if any(a < d and c < b for i, (a, b) in enumerate(ram_regions) for c, d in ram_regions[i + 1:]):
        raise ValueError("RAM address intervals overlap")
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("expected a little-endian ELF32 file")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if header[1] != 2:
        raise ValueError("expected a linked executable, not a relocatable object")
    offset, entry_size, count, strings_index = header[6], header[11], header[12], header[13]
    if entry_size != 40 or not count or strings_index >= count:
        raise ValueError("unsupported or invalid section table")
    if offset < 52 or offset + count * entry_size > len(data):
        raise ValueError("truncated section table")
    sections = [struct.unpack_from("<10I", data, offset + i * entry_size) for i in range(count)]
    strings = sections[strings_index]
    if strings[1] != 3 or strings[4] + strings[5] > len(data):
        raise ValueError("invalid section name table")
    names = data[strings[4]:strings[4] + strings[5]]
    allocated_sections = []
    for section in sections:
        name_offset, kind, flags, address, file_offset, size = section[:6]
        if kind != 8 and file_offset + size > len(data):
            raise ValueError("truncated section contents")
        if not flags & 2 or not size:  # SHF_ALLOC
            continue
        if not any(start <= address and address + size <= stop for start, stop in ram_regions):
            if any(address < stop and start < address + size for start, stop in ram_regions):
                raise ValueError("section crosses a selected RAM boundary")
            continue
        end = names.find(b"\0", name_offset)
        if name_offset >= len(names) or end < 0:
            raise ValueError("invalid section name")
        allocated_sections.append({"name": names[name_offset:end].decode("ascii", errors="replace"),
                         "address": f"0x{address:08X}", "bytes": size,
                         "no_file_contents": kind == 8})
    allocated = sum(section["bytes"] for section in allocated_sections)
    required = allocated + extra_reserve
    return {"sha256": hashlib.sha256(data).hexdigest(), "machine": header[2],
            "ram_budget_bytes": ram_bytes, "allocated_ram_bytes": allocated,
            "extra_reserve_bytes": extra_reserve, "required_bytes": required,
            "remaining_bytes": ram_bytes - required, "fits_aggregate_budget": required <= ram_bytes,
            "sections": sorted(allocated_sections, key=lambda section: -section["bytes"]),
            "limitation": "Aggregate capacity only; no address-map, runtime peak, hardware or flashing validation."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--ram-bytes", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--ram-region", required=True, action="append",
                        help="RAM START:END addresses in the input ELF; repeat for separate banks")
    parser.add_argument("--extra-reserve", default=0, type=lambda value: int(value, 0))
    args = parser.parse_args()
    try:
        regions = [tuple(int(value, 0) for value in region.split(":")) for region in args.ram_region]
        if any(len(region) != 2 for region in regions):
            raise ValueError("each RAM region needs START:END")
        report = inspect_memory(args.elf.read_bytes(), args.ram_bytes, args.extra_reserve, ram_regions=regions)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")
    print(json.dumps(report, indent=2))
    return 0 if report["fits_aggregate_budget"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
