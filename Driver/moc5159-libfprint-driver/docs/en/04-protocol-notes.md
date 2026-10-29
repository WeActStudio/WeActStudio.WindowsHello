**English** · [简体中文](../zh-CN/04-protocol-notes.md)

# 04 · Protocol notes and the technical basis for the hardware limit

This document is for people who want to modify the driver, build on top of it, or confirm
that a limitation is not a driver bug. It records **only conclusions that have been verified**,
together with the evidence behind them. More detailed frame-by-frame captures (USBPcap sessions
of the official Windows driver, DLL disassembly notes) are not published with this repository;
you can reproduce them yourself with the method in §9.

---

## 1. Device

| Item | Value |
|---|---|
| VID:PID | `28e9:5159` |
| Name | Petaic MOC-5159 (GDMicroelectronics) |
| MCU / sensor | GD32W515 / GSL6150 |
| Type | match-on-chip (templates and matching on-chip; the host never gets feature data) |
| Endpoints | 2 bulk: `0x01` OUT, `0x81` IN (wMaxPacketSize=64). **No interrupt endpoint** |
| Frame size | 128 bytes, fixed |

## 2. Frame format (bulk OUT, 128 bytes)

```
[0]      payload length
[1]      0
[2]      subcommand code
[3..5]   0
[6..]    payload
```

- Checksum: the sum of the first 126 bytes goes into the last byte (implemented in `moc5159-proto.c`).
- **You must drain leftover IN frames before sending a command**, otherwise every following OUT
  NAKs until it times out (looks like "the device died").
- The IN URB length must be exactly 128 (replies are two full 64-byte packets with no short
  packet terminating them).
- One `SETUP` per session (control IN `0xC0/0xC0`, 1 byte, expects `0x5C`).

## 3. Command table (the part confirmed by measurement)

| Code | Name | Purpose |
|---|---|---|
| `0xC0` | VERIFY_CAPTURE | verification capture (blocking, waits for finger) |
| `0xC1` | VERIFY_MATCHING | fetch match result (120-byte payload) |
| `0xC2` | CREATE_ENROLL | start an enrollment session |
| `0xC3` | COMMIT_ENROLL | commit enrollment (carries the 80-byte record) |
| `0xC4` | ENROLL_CAPTURE | enrollment capture (blocking, waits for finger) |
| `0xC5` | ENROLL_APPLY | apply this stage's sample |
| `0xD5` | DB_SIZE | `[0:4]` = free slots, `[8:12]` = used count |
| `0xD7` | DB_WIPE | wipe the database |
| `0xD8` | DB_UPDATE_TPL | adaptive template write-back (after a successful match) |
| `0xD9` | DB_DUMP_RECORD_BY_INDEX | read record n (84-byte reply = 4 header + 80 record) |
| `0xDA` | DB_CREATE | create database (required after a wipe, otherwise `ev=0x29` forever) |
| `0xDB` | DB_OPEN | open database by DB GUID (returns `ev=0x28` if absent) |
| `0xDC` | DB_CLOSE | close database |
| `0xDE` | DB_QUERY_BY_SUBJECT | upper-layer query interface, **not used by the verification path** |
| `0xDF` | DB_DEL | delete a record (echoes the 80-byte record) |
| `0x13` | image block transfer | 2048-byte block; header offsets 2/3/6/7 must be 0, `0x01` padding only starts at offset 9 |
| `0x30` | session status word | always `0x00` on this model, **cannot be used to detect a finger** |

## 4. 80-byte record layout (confirmed by both D9 replies and C3 submissions)

```
[0:4]    template id (tid)   new template = max_tid + 1
[4:8]    template data length  normally 0x1C (28)
[8:12]   0x00000501
[12:16]  0x05000000
[16:20]  0x00000015
[20:36]  database identity GUID  same for the whole DB: a1e02ef3-46a8-1266-f2d5-bc0fe9030000
[36:76]  0
[76:80]  global monotonic counter  increases across templates (245→249 observed on Windows)
```

## 5. Real timing of the two paths (captures of the official Windows driver)

**Verification / identification**

```
D5 → D9(0..N) ×2 rounds → 0x30 → C0 → 0x13 blocks → 0x31 → D5 → C1 → 0x13 blocks → D8
```

**Enrollment**

```
… D5 → D9(0..2) → C2
  [C4 → 0x13 blocks → 0x31 → C5 → 0x13 blocks] × 6
  → C3 (80-byte record) → 0C
```

