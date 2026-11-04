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
  answers to a send read -62 dBm like its minute answers), the per-packet log of #6, #2 (own system ID only, counter only from
  own packets), #4 (the whole frame with its CRC).
- Also done on 2026-11-03: #7 (inside temperature checked by byte 23), #9 (RC presses de-duplicated by counter),
  #10 (`down` renamed to `close`), #5 in part (see below), the command order noted at each list.
- Fixed on 2026-11-03: a send across `v5 = 00` kept the old `v7` and K, and the station ignored it (and every send
  after it until the RC resynced). Now `v7 + 2`, `K - 1`, as the RC does; tested with 140 Stop sends across a wrap.

- Done on 2026-11-04: receiving with the chip's Manchester decoder (sync `ce ad`, 36 bytes, stock cc1101 driver),
  only packets with a matching CRC. Measured against raw chips (4 h alternating): station CRC ok in 42% of RC
  minutes against 29%. Also tried: sync on a later header byte (`ba`, raw): worse than `ce` (46% against 58%).

## High

### 5. Outside temperature above 25.5 °C is unknown
The station has only a sign bit, so the decoded range is -25.6 to +25.5 °C (the sign is untested too). A CRC-ok
packet whose bytes 17, 18 fail the pair check now logs a warning with both bytes and publishes no temperature;
Raw Outside keeps updating from CRC-ok packets. To do: work out the encoding on a hot day from those warnings.

## Medium

### 6. Diagnostics for reception
- A diagnostic sensor for the station hit rate: count RC status packets with and without a station answer within
  ~50 ms. That turns "it worked better this morning" into a graph.
- Many station packets arrive but fail the CRC (at -62 dBm, so not weak): the errors are spread over the whole
  frame, and the frequency offset is small. About 40% of them have one wrong bit, which the CRC can repair: station
  CRC ok 56% of RC minutes instead of 42%, Stops 1 of 20 unconfirmed instead of 6 (1 h each, 2026-11-04). A repair
  can make a packet with 3+ wrong bits wrong (1 in 230 of those, about 2 a day), so repaired packets only confirm
  sends; the sensors stay at the 42%. The
  chip's data rate offset compensation (BS_LIMIT 6.25%) made it worse (2 h alternating).
- Not the cause of lost station answers (measured on 2026-11-03 with a timed copy of the cc1101 driver): latency
  (interrupt to loop 0.04-0.07 ms, back in RX after 2.1-2.2 ms, the same in hit and missed minutes; the station starts
  14 ms after the RC), WiFi, FOC off (worse), a full re-init after the RC packet, 15/16 sync (20 times more noise
  syncs). In a miss the station packet arrives but its start (preamble, `ce`, `ad`) is broken and the radio locks on
  later inside it (measured with raw chips).

### 8. Station Raw lacks the system ID
Add bytes 8..16 (and the counter 5..7), or a separate text sensor, so foreign or broken packets are visible.

### 13. Research: answers to our sends fail the CRC more often
In fast Stop series (raw chips), 74 of 142 station answers had a matching CRC, against about 80% of its minute
answers. Confirmation needs a matching CRC (one bit repaired at most): 20 Stops gave 14 confirmed after 1 send,
3 after 2, 2 after 3, 1 not confirmed (the station took it). Check whether our next copy (30 ms after
the first) or the RX restart after a TX clips the answer. The stock driver also keeps the RX FIFO through a send
(no SFRX), so a packet half received when we send makes the next one invalid; offer that fix upstream.

## Low

- The command list exists four times in an order that must match (now noted at each). Generate from one list?
- `validate_pairing`: check that bytes 8, 9 equal 13, 14, to catch a typo.
- Wind: one byte, so at most 25.5 m/s; what the station sends above that is unknown.
- #9 (RC presses de-duplicated by counter) is untested: press a button on the real RC and check one event per press.
- A CRC-ok packet with another `v7` always replaces the counter, so a late packet of an older block could roll it
  back (rare, heals with the RC's next resync). Accept only a newer `v7`, once it is clear how `v7` changes on a resync.
