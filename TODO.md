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

- Done on 2026-11-07 (later): the running sum for all values (rain no longer breaks the brightness), the counter
  as 16 bits with a per-system check byte (learned, or the 10th pairing byte), the station's rule (a newer counter,
  mod 2^16) explains which counters it takes.
- Done on 2026-11-07: the window text sensor (README: Window state), with the RC's automatic settings from the
  manual in the YAML. Untested on air: an alarm or the automatic mode moving the window (only the estimate seen).
- Done on 2026-11-07: #8 (the Last Frame text sensor has every whole frame, system ID and counter included).
- Done on 2026-11-07: the resync (README, Resync): decoded, tested on air, and used when there is no counter or no
  answer. Only the station's packets change the counter block (the RC repeats its stale status before it
  resyncs). The pairing check for typos. Station bytes 22, 23 (the brightness's low byte), 26 (a check byte of the
  mode) and the wind below 0 °C (byte 19 also holds the temperature's high part) decoded.

- Done on 2026-11-19: the wind alarm is byte 27 (`b2` calm, `a9` for about 5 minutes after a gust; 18 times in
  Nov 11..19, independent of the rain alarm), not byte 28; seen live on 2026-11-23. Bytes 28, 29 never changed
  (the `rain_sensor` still publishes 29). Below 0 °C seen (-1.5 °C), matching Tempelhof.

- Done on 2026-11-26: the window as a cover with a timed position (README: Window position), replacing
  `travel_time`. Closing tested (100 to 0 % in 1 + 19 s); a position in between not yet tested on air.
- Done on 2026-11-26: sends confirmed again (the fixed gain right after my own packet lost every answer since
  2026-11-08; now the AGC while sending). #9 tested: a Stop on the RC, sent 6 times, gave one event.

- Done on 2026-11-04: receiving with the chip's Manchester decoder (sync `ce ad`, 36 bytes, stock cc1101 driver),
  only packets with a matching CRC. Measured against raw chips (4 h alternating): station CRC ok in 42% of RC
  minutes against 29%. Also tried: sync on a later header byte (`ba`, raw): worse than `ce` (46% against 58%).

## High

### 5. Outside temperature above 25.5 °C is not seen yet
With the running sum (README: Values) the temperature is a plain 16-bit value, so above 25.5 °C should just work;
below 0 °C it is seen and works. To do: check on a hot day.

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

### 13. Station packets with wrong bits: solved (the AGC)
About 40% of the station's packets came with wrong bits, at -64 dBm, 4 m away; the RC's never. Not the cause (each
tested 4 to 12 hours alternating, 2026-11-04..06): ESP activity and WiFi (even WiFi off), the two 433 MHz radios
(idle), background noise, centre frequency (+-5 kHz), deviation (34.9 to 47.6 kHz), bit-sync gains, TI's AGC
values. The clues: the RC hears every station answer (it repeats its packet otherwise: once in 727), the 433.30
radio tuned to 868 MHz got 175 of 177 answers clean at -84 dBm, and the station's answers to my own sends were 85%
clean against 51% after the RC's packets. The AGC: frozen on the sync word, the gain was still low from the RC's
packet (16 dB stronger) when the station answered 8 ms later. With the gain fixed: 6% of the station's packets with
wrong bits against 43%, 93% usable against 44% (4 h alternating). The stock driver also keeps the RX FIFO through a
send (no SFRX), so a packet half received when we send makes the next one invalid; offer that fix upstream.
Later (2026-11-08): the fixed gain did not always settle at the same level (the RC's RSSI jumped between -61, -56
and -52 dBm), and the higher levels broke the station's packets (22%, 62%, 90% with wrong bits). Fixing the gain
again every 5 min made it worse (81%). With the DVGA's 3 highest steps left out it settles the same every time: 87%
of the station's packets usable against 46%, 5 lost against 46 (6 h alternating; 2 steps 83%; LNA limits and 1
step no better).
How many steps depends on the distance, so the component now calibrates it (counts the station's answers, compares
other levels now and then). Letting the AGC set the gain on each station packet and holding it failed (1 of 6
usable): the 13-chip preamble is too short for the AGC. The AGC running all the time failed too (half broken). Even
with a limit, 1 ms was not always enough for the gain to climb to it (RC RSSI -67/-57/-54 from one fixing to the
next); with 5 ms it lands on the limit every time (25 of 26), 3 steps: 93% usable.
With the gain settled, the receive settings again (2026-11-09, 10 h alternating, ~140 station packets each):
deviation 41.3 kHz with filter 232 kHz gave 83% clean, 94% usable, against 76% and 91% at 47.6/270 (each alone no
better; 325 kHz and +-5 kHz worse), so that is the default now. Rebuilt from neighbouring clean packets, most
broken ones have 1 or 2 wrong bits; a few lose the bit timing (50+ wrong bits) or have a short burst (9-17).

### 14. The station's packets the 868 module misses (2026-11-26, against an RTL-SDR)
An RTL-SDR (farther away, a plain monopole) decoded 100% of the RC's, the station's and my packets; the gateway got
44-61% of the station's (most missed entirely, the rest with 2-5 wrong bits spread over the packet), the RC's 85-95%.
The station's signal is the cleanest of the three (tones +-46 kHz, 4-5 kHz jitter, +5 kHz from my TX, chip rate
+0.3%). Not the cause (each measured against the SDR, alternating per send or minute): ESP latency (the driver reads
0.1 ms after GDO0), the receiver's restart after a packet (staying in RX the same), the Wi-Fi updates in the reply
gap (handling packets 30 ms later the same), TI's 100 kBaud demodulator and AGC values, FOC gated or off, FREND1,
FSCAL/TEST0, deviation 47.6 kHz, other gain caps. Helped: TEST2/TEST1 0x81/0x35 (now the default), 61% against 44%.
Polling the chip's status during a packet breaks the packet (SPI activity), so no probing that way.
Strongest lead: on 2026-11-05 the 433.30 module tuned to 868 MHz got 175 of 177 station answers clean at -84 dBm,
the 868 module 41%, same board and code. So the 868 module, its antenna or its place on the board. To do: try the
module or the antenna swapped, or the SDR antenna right at the gateway (does it still get them all there?).

## Low

- The command list exists four times in an order that must match (now noted at each). Generate from one list?
- Wind: a 16-bit value like the temperature (README: Values), so above 25.5 m/s should just work; not seen yet.
- The scrambler's 31 masks (README: Scrambling) follow a shift pattern but no short recurrence I found; a generator
  would replace the table.
