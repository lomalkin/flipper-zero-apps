# Flipper Share iButton — direct file transfer between Flippers over the 1-Wire pad

> **⚠️ WARNING:** Flipper Share iButton is an **experimental-only** app, it is not recommended for regular use.
> Consider using other Flipper Share transports (NFC, Sub-GHz, IR) for everyday file transfer.

## Overview

**Flipper Share iButton** transfers any file directly from one Flipper Zero to another over
the **iButton pad** (1-Wire, the contact on the top of the case) — no extra hardware,
cables, phone, computer, internet or radio needed. The receiver drives the bus as a 1-Wire
host; the sender answers as a 1-Wire slave. Just touch the two pads together.

Expected transfer speed is around **1.2 KB/s** (standard-speed 1-Wire slots, ~13.7 kbit/s
raw). This is an estimate from the timing budget, not yet a bench measurement.

Other Flipper Share transports (Sub-GHz, IR, NFC & more): [github.com/lomalkin/flipper-zero-apps](https://github.com/lomalkin/flipper-zero-apps)

Features:

- Works out of the box on any Flipper Zero — the iButton pad is built in.
- Integrity check with an MD5 hash after reception; per-packet CRC16.
- Resumes automatically: separate and re-touch the pads mid-transfer and the receiver's
  block bitmap picks up where it left off.
- Torrent-like progress bar on the receiver; filename/size and ETA on the sender.
- Works over a jumper wire too (pin 17 ↔ pin 17 plus GND ↔ GND — the same net as the pad),
  which is the reliable option if pad-to-pad contact is fiddly.

Contact bounce at touch time is expected and harmless — every transaction starts with a
1-Wire reset/presence pulse, so the link resynchronizes by itself. Received files are saved
to **/ext/inbox/**.

# Notes

See the full [README.md](https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_ibutton/README.md) for the 1-Wire transport and protocol description.

Source code of the latest version is [here](https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_ibutton). Please feel free to open issues and PRs.

# Credits

Derived from Flipper Share. The 1-Wire transport is built on the Flipper firmware
`one_wire` host/slave API, all through the official external app API.
