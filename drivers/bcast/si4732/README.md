<!-- SPDX-License-Identifier: MIT -->
# Si4732 HF/broadcast receiver driver

Drives an Si4732/35-D60 as the board's `hal_bcast_t`: AM (LW/MW/SW) and, with the SSB patch, LSB/USB on HF, plus
FM broadcast. On the RT-880 it is the HF second receiver (VFO tuned into its range plays on it; receive only).

## SSB patch (not in this repository)

SSB needs a ~15.8 KB patch uploaded to the chip's RAM at every SSB power-up. Those bytes are **third-party**
(Silicon Labs, published by Vadim Afonkin; adapted to a C array by the PU2CLR SI4735 Arduino library) and are
**personal / non-commercial** use only. They are kept in `si4732_ssb_patch.h`; do not reuse them commercially or
redistribute them outside a personal fork.

If you need to recreate `si4732_ssb_patch.h` (`CONFIG_SI4732_SSB=1`, set by the RT-880 `HF=1` build), define:

```c
static const uint8_t si4732_ssb_patch[] = { /* 8 bytes per line ... */ };
#define SI4732_SSB_PATCH_SIZE ((uint16_t)sizeof si4732_ssb_patch)
```

from `patch_full.h` of the PU2CLR SI4735 library (file `amrx_6_0_1_ssbrx_patch_full_0x9D29.csg`).

Without the file the driver still compiles — SSB is disabled and HF is received in AM only (the build prints a
`#warning`).