Note: `C2 / C4 / C5` **all take zero-length parameters**; only `C3` carries the record itself.
**The verification path contains no command that selects a template or sets a subject.**

## 6. Why only the first finger works

Evidence chain (each item verified separately):

1. **The Windows capture has no multiple templates either.** The 5 records in
   `verify_truth.pcapng` are `tid=3 ×4` (four samples of the same finger, cnt 245-248) plus
   `tid=4 ×1` (`[4]=3`, i.e. data length 3 — an **invalid record** written by an early
   prototype). In other words, Windows never compared two different templates on this device.
2. **Deleting the first record makes the next one work immediately.** With 2 records in the
   database, deleting index 0 makes the remaining one match right away (user-verified and
   double-checked with `probe ident`).
3. **D9 cannot select the comparison target.** Loading only the index-1 record and pressing
   with the corresponding finger still does not match; D9 payloads of 1 byte and Windows'
   16 bytes behave identically.
4. **There is no "select template" command.** We swept `0xD0`-`0xE4`: `0xDD` is an unknown
   command (`ev=0x90`), `D3/D2/D4` return `ev=0x26` for any payload (unsupported by this
   firmware), and `0xDE DB_QUERY_BY_SUBJECT` has **exactly one call site in
   `PetaicTouchFP.dll`, at `0x180007931`**, inside the top-level IOCTL dispatcher
   (`sub eax, 0x44244C` and friends, switching on IOCTL code). It exists for the Windows
   biometric engine's queries and the verification path never reaches it.
5. **You cannot re-query after C1.** In a `D5 + D9(i) + C1` loop the second C1 never answers —
   the device has already left match state — so "one press, walk the whole database" does not
   work either.
6. **One database per finger is not viable.** `0xDB` with a different DB GUID returns
   `ev=0x28` (database does not exist); a second database can only be created with `0xDA`,
   and in practice `0xDA` creating a second database **hangs the device** (needs a physical
   replug).

⇒ Conclusion: **single-template comparison is firmware behaviour**; there is no driver-side
workaround. All the driver can guarantee is that "the first record is the finger you want".

## 7. The C1 verdict (how the driver decides match / no-match)

`0xC1` replies with a 120-byte payload:

```
[0:4]     tick
[4:8]     status
[8:24]    block 1
[24:40]   block 2
[108:120] fixed 57 8D 07 08 01 00 00 00 00 00 04 00
```

Driver predicate (`moc_c1_matched()`):

```
match    : block1[8] == block2[8]  and  block1[9:16] differs from block2[9:16] (L1 = 16..28)
no match : block1[8] != block2[8]  and  block1[9:16] identical to block2[9:16] (L1 = 0)
```

The `ev` field is unrelated to the outcome (it is always `0x01` for normal recognition, and
`0xAF` appears only after enrollment self-verification). `block1[8]/block2[8]` were once
mistaken for a similarity score — they look more like some stable reading; **what is actually
discriminative is the relationship between the two blocks** (a comparison inside one capture,
not across captures).

## 8. Known unknowns / not done

| Item | Status |
|---|---|
| Full semantics of the 120-byte `C1` payload | only the part needed for the verdict is decoded |
| Correct payload format for `0xDE` | not found (`ev=0x26`), but the verification path does not need it |
| `D9` returns `ev=0x01` (not `0x05`) after enrollment | handled, does not affect matching |
| Multiple fingers | firmware limitation, unsolvable |
| `C1[7]` toggles between `0x49/0x89` | meaning unknown (only bit 7 differs), does not affect the verdict |

## 9. If you want to dig further

- The standalone `probe` tool supports: `info / db / list / dump / ident / mon / cap / ctl / raw / seq / del`
- Send several commands within one session (very useful for protocol timing work):
  ```bash
  sudo ./build/probe seq db:<DBGUID as 36 hex chars> d5 d9:00 c1
  ```
- Capture our driver's traffic (no pressing needed):
  `sudo modprobe usbmon && cat /sys/kernel/debug/usb/usbmon/3u`
- Capture real Windows traffic: run USBPcap on Windows, then inspect bulk frames in Wireshark.

⚠️ **Back up before protocol experiments**: `0xDA` (create database) and `0xD7` (wipe) are
destructive; one wrong move can erase the template database or wedge the USB channel
(requiring a physical replug).
