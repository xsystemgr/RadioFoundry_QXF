
## Audio-silence experiment log (2026-11-02)

Hardware observations (owner, RT-880, 446.5 MHz UHF):
- RX: squelch opens on signal, RSSI -131 (floor) -> -89 (near signal), noise 99 -> ~18. Demod/squelch WORKS. Absolute RSSI ~30 dB low.
- Audio: internal tone test (AF 0x6240) = completely silent. Real RX = very faint background, not intelligible voice.

Single-variable experiments (receiver = BK4819 on bus A), each flashed & tested:
| # | Change | On-screen confirm | Result |
|---|--------|-------------------|--------|
| reg37 | 0x37 1D0F -> 1F0F (RF LDO bit9) after analog_mode | `37:1F0F` shown | NO change to audio or RSSI. Reverted. |
| ampsweep-ALL | drive 21 unassigned GPIO descriptors HIGH during tone | `P:21` | NO beep. Not a simple active-high amp-enable among them. |
| af2B | 0x2B C000 -> 0000 (enable AF HPF/LPF/de-emph) after analog_mode | n/a | NO change (still silent/faint). Reverted. |
| phys-knob | turn physical volume pot fully up (no flash) | n/a | NO change in loudness. |

Ruled out: reg37 LDO bit, 21 candidate active-high GPIO enables, AF filter bypass, physical volume pot.
Signature (dead internal tone + faint-not-voice RX) points to an unset audio MUX/routing after the BK4819 AF output -- likely an I2C step (Si47xx-class tuner on PC0/PC1 @0x11) or analog switch that nicFW static analysis never located.

Next: analyze the FACTORY ("mama") firmware (GPIO descriptors + I2C writes around audio-open) to find the amp-enable + AF routing sequence. nicFW analysis was insufficient for the audio path.

### Factory firmware analysis (firmware_UV98_1.05.bin, 2026-11-02)
- Image = APP-ONLY update: real content 0x08002800-0x08010000 (~55 KB), clean cut, rest zero-padded to 0x08040000.
- App branches into the zero region 741x (135 distinct targets) -> factory driver/library at 0x08010000+ is NOT in this file (hottest: 0x08014D24 x157, called with BK4819 reg args 0x30/0x33).
- CONFIRMED: PC4 (GPIOC bit4, mask 0x10) is the audio mute line. Mute routine @0x08007CE0:
    flag[0x20002C78+0x107]!=0 -> GPIOC_CLR 0x10 (PC4 low) + bl 0x08015594(4)
    else                      -> GPIOC_SCR 0x10 (PC4 high) + bl 0x08014964(4)
    flag[+0x77]==0            -> GPIOC_CLR 0x10 (PC4 low) @0x08007D36
  The two bl helpers (0x08015594 / 0x08014964, arg 4) do the real audio enable/routing and live in the MISSING library section -> cannot read.
- Port already drives PC4; the missing step is those helpers (likely an I2C write to Si47xx/audio mux, since the 21-GPIO active-high sweep produced no audio).
- NEED: full SWD read of AT32F423 internal flash (0x08000000, 256 KB) to recover 0x08010000+ library incl. the audio helpers.

### BREAKTHROUGH from RT-880 V1.16 upgrade image (full code present, 2026-11-02)
- RT-880_Upgrade_V1_16 is a COMPLETE image (content to 0x0802EF8F), unlike the UV98 app-only file. Library + helpers all present.
- Mute routine @0x0800B0EA: PC4 toggle + bl 0x801FC70. 0x801FC70 = sets software audio-vote word *(u16*)0x20001204 |= arg|0x8000 (ref-counted gate). Applier @0x0801F084: when word==0x8000 (no votes) -> clear PF8 (GPIOF bit8) = speaker gate OFF. So PC4=mute, PF8=speaker gate -- both already driven by the port. GPIO audio-enable is NOT the gap.
- MCU DAC (0x40007410) is used by factory (beeps/prompts).
- KEY: factory BK4819 bit-bang write = 0x0801E660 (x35 calls), byte-send 0x0801D8E8. Pins: CLK=PF6, CS=PA8, DATA=PC9  ==> that is bus_b (RF path B). Factory runs its ENTIRE main BK4819 setup (incl. AF regs 0x47/0x48) on CHIP B, not chip A.
- Our port received on chip A (bus_a, PC6/7/8) and halted B. Audio amp is fed by chip B's AF output -> chip A gave only faint bleed; local tone on A was dead.
- EXPERIMENT rxchipB: attach receiver=bus_b, control=bus_a; band-select reg 0x33 written on receiver(B). Build rfoundry_rt880_rxchipB_UNTESTED.bin. Awaiting hardware result (audio? RSSI on B?).

**RESULT: CONFIRMED WORKING (owner, 2026-11-02 21:13).** Receiving on chip B (bus_b) restored speaker audio. Root cause = wrong BK4819 chip; amp is fed by chip B AF. rxchipB change is the fix.
