# Flipper Share iButton — direct file transfer between Flippers over the 1-Wire pad

> **⚠️ WARNING:** Flipper Share iButton is an **experimental-only** app, it is not recommended for regular use.
> Consider using other Flipper Share transports (NFC, Sub-GHz, IR) for everyday file transfer.

## Overview

**Flipper Share iButton** transfers any file directly from one Flipper Zero to another over
the **iButton pad** (1-Wire, the contact on the top of the case) — no extra hardware,
cables, phone, computer, internet or radio needed. Just touch the two pads together.

It is a rewrite of Flipper Share with the transport replaced by a 1-Wire host/slave pair.
The basics of the classic flipper_share file-transfer protocol (resumable,
integrity-checked) are preserved.

Expected transfer speed is around **1.2 KB/s** — standard-speed 1-Wire slots are
~13.7 kbit/s raw, and the link layer spends most of the bus time on the payload. This is
an estimate from the timing budget, not a bench measurement (see *Status* below).

Other Flipper Share transports (Sub-GHz, IR, NFC & more): [github.com/lomalkin/flipper-zero-apps](https://github.com/lomalkin/flipper-zero-apps)

Features:

- Works out of the box on any Flipper Zero — the iButton pad is built in. Builds with
  `ufbt` against the official firmware; no firmware modification.
- Integrity check with an MD5 hash after reception; per-packet CRC16.
- Automatic retransmission of lost/corrupted packets — the transfer continues "until
  success". Separating and re-touching the pads mid-transfer resumes where it left off.
- Half-duplex command/response link: the receiver drives the bus (1-Wire host), the
  sender answers as a 1-Wire slave (emulator).
- Torrent-like progress bar on the receiver; filename/size and ETA on the sender.
- Works over a jumper wire too (pin 17 ↔ pin 17 plus GND ↔ GND — the same PB14 net as
  the pad), which is the reliable option if pad-to-pad contact is fiddly.

# Usage

1. On the receiving Flipper: open Flipper Share iButton → **Receive via iButton**.
2. On the sending Flipper: open Flipper Share iButton → **Send via iButton** → pick a
   file → **OK**.
3. Touch the two iButton pads together (or wire pin 17 ↔ pin 17 and GND ↔ GND) and hold
   until it completes. The receiver shows a progress bar and verifies the MD5 hash at the
   end; the file is saved to `/ext/inbox/`.

The sender shows the file name, size and a rough ETA. The receiver shows
"Touch iButton pads / Waiting for announce..." until it locks, then the progress bar with
percentage and ETA.

Contact bounce at touch time is expected and harmless — every transaction starts with a
1-Wire reset/presence pulse, so the link resynchronizes by itself.

---

# Flipper Share iButton protocol

Two layers: a **1-Wire transport** (physical/link layer) under the existing
**file-transfer protocol** (selective-repeat ARQ). The file-transfer protocol is identical
to the other Flipper Share builds; only the transport differs.

## Physical / link layer — the 1-Wire transport

- **Bus:** standard-speed 1-Wire on `gpio_ibutton` (PB14 — the iButton pad, also GPIO
  header pin 17). One slot ≈ 73 µs/bit → ~0.6 ms/byte. Overdrive mode is not used: the
  slave side is a software bit-banger in a critical section, and overdrive between two
  Flippers is timing-marginal.
- **Role mapping:** the **receiver** is the 1-Wire **host** — it drives the bus and owns
  all timing; the **sender** is the 1-Wire **slave** (emulator) and answers in read slots.
  This matches the other transports, where the sender is the passive side (NFC listener,
  RFID tag).
- **Transactions:** the host drives every exchange as
  `reset → presence → command byte → payload`, using two custom commands:

  | Command | Direction after command byte | Payload |
  |---|---|---|
  | `IBTN_TP_CMD_POLL` `0xA1` | slave → host | `len(1)` + `len` packet bytes; `len = 0x00` means "nothing queued" |
  | `IBTN_TP_CMD_PUSH` `0xA2` | host → slave | `len(1)` + `len` packet bytes |

  `len` must be `1..FSH_PACKET_MAX`; anything else aborts the transaction on both sides
  and the next reset resynchronizes. The command codes deliberately avoid the standard
  1-Wire ROM commands (`0x33` READ ROM, `0xCC` SKIP ROM, `0xF0` SEARCH ROM, …), so a
  foreign 1-Wire reader touching the sender gets nothing. There is no ROM search or
  addressing — this is a point-to-point link with exactly two devices.
- **No link-layer CRC:** integrity is the packet's own CRC16 (checked by the engine) plus
  the whole-file MD5. A corrupted transaction either fails in the 1-Wire driver or is
  dropped on CRC16 — both silent, and the ARQ re-requests.
- **Outbound mailbox:** the engine's `fsh_transport_send` enqueues packets into a 4-deep
  queue for DATA, plus a single latest-wins slot for control packets (ANNOUNCE / REQUEST)
  that is drained first, so a DATA stream can never starve control traffic. A full data
  queue blocks the sender for up to `IBTN_TP_SEND_TIMEOUT_MS`, which is the natural
  backpressure that paces the engine.
- **Resume:** if the pads separate, the host simply sees no presence pulse and retries
  every `IBTN_TP_RECONNECT_MS`. On re-touch, the receiver's block bitmap re-requests only
  the missing blocks, so the transfer continues where it stopped.
- **ISR discipline:** the slave's command/reset callbacks run in the GPIO EXTI interrupt
  inside a `FURI_CRITICAL` section, so those paths use only zero-timeout
  `furi_message_queue_put/get` (which route to the `*FromISR` variants) and a
  `FURI_CRITICAL`-guarded control slot — no mutexes, no storage I/O, nothing blocking.
  Received frames are handed to a worker thread, which calls `fsh_receive_callback` in
  thread context, exactly as the engine expects.

## Timing budget

One DATA transaction is reset+presence (~1 ms) + command (1 B) + length (1 B) + packet
(73 B) ≈ 45 ms, plus the `IBTN_TP_POLL_INTERVAL_MS` gap → ~19 packets/s × 64 payload bytes
≈ **1.2 KB/s**.

The slave services each transaction inside interrupt/critical context (~45 ms per DATA
frame), which is this transport's main systemic risk. `FSH_DATA_LENGTH` is therefore kept
at 64 until a bench run confirms the UI, input and BT stack stay healthy during a long
transfer; raising it to 128 is a pure config change afterwards.

## Packet structure

Every packet: `[version(1)][tx_id(1)][packet_type(1)][payload][crc16(2)]`. The payload
length depends on the type.

### `0x01` — Announce (control payload)

| Field       | Size     | Type                  |
|-------------|----------|-----------------------|
| `file_name` | 36 bytes | char[36], zero-padded |
| `file_size` | 4 bytes  | uint32_t              |
| `file_hash` | 16 bytes | MD5                   |

### `0x02` — Request range (control payload)

| Field     | Size    | Type     |
|-----------|---------|----------|
| `start`   | 4 bytes | uint32_t |
| `end`     | 4 bytes | uint32_t |
| padding   | rest    | zero     |

### `0x03` — Data (data payload)

| Field        | Size            | Type     |
|--------------|-----------------|----------|
| `block_num`  | 4 bytes         | uint32_t |
| `block_data` | FSH_DATA_LENGTH | raw data |

## Session

- **Sender** announces the file (name, size, MD5) until a receiver locks on, then serves
  the requested DATA blocks in its POLL responses.
- **Receiver** locks to the first valid announce (`tx_id`), preallocates the file, and
  re-requests the missing block range on timeout. It writes each block once (duplicates
  ignored) and, when all blocks are in, computes the MD5 and compares it to the announced
  hash.
- Lost or corrupted packets are simply re-requested, so the transfer converges.

## Files

- `share.c` / `share.h` — shared file-transfer engine (byte-identical across the new
  Flipper Share apps).
- `ibutton_transport.c/.h` — 1-Wire glue: the host worker loop, the slave callbacks, the
  outbound mailbox and the RX worker.
- `share_config.h` — all tunables (bus pin, command codes, poll/reconnect timings, packet
  size, throughput estimate).
- `md5_hash.c/.h` — MD5 for the integrity check.
- `share_app.c/.h`, `scenes/share_scene_*.c` — app shell and the five UI scenes.

## Status

- Builds warning-clean against the official firmware SDK (API 88.2) and passes `APPCHK`,
  so all imports resolve on unmodified official firmware. The 1-Wire host/slave symbols
  and `gpio_ibutton` are all exported to FAPs.
- **Not yet bench-tested on two devices.** `FSH_PAYLOAD_THROUGHPUT_BPS` is the `1200`
  estimate from the timing budget above, not a measured value; the ETA shown in the UI is
  only as good as that constant. Replace it (and the numbers in this README) once a real
  transfer has been timed.
- If pad-to-pad presence detection turns out unreliable on the bench (pull-up topology),
  `IBTN_TP_GPIO` can be switched to `&gpio_ext_pa7` with an explicit wire — same code path,
  pure config change.

## Non-goals / v2 ideas

Overdrive slots (~8× throughput, needs bench proof that two software-timed sides hold the
tolerances); `FSH_DATA_LENGTH` 128 after load testing; a DS1996-emulation compatibility
mode (reading a whole 8 KB virtual iButton per chunk — zero custom PHY, but a clunkier
session model).

# Credits

Derived from Flipper Share. The 1-Wire transport is built on the Flipper firmware
`one_wire` host/slave API, all through the official external app API.
