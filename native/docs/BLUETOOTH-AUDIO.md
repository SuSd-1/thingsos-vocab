# Bluetooth audio candidate (2026-10-27, version 0.3.6)

Implemented against the approved prototype: Settings entry, discovery,
pair/connect confirmation, saved devices, optional startup reconnect, disconnect
and forget. Unexpected loss pauses playback without speaker fallback.
The external BH1098G supplies Bluetooth, not an ESP-IDF on-chip Bluetooth stack.

## Source and ownership

Protocol and wiring reference: Metalio MIT source at
`b400605e9a6a276a2c39864c0cff63798c78e8d8`,
`main/display/screen/bluetooth_screen/bluetooth_uart.cc` and
`bluetooth_screen.cc`. Reference checkout unchanged. Parser, state machine,
UI and record storage are independently authored.

A single worker owns UART RX while Bluetooth is enabled. The main task owns
UI/display work. UART uses TX39/RX38; existing I2S uses 42/43/44/45. Mode changes
require exact acknowledgements and bounded timeouts. Connection success must
correlate to a requested device; device names cannot masquerade as status lines.
Scan results are bounded to 16 devices and saved records to four.

NVS namespace `thingsos-bt` stores a versioned CRC32 record with flags, saved
addresses/names and the last device. Records save only on confirmed connection.
Failed storage does not claim a durable record. Forget removes the host record;
it does not claim to erase undocumented module-owned bonding keys.

Route changes suspend I2S before switching the module. Software gates prevent
speaker output while Bluetooth is enabled or a route command is queued. Recording
requires Bluetooth off. Acknowledgement failure keeps the output gate closed.

## Validation boundary

Host tests execute the production parser and service with deterministic UART,
NVS and audio adapters: framing, injection rejection, bounds, mode timeouts,
correlated success, save/reload/CRC failure, storage failure and route gating.
Native UI tests exercise discovery, pairing, saved records, forget and switches.
All 32 host suites passed. These checks do not establish radio pairing or audio.

The intended COM5 / serial `1C:29:04:D0:4A:B0` was absent during integration.
COM4 / serial `14:C1:9F:D1:80:F0` is a different device and was not flashed.
Pending on the intended device: real headset discovery/pairing, BH1098G mode-2
PCM/I2S output, MP3/WAV playback, volume, disconnect pause, reboot persistence and
startup reconnect. This is a development candidate, not a hardware-accepted release.
