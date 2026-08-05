v0.1: EXPERIMENTAL: Flipper Share iButton — file transfer over the 1-Wire pad
- New app derived from Flipper Share: the transport is a 1-Wire host/slave pair on the iButton pad (`gpio_ibutton`, PB14 — also GPIO header pin 17), built on the firmware `one_wire` API.
- Role mapping: the receiver drives the bus as the 1-Wire host, the sender answers as a slave (emulator); two custom commands (POLL `0xA1` / PUSH `0xA2`) carry one flipper-share packet per transaction.
- Resumable: the block bitmap picks up where the pads separated; per-packet CRC16 and a whole-file MD5 check after reception.
- Control traffic (ANNOUNCE / REQUEST) has priority over DATA in the transport mailbox, so a DATA stream cannot starve it.
- Custom command codes avoid the standard 1-Wire ROM commands, so a foreign 1-Wire reader touching the sender gets nothing.
- Standard-speed slots only; ~1.2 KB/s expected from the timing budget — not yet bench-measured.
