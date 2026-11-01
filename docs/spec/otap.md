<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Fleet administration over DMR (OTAP / OTAR)

`CONFIG_OTAP` (on where a build has both DMR voice privacy and the DMR data core) lets one owner manage a group of
their own radios over the air: read a radio's state, change a few of its settings, and change its privacy key. It is
the fleet-management model of commercial OTAP (over-the-air programming) and OTAR (over-the-air rekeying): every radio
in the fleet holds the same 32-octet **shared secret**, loaded into each one over USB and never sent on the air. A
radio obeys a command only when the command is signed with that secret, so a radio answers its own fleet and no one
else. A radio that holds a different secret, or none, ignores the command. This is not a way to reach a radio one has
not provisioned.

It is deliberately small: it changes individual channels and settings (tens of octets each), not a whole codeplug
(a codeplug is ~700 KB, over an hour on the air at the data rate). For bulk changes, use the USB CPS or zone sharing
([zone-share.md](zone-share.md)).

| Section | Topic |
|---|---|
| 1 | What it can do, and the policy that gates it |
| 2 | The shared secret and the derived keys |
| 3 | The message format, the signature and the key encryption |
| 4 | The service: receiving, replay protection, answering |
| 5 | The settings that back it |
| 6 | Provisioning and the PC console over the debug link |
| 7 | Legality |

## 1. Commands and policy

A command is one of:

| Command | Payload | Effect |
|---|---|---|
| `STATUS` | none | the radio answers with its state (1.1) |
| `TALK_TG` | u24 talkgroup | the talk target for now (0 = the channel's own) |
| `RX_TG` | u24 talkgroup | a talkgroup heard besides the talk target (0 = none) |
| `ZONE_POS` | u16 position | switch to that channel in the current zone |
| `PRIVACY` | u8 (1/0) | the stored privacy key on / off |
| `SET_KEY` | type u8, key u16, activate u8 | change the stored privacy key (OTAR); the payload is encrypted (3) |

Each radio has a **policy** (its `otap_flags`, set over USB) that says what a signed command may do to it, beyond the
signature and replay checks:

- `STATUS` is always answered once a secret is set.
- setting changes (`TALK_TG`, `RX_TG`, `ZONE_POS`, `PRIVACY`) need `OTAP_ALLOW_SETTINGS`.
- key changes (`SET_KEY`, OTAR) need `OTAP_ALLOW_KEYS`, and are taken only when the radio's transmit frequency is
  inside one of its privacy bands (the fleet uses encryption only where it is allowed; [dmr-voice.md](dmr-voice.md)).

A radio with neither flag set answers `STATUS` and refuses the rest (`OTAP_R_REFUSED`). So a fleet can leave key
changes off everywhere, and the owner turns them on only where and when they are wanted. The **key change can be
switched off** at any time by clearing `OTAP_ALLOW_KEYS` over USB (or by a factory reset, which clears the secret).

Optionally, up to two **admin DMR IDs** are listed; then only commands from those senders are obeyed. With no admin
listed, any sender who holds the secret is obeyed (the signature is the gate).

### 1.1 Status reply

After the result octet, `STATUS` returns (big endian): a flags octet (`OTAP_ST_*`: DMR, VFO, a key loaded, calls go
out encrypted here, OTAR allowed, settings allowed), the RX and TX frequency (Hz), the talk target and RX talkgroup
(u24 each), the battery (mV), the channel number (u16) and the privacy type.

## 2. The shared secret and the derived keys

The fleet secret is 32 octets. From it:

- `Ka = HMAC-SHA256(secret, "RF-OTAP auth")` signs and checks messages.
- `Ke = HMAC-SHA256(secret, "RF-OTAP enc")` encrypts the new key in `SET_KEY`.

The secret is set over USB and is never read back or sent on the air. All zeros means "no secret": the service is off.

## 3. Message format

A command is a UDP datagram on port `OTAP_PORT` (4101), to one radio:

```
0  2  magic "RA"              8  3  the target's DMR ID        15   n  payload (n <= 28)
2  1  version (1)            11  3  the sender's DMR ID        15+n 16 tag
3  1  command (bit 7: reply)
4  4  counter (big endian)   14  1  n
```

The **tag** is `HMAC-SHA256(Ka, octets 0..15+n)[0..16)`: it covers the whole message but the tag, so any change to any
octet — the command, the counter, the IDs or the payload — makes it fail. A reply is the command with bit 7 set, its
payload beginning with an `OTAP_R_*` result; it carries the request's counter so the sender can match it.

For `SET_KEY`, the payload is XORed with `HMAC(Ke, octets 0..15)` before the tag is taken, so the new key never
appears in the clear on the air. The receiver removes the tag first, then decrypts.

Everything decoded comes off the air from anyone: the version, the length `n` and the total length are checked, and a
message whose tag fails is dropped.

`core/dmr/otap.c` is pure encode/decode/verify and is covered by `tests/host/otapTest.c`; the Python side
(`tools/rfoundry_debug.py`) produces byte-identical messages, cross-checked against it.

## 4. The service

`core/dmr/otapSvc.c` sits between the data service and the radio core (`otap_api.h`).

- The data service hands a datagram on `OTAP_PORT` to `otap_rx`. It checks: ours, a secret set, the tag good, the
  sender an allowed admin, and the **counter above every one taken before**. Only then does it become the pending
  request. A replay of a captured command is refused (its counter is not above the last taken).
- The radio core's data tick takes the pending request (`otap_take`), runs it (`otap_run_pending` in `radio_sm.c`
  applies the policy and the privacy-band rule), and answers (`otap_answer`). The answer is signed and goes out from
  the tick when the channel is free.
