// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef AT32F423_RESET_POLICY_H
#define AT32F423_RESET_POLICY_H
#include <stdint.h>
#include <stdbool.h>
// ES0010: PC13 can remain high-impedance after a slow power-on ramp. A
// software reset after POR fixes it; the software-reset flag prevents a loop.
static inline bool at32_pc13_needs_reset(uint32_t flags)
{
	return (flags & (1u << 27)) != 0 && (flags & (1u << 28)) == 0;
}
#endif
