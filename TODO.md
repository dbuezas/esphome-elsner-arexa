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

## High

### 1. Confirm every command, retry, and fail loudly
Open, Stop and Down are sent once (3 blind copies, same counter). Auto and Manual are resent up to 4 times until an
answer shows the new mode. About 1 answer in 5 is not heard.
- Retry all commands: new counter each time, every ~300 ms, up to 4 sends, until a station answer **with the counter
  just sent** arrives. Check first (log `v5` of the answers) that the answer really carries that counter. My SDR
  logs say it does (station `7c` after RC `7c`); confirm it on the gateway.
- Mode commands keep their extra condition (the answer must show the new mode).
- Report the result: `ESP_LOGW` on failure, plus a `last_command` text sensor (`open: confirmed`,
  `open: no answer after 4 sends`, `open: not sent, RC not heard yet`) or an event, so an HA automation can alert.
  A button cannot return an error to HA.
- The send is blocking today: 3 copies with `delay(30)` between them (~80 ms in the main loop). Make it a
  non-blocking state machine (send one copy, wait for the answer in `loop()`, resend), which also fixes #3.

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

### 3. RC/Station Signal and Frequency Offset are wrong around commands
During the Stop test the station answers read -77 to -96 dBm (its minute answers: about -60 dBm), and history shows
station offsets of -50 to -68 kHz around 11:00 UTC (probably the tests from then). Likely cause: the driver reads the
RSSI and FREQEST registers when its `loop()` runs, not from the packet. While `send_frame_` blocks, the answer to
copy 1 or 2 waits in the FIFO, the radio transmits the next copy and re-enters RX, and the registers then hold the
noise floor. (Appended status bytes are not an option: the 64-byte packet fills the FIFO.)
- Don't publish RSSI or offset for a packet that was read after a transmit, or (better) fix it with the non-blocking
  send from #1.

### 4. Fields without a check of their own can be wrong silently
There is no CRC (it is past the 64-byte FIFO). A chip pair that is inverted (`10` read as `01`) is not detected, so a
byte can be wrong while marked clean. The temperatures and wind have a check byte; these don't:
- `wind_alarm`: on for **any** value of byte 28 other than `27`, so one bad byte raises a wind alarm.
- `rain` (byte 21), `brightness` (24), `auto_mode` (25, only `b3`/`bf` accepted, which helps), `rain_sensor` (29).
- For the binary sensors, change state only after two packets in a row agree (they arrive every minute, and the
  station holds an alarm for minutes anyway).
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
- A per-packet debug log line: sender, counter, RSSI, bad byte positions. Today only startup, sends and RC presses
  are logged, so the log shows nothing about reception.
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
- The auto mode switch gives no feedback when the send fails (not learned yet): covered by #1's status.
- README: describe the 3 copies per send, the retries, and what the status sensor reports.