- The accepted counter is saved to the settings (`otap_counter`), so a replay cannot win even across a reboot.

`tests/host/otapSvcTest.c` covers accepting a command, answering, advancing the counter, and dropping a bad tag, the
wrong sender, a replay and a command with no secret set.

## 5. Settings

Backed in `settings_t` (page 2, [settings-store.md](settings-store.md) 3): `otap_secret[32]` (all zeros = off),
`otap_counter` (the highest request counter taken), `otap_admin[2]` (allowed sender IDs, 0 = unused) and `otap_flags`
(`SETTINGS_OTAP_SETTINGS`, `SETTINGS_OTAP_KEYS`). A record written by firmware from before reads the secret as zeros,
so the service stays off until the user provisions it. A factory reset clears all of it (the privacy keys with it).

## 6. Provisioning and the PC console

Over the debug link (`DL_CMD_OTAP`, [debuglink.md](debuglink.md) 3.23):

- **Provision** this radio: set its secret (`rfoundry_debug.py otap provision --secret HEX` or `--random`), its policy
  flags (`--settings`, `--keys`) and optional admin IDs. Do this for every radio in the fleet, with the same secret.
- **Console**: the PC holds the secret and builds and signs the commands itself; the radio on USB only relays them
  (`otap send ...` and the replies). So the admin radio need not itself be provisioned to drive the fleet, and the
  secret on the PC is only ever used to sign — it is not stored on the radio in readable form.
- **Group commands**: a command can be sent to a talkgroup instead of one radio (`SEND_GROUP`), so every fleet radio
  on that talkgroup takes it in one unconfirmed transmission. Group data carries no acknowledgement and each radio
  would otherwise answer at once, so a group command is **run but not answered**; confirm it with the counters (4) on
  each radio, or by the effect. The retransmission (each message goes out a few times) covers a radio that missed the
  first burst.
- **Rekeying the fleet**: the bench GUI's "new privacy key" makes a fresh key, saves it on the connected (admin) radio
  over USB (`LOCAL_KEY`, which writes the same setting the OTAR receive path does, so it persists), and sends it to the
  others with `SET_KEY` — to one radio, or to a talkgroup for the whole fleet at once.

The bench GUI (`rfoundry_bench_gui.py`, the "Στόλος (OTAP/OTAR)" tab) wraps both: a secret field with a "random"
button, provisioning with the policy toggles (including the **key-change on/off** switch), and console buttons for
status, the talkgroup and zone changes, privacy on/off and the key change.

## 7. Legality

On amateur bands, encrypting or obscuring the content of a transmission is not allowed, so a fleet on those bands
leaves the key change off and keeps the privacy bands empty; the authenticated commands themselves are in the clear
(the signature only proves who sent them, it does not hide anything). Encryption, and so OTAR, belong on licensed PMR
or business frequencies. The radio enforces the band rule for key use (1); the operator is responsible for staying
within their licence.
