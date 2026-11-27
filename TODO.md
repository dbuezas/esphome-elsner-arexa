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

### 13. Station packets with wrong bits (the fixed gain: replaced, see 14)
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
A calibration that compared the levels by the station's answers (2026-11-08) settled on 2 steps, worse against an SDR
(28% against 36%, 2026-11-26), and is removed: 3 steps fixed. Letting the AGC set the gain on each station packet and holding it failed (1 of 6
usable): the 13-chip preamble is too short for the AGC. The AGC running all the time failed too (half broken). Even
with a limit, 1 ms was not always enough for the gain to climb to it (RC RSSI -67/-57/-54 from one fixing to the
next); with 5 ms it lands on the limit every time (25 of 26), 3 steps: 93% usable.
With the gain settled, the receive settings again (2026-11-09, 10 h alternating, ~140 station packets each):
deviation 41.3 kHz with filter 232 kHz gave 83% clean, 94% usable, against 76% and 91% at 47.6/270 (each alone no
better; 325 kHz and +-5 kHz worse), so that is the default now. Rebuilt from neighbouring clean packets, most
broken ones have 1 or 2 wrong bits; a few lose the bit timing (50+ wrong bits) or have a short burst (9-17).

### 14. The station's packets the 868 module missed: solved (the input attenuator, the DC filter)
An RTL-SDR (farther away, a plain monopole) decoded 100% of the RC's, the station's and my packets; the gateway got
44-61% of the station's (most missed entirely, the rest with 2-5 wrong bits spread over the packet), the RC's 85-95%.
The station's signal is the cleanest of the three (tones +-46 kHz, 4-5 kHz jitter, +5 kHz from my TX, chip rate
+0.3%). Not the cause (each measured against the SDR, alternating per send or minute): ESP latency (the driver reads
0.1 ms after GDO0), the receiver's restart after a packet (staying in RX the same), the Wi-Fi updates in the reply
gap (handling packets 30 ms later the same), TI's 100 kBaud demodulator and AGC values, FOC gated or off, FREND1,
FSCAL/TEST0, deviation 47.6 kHz, other gain caps. Helped: TEST2/TEST1 0x81/0x35 (now the default), 61% against 44%.
Polling the chip's status during a packet breaks the packet (SPI activity), so no probing that way.
Solved on 2026-11-27 (the same method, 5 to 45 minutes per round, 30 to 100 station answers per setup):
- The input attenuator (FIFOTHR.CLOSE_IN_RX): 0 dB 27-44%, 6 dB 83-97%, 12 dB 89-94%, 18 dB 87%. Lower gain behind
  it does nothing (LNA limits -6.1/-11.5/-17.1 dB: 62/31/27%), so it is not the level at the demodulator.
- The DC blocking filter off (MDMCFG2.DEM_DCFILT_OFF): 56-78% against 32% at 0 dB, 97-98% against 83% with 6 dB.
- With both: IF 153, 203 or 254 kHz, the AGC frozen on sync or not, TEST2/TEST1 at TI's or the reset values: all
  93-100%, no difference left to measure. Kept: IF 153 kHz, the AGC as by default, TI's TEST2/TEST1.
- So the fixed gain and its special case while sending are gone (the AGC runs).
Not the cause either: the 433 MHz radios (asleep during a command: 43% against 42%), Wi-Fi (no ESP traffic around
the answer: 28% against 36%; the level in the channel is the same after a Wi-Fi send), the AGC stepping during the
packet (frozen on sync: 17-36%), the packet's content or its signal (lost and received ones look the same on the
SDR: level, frequency, deviation, start, timing). The broken packets (181 compared with the SDR's copy): mostly 2
wrong bits next to each other, twice as many in the first half, a 0 read as 1 more often than the reverse.
The shipped setup, 31 minutes against the SDR: the station's answers to my sends 94 of 96, to the RC 18 of 18, the
RC's packets 93 of 94; 2 of 220 frames broken or repaired.
Not understood: why these two settings. The level is not it (the RC is as strong and arrives at 0 dB). What differs
is that the station answers 6.6 ms after another packet. On 2026-11-05 the 433.30 module tuned to 868 MHz got 175 of
177 answers at -84 dBm: with 24 dB less signal, like a strong attenuator.
An on-device test script (the ESP pressing its own buttons) crashed in a log call: not looked into.
Checked afterwards (2026-11-27 morning), nothing changed by it:
- A normal night on the shipped setup, 170 minutes against the SDR with no test sends: the RC's packets 168 of 171,
  the station's answers to it 158 of 166, 1 of 341 frames broken.
- The station's signal against the RC's on the SDR (226 and 53 bursts): carrier +4.8 and +1.6 kHz from my TX,
  deviation +-40 and +-37 kHz, drift through the packet under 1 kHz, 99% bandwidth 191 and 188 kHz, the same 0.3 ms
  of plain carrier before the data. So the station sends like the RC. At the gateway it is 8 dB weaker (-66 against
  -58 dBm with 6 dB), and broken and good station frames have the same RSSI.
- The DC filter on at IF 203 kHz (39 of 41 the night before) was luck: with 6 dB, DC on 23 of 29 (IF 153) and 23 of
  27 (IF 203), DC off 24 of 24 and 24 of 25.
- The ESP's Wi-Fi modem kept awake instead of sleeping between beacons: 10 of 26 against 7 of 24 with the old
  settings, 10 of 12 against 12 of 13 with the shipped ones.
- The likely cause of the overload (not proven; the README has it): an LTE 800 downlink at 811-821 MHz. The 868
  chip tuned there reads -54 dBm in 232 kHz (peaks -42; about -38 dBm over the block) against -62 dBm for the
  station; the SDR shows the block 35 dB over its floor, clipping. 796, 805, 840 and 780 MHz: nothing. The phone's
  uplink block (852-862 MHz) was not measured. The chip's idle RSSI is no use as a noise measurement (flat -77 dBm at
  0 dB, -92 at 6 dB, -86 at 18 dB; it moves 23 dB with the DVGA limit). The test: an 868 MHz SAW filter, then 0 dB.
- Carrier sense for the sync word (set_carrier_sense_above_threshold; the SYNC_MODE_*_CS values of the driver do not
  switch it on): the false starts on noise go from 19 a minute to 0, the answers stay the same (17 of 18 without, 16
  and 17 of 18 with -7 and 0 dB) and at +7 dB a third is lost. Not used. Sync on 15 of 16 bits: 309 false starts a
  minute.
The station nearly always sends within 25 ms of another packet, so "the station's" and "right after another packet"
cannot be told apart from this data.

## Low

- The command list exists four times in an order that must match (now noted at each). Generate from one list?
- Wind: a 16-bit value like the temperature (README: Values), so above 25.5 m/s should just work; not seen yet.
- The scrambler's 31 masks (README: Scrambling) follow a shift pattern but no short recurrence I found; a generator
  would replace the table.
