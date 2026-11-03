# TODO

Review of 1facf63 (2026-11-03), with measurements from the live gateway (ESPHome 2026.10.1, CC1101 driver of that
release). Ordered by priority.

## Measured on 2026-11-03

- RC status: every packet arrives (5 of 5 in a 5-minute sample), about -49 dBm since midday (it was about -36 dBm
  until 11:00 UTC: something moved).
- Station: about 90% of its minute answers arrive (141 of about 157 between 11:15 and 13:52 UTC), and 20 to 33% of
  those have at least one bad byte (`..` in Station Raw).
- 10 Stop presses, 1 s apart: all sent, 8 answers heard (6 clean, 2 with bad bytes at different positions). Same
  rates as the spontaneous reports, so the losses are reception, not the send path.

## Done

- Done on 2026-11-03: #1 (confirmed sends, Last Command sensor), #3 (fixed by the non-blocking send: station
  answers to a send read -62 dBm like its minute answers), the per-packet log of #6.

## High

### 2. Received packets are not checked against the system ID
`decode_` accepts a station packet when byte 10 (or 15) equals the station address. The system ID (bytes 8, 9 and
the copy in 13, 14) is never compared.
- A neighbouring Arexa with the same station address byte would feed its weather into these sensors.
- The counter (`v5_`, `v7_`, `k_`) is taken from **any** Arexa frame, before any sender or system check. A foreign
  packet (or an undetected bit error in byte 7, which resets the counter whenever `v7` differs) moves it, and the
  next commands carry a counter the own station ignores.
- `system: auto` pairs with the first RC status packet heard. Require two matching status packets before learning.
- Fix: drop packets whose clean bytes 8, 9 (or 13, 14) differ from the own system; take the counter only from own
  packets.

### 4. Receive the CRC: fields without a check of their own can be wrong silently
A chip pair that is inverted (`10` read as `01`) is not detected, so a byte can be wrong while marked clean. The
temperatures and wind have a check byte; `wind_alarm` (on for any byte 28 other than `27`, so one bad byte raises a
wind alarm), `rain` (21), `brightness` (24), `auto_mode` (25) and `rain_sensor` (29) don't. The frame's CRC would
catch all of it, but it never arrives: 38 frame bytes are 608 chips, the 64-byte FIFO holds 512 (frame bytes 1..32).
- Option A: read the FIFO while the packet comes in (FIFO threshold, packet length 76) and keep the software
  Manchester. Needs a change in (or a copy of) the cc1101 driver, which reads only at the end of a packet.
- Option B: let the chip decode the Manchester (38 bytes fit). The bad-byte marks are lost, so a packet with one
  broken chip is lost entirely (20-33% of the station's packets today): use the CRC-checked packet for the fields
  without a check, and keep the software path for the rest?
- Then publish `wind_alarm`, `rain`, `brightness`, `auto_mode` and `rain_sensor` only from packets whose CRC
  matches.
- Brightness has no check byte: `2 * v22 + v23` is not constant (0x25..0x38 over two hours), so 22, 23 is not a pair
  like 17, 18, and byte 24 stands alone.

### 5. Outside temperature above 25.5 °C and below 0 °C are unknown
The station has only a sign bit, so the decoded range is -25.6 to +25.5 °C. Above 25.5 °C the encoding is unknown;
if the station just wraps, a hot day would read as about -25 °C (bad for any automation using it). The sign is
untested too.
- If bytes 17 and 18 are clean but fail the pair check, log a warning with both bytes. Today the value just stops
  updating silently, and Raw Outside stops too, because it is gated by the same check.
- Optionally reject a jump of more than a few degrees from the last value until a second packet confirms it.

## Medium

### 6. Diagnostics for reception
- A diagnostic sensor for the station hit rate: count RC status packets with and without a station answer within
  ~50 ms. That turns "it worked better this morning" into a graph.
- Not the cause of lost station answers (measured on 2026-11-03 with a timed copy of the cc1101 driver): latency
  (interrupt to loop 0.04-0.07 ms, back in RX after 2.1-2.2 ms, the same in hit and missed minutes; the station starts
  14 ms after the RC), WiFi, FOC off (worse), a full re-init after the RC packet, 15/16 sync (20 times more noise
  syncs). In a miss the station packet arrives but its start (preamble, `ce`, `ad`) is broken and the radio locks on
  later inside it. The open idea is syncing on frame byte 2 (comment in `start_listening()`): test on 30 Nov, not
  before.

### 7. Inside temperature has no check
Byte 23 holds `2 * low + sign`, like byte 18 for the station. Use it as a check, like `outside_ok`.

### 8. Station Raw lacks the system ID
Add bytes 8..16 (and the counter 5..7), or a separate text sensor, so foreign or broken packets are visible.

### 9. RC event de-duplication by time
The same command within 3 s counts as one press, so two real presses within 3 s give one event. The RC repeats a
command with the same counter until answered: de-duplicate on the counter instead.

### 10. Rename `down` to `close`
The window closes; `down` probably comes from the Solexa (shade) or the RC's arrow. Add `close` only as an alias:
`down` keeps working everywhere (YAML key and event type), because existing HA automations use it.

## Low

- `decode_`: if the driver ever hands over fewer than 64 bytes, the missing bytes are zeros marked clean. Mark bytes
  past the end as bad (the driver gives 64 in fixed-length mode today, so this is only defensive).
- The command list exists four times in an order that must match: `Command`, `COMMANDS`/`COMMAND_NAMES`,
  `TYPES` in button.py, `EVENT_TYPES` in event.py. Generate from one list, or at least add a comment in each place.
- `validate_system`: check that bytes 8, 9 equal 13, 14, to catch a typo.
- Wind: one byte, so at most 25.5 m/s; what the station sends above that is unknown.
