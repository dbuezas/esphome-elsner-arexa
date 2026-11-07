[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/dbuezas)

Post in HA forum: https://community.home-assistant.io/t/elsner-arexa-solexa-868-mhz-radio-in-home-assistant-with-esphome-cc1101

# esphome-elsner-arexa

An ESPHome component for the **Elsner Arexa** window control (Arexa 230 V: a battery RC handset and a weather
station that drives the window motor). With a CC1101 868 MHz module it gives you in Home Assistant:

- **Buttons:** open, stop, close, automatic mode, manual mode. They work like the RC's own buttons.
- **A switch** for automatic mode.
- **Sensors:** inside temperature (from the RC), outside temperature, wind, brightness, rain alarm, rain sensor,
  automatic or manual mode.
- **An event** for every press on the real RC.

Nothing has to be opened or re-paired: the component listens to your RC for up to a minute and then speaks with
its system ID and addresses. The RC keeps working as before.

## Setup

It needs ESPHome's `cc1101` component (ESPHome 2026.10 or newer).

```yaml
external_components:
  - source: github://dbuezas/esphome-elsner-arexa
    components: [elsner_arexa]

cc1101:
  - id: radio_arexa
    cs_pin: D8
    gdo0_pin: D2
    packet_mode: true    # needed: elsner_arexa sets all other radio settings itself
    packet_length: 64    # any value: elsner_arexa sets its own

elsner_arexa:
  cc1101_id: radio_arexa
  # frequency: 868.19MHz   # 868.19 MHz + your module's crystal error, see below
  # output_power: 11

button:
  - platform: elsner_arexa
    open: { name: Arexa Open }
    stop: { name: Arexa Stop }
    close: { name: Arexa Close }
    auto: { name: Arexa Auto }
    manual: { name: Arexa Manual }
```

The full example with all sensors is in [examples/elsner-arexa.yaml](examples/elsner-arexa.yaml).

| Platform | Keys |
|---|---|
| `sensor` | `inside_temperature`, `outside_temperature`, `wind`, `brightness`, `rain_sensor`, `rc_rssi`, `station_rssi`, `rc_freq_offset`, `station_freq_offset`, and `raw_inside`, `raw_outside`, `raw_brightness` (for calibration) |
| `binary_sensor` | `rain` (the station's rain alarm), `wind_alarm`, `auto_mode` (on in automatic mode) |
| `switch` | `auto_mode` (on: automatic, off: manual; it shows the station's mode) |
| `button` | `open`, `stop`, `close`, `auto`, `manual` |
| `event` | `remote` (event types `open`, `stop`, `close`, `auto`, `manual`; one event per press: the RC's repeats have the same counter) |
| `text_sensor` | `pairing`: the pairing of the RC heard on the air; `station_raw`: the station's value bytes 17..35 (for working out new values); `last_command`: the result of the last command sent (see below) |

**Pairing:** by default (`pairing: auto`) the component learns your RC's system ID and the RC's and
the station's addresses from the first RC status packet. The `pairing` text sensor shows what arrives, as 20 hex
digits (bytes 8 to 16 of the RC's packets: the system ID, the RC's address, the station's address and the device
types, then the counter's check byte). After that, only packets with this system ID (bytes 8, 9 and 13, 14) count, so a neighbour's Arexa does
not feed its weather into the sensors or move the counter. To skip the learning, put that value into the hub, for example `pairing: 1a2b3c4d5e1a2b6f7048` (made up: use
yours; the system ID, digits 1..4, must come again in digits 11..14, which catches most typos). Then the buttons work right after a start, even without the RC
(see After a restart).

**Frequency:** cheap CC1101 modules are often 20 to 40 kHz off. Mine needs `frequency: 868.227MHz` to come out at
868.19 MHz. If the buttons do nothing or only the RC's packets come in, try steps of 10 kHz between 868.15 and
868.24 MHz. The `rc_rssi` / `station_rssi` sensors and the log (`Learned my RC: ...`) show whether packets arrive.

**Sharing the radio:** if something else uses the same CC1101 (for example a `remote_transmitter` in OOK mode), call
`id(<your elsner_arexa id>).start_listening();` after it, to put the radio back into Arexa mode.

**Sending and its result:** a command goes out as 3 copies 30 ms apart. The station answers the first copy it
hears, 14 ms later, with the command's counter. Without that answer, I send again with a new counter every 300 ms,
at most 4 times. If none of them is answered, my counter may be behind the station's: I announce a new counter
block (a resync, see Protocol) and then send the command again, 4 more times at most. The station answers with its state from before the command, so a mode command needs one more
send (it counts as done when an answer shows the new mode), and so do open, close and stop in automatic mode (they
switch the station to manual: the second answer shows it). A damaged answer (the CRC does not match; a single wrong
bit is repaired) also means one more send. The `last_command` text sensor shows the result, so an automation can alert:
`stop: confirmed`, `stop: no answer after 4 sends`, `stop: no answer to a resync`, `auto: mode unchanged after 4 sends`,
`stop: not sent, pairing not known yet`, `stop: not sent, no packet heard yet` (a fixed pairing without the check byte), or `open: replaced by stop` (a new command stops the one being sent). Failures
are also logged as warnings.

**Open, Stop and Close end automatic mode**, like on the RC: the station switches to manual (seen with Stop). The
station closes the window on rain in both modes; automatic mode adds opening and closing by temperature.

**After a restart** the buttons work once the pairing is known: learned from an RC status packet (the RC sends one
every minute), or fixed in the configuration. Without a counter heard yet, a command starts with a resync, so with a
fixed `pairing` (all 20 digits) the buttons also work without the RC, for example to replace a broken one. (Automatic mode then
lacks the inside temperature, which only the RC measures.)

**"err" on the RC display:** right after a command from Home Assistant, the RC's next exchange can show "err" for
a second. The RC sleeps between its packets, so it does not see the new counter; it learns it from the station's
answer, and the next exchange is fine.

## Accuracy

The value scales come from my own RC display readings, so they are a first fit:

| Value | Formula | Based on |
|---|---|---|
| inside temperature | bytes 22, 23 (see Protocol) | the RC cooled in a freezer and warmed up: every display step from -1 to 13 °C, plus 20 to 25 °C; a thermometer next to the RC agrees within 0.1 °C |
| outside temperature | bytes 17, 18 (see Protocol) | 30 readings, 7 to 11 °C; seen down to -0.7 °C |
| brightness | bytes 22, 23 (see Protocol), × 1.05 lx | 5 readings from 0 to 6.5 klux (the RC shows whole klux) |
| wind | bytes 19, 20 (see Protocol) | 3 readings: 0, 1 and 2 m/s (the RC shows whole m/s) |

If you have more pairs (RC display, the `raw_*` sensors), please open an issue.

## How it works

I decoded the protocol with an RTL-SDR, starting from the rtl_433 decoder `elsner_solexa` (364), which knows the
frame and the CRC. The Solexa (the shade version) uses the same radio protocol: my model also decodes its
recordings in rtl_433_tests (the same commands, and its sensor packet differs only in bytes 19, 20), so the
buttons may work there too. rtl_433's decoder describes the same scrambling differently (a bit transform plus a
"rolling state"); its `spread_k(seed)` is the seed part of the running sum here. Other Elsner RF devices (Solexa
II, Solexa Home, WS1, Remo remotes, RF-MSG-ST) use "Elsner RF" at 868.2 MHz too; I found no public decoding of them.

Receiving: the chip decodes the Manchester itself, synced on the first two frame bytes `ce ad` (the preamble is
too short to fix the chip phase; the sync word does). The 36 bytes after it, with the CRC, fit the 64-byte FIFO.
Only packets whose CRC matches are used: the chip's decoder does not say which bytes are broken. Where a chip pair
is broken, it guesses the bit, so many damaged packets have just one wrong bit: the CRC finds and repairs it. A
repair can also turn a packet with 3 or more wrong bits into a wrong one (about 1 in 230 of those), so a repaired
packet only confirms a send (it must carry the station's address, my system ID and the exact counter sent); the
sensors, the counter and the RC events come only from packets whose CRC matched as received. I also tried raw
chips decoded on the ESP (sync on the 16 chips of `ce`, 74 FIFO bytes read while the packet arrives, damaged packets
used for the fields with a check of their own): over 4 hours alternating, 29% of the station's packets came in
with a matching CRC that way, against 42% with the chip's decoder. The station answers the RC 14 ms after its packet, so the component sets the main loop interval to 2 ms (the
default 16 ms misses most answers). The gain is fixed (no AGC): with the AGC, the gain was still low from the
RC's packet when the station answered, 16 dB weaker, and 43% of its packets came with wrong bits; with the gain
fixed, 6% (4 hours alternating). FSK needs no gain control. Sending: the chip does
the Manchester coding itself (an ESP8266 cannot time 10 µs chips in software while WiFi runs).

## Protocol

### Radio

- 868.19 MHz, 2-FSK. Deviation about ±35 kHz (RC) and ±45 kHz (station).
- Manchester: a 1 is the chips `10`, a 0 is `01`. Chips of exactly **10.000 µs** (100 kbaud). rtl_433's flex
  setting `short=11` works only because it is tolerant.
- A short preamble (about 13 alternating chips), the anchor byte `0a`, then the 38-byte frame.
- The RC and the station talk once a minute (the RC sends its status, the station answers 14 ms later), and on
  every button press (the RC repeats a command until the station answers).

### Frame

| Bytes | Content |
|---|---|
| 0..3 | `ce ad 93 ba` |
| 4 | seed: random for every packet |
| 5..35 | payload, scrambled (below) |
| 36..37 | CRC-16 over bytes 0..35: polynomial 0x1021, init 0x68b3 |

### Scrambling

With `d[k] = c[k] - 2 * c[k-1]` (mod 256, on the raw bytes `c` as sent), every payload byte is

```
d[k] = value[k] + (parity(seed & mask[k]) ^ inv[k])        seed = c[4]
```

| k | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 | 24 | 25 | 26 | 27 | 28 | 29 | 30 | 31 | 32 | 33 | 34 | 35 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mask | 01 | 01 | 01 | 01 | 01 | 01 | 01 | 81 | c1 | e1 | f1 | f9 | fd | ff | fe | ff | fe | ff | fe | ff | fe | ff | 7e | bf | 5e | af | 56 | ab | 54 | aa | 55 |
| inv | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 1 | 0 | 1 | 0 | 1 | 0 | 1 | 0 | 1 | 0 | 1 | 0 | 1 | 1 | 0 | 0 | 1 | 1 | 0 | 0 | 1 | 1 |

So `value[k] = c[k] - 2 * c[k-1] - (parity(seed & mask[k]) ^ inv[k])`. To build a frame: pick a seed, compute
`d[k]` and then `c[k] = d[k] + 2 * c[k-1]` from k = 5 on. This model rebuilds all 353 frames of my RC bit for bit.

### Values

The descrambled bytes `v[k]` hold the header as is, but numbers in a second, simpler form: a running sum.

| Bytes | Content |
|---|---|
| 5..7 | the counter (below) |
| 8, 9 | system ID (set by the teach-in), repeated in 13, 14 |
| 10 | sender address |
| 15 | receiver address |
| 11, 12, 16 | device type |
| 17.. | the command (RC) or the measurements (station), as a running sum (below) |

### Counter

`n = k << 8 | (v5 - 1)` with `k = v6 + 2 * v5`: 16 bits, counting down by one per packet (the RC's own wraps:
`ae00`, `adff`, `adfe`). Byte 7 is a check: `v7 + 2 * k` is the same in every packet of a system (`f7` on mine, `48`
on the Solexa of rtl_433's recordings). The station takes a command whose counter is newer than its last one: lower,
mod 2^16 (so up to 32767 steps ahead). That also covers the RC after a command from the gateway: its next status has
an older counter, the station ignores it, the RC shows "err" and resyncs (below). To send:
`v5 = (n & ff) + 1`, `v6 = k - 2 * v5`, `v7 = check - 2 * k`.

### Running sum

From byte 17 on, `u[17] = v[17]` and `u[k] = v[k] + 2 * u[k-1]`. Then every value is a plain number: a 16-bit value
is `(b0 - u[k]) | (b1 - u[k+1]) << 8` with these bases (in the raw `v`, every byte after a value looked like a check
byte `constant - 2 * value`: that was this sum).

| Sender | Bytes (`u`) | Content |
|---|---|---|
| RC | 17, 18 | command: `25 04` open, `27 04` stop, `36 04` close, `6a f3` automatic mode, `6a e7` manual mode, `6a 04` status (once a minute) |
| RC status | 22, 23 | inside temperature in 0.1 °C, signed, bases `40 80` |
| station | 17, 18 | outside temperature in 0.1 °C, signed, bases `9a 34` |
| station | 19, 20 | wind in 0.1 m/s, bases `68 d0` |
| station | 21 | rain alarm: `52` dry, `4e` wet (byte 26 the same: `b2`, `ae`) |
| station | 22, 23 | brightness, about 1.05 lx per step, bases `3c 80`; byte 24 `00` |
| station | 25 | mode: `b3` manual, `bf` automatic |
| station | 28 | wind flag: `8b` calm |
| station | 29 | rain sensor reading: `bc` dry, lower = wetter |
| station | 27 | not decoded: `b2` in all packets so far |
| both | rest | filler: `d0` (`30` in `v`) |

The RC sends the commands as `v` (bytes 17..19, also on the Solexa): `25 ba 30` open, `27 b6 30` stop, `36 98 30`
close, `6a 1f 52` automatic, `6a 13 6a` manual, `6a 30 30` status. A start-up packet after a battery change has
`00`/`ff` in bytes 31..35 instead of the filler.

Checked on a week of station packets (1189, all decode; seen from -0.7 to 12.4 °C), the RC from -1 to 25 °C, and
on rtl_433's Solexa recordings: the same commands, the RC's inside temperature with the same bases (20.6 °C), and
the station's packet the same except bytes 19, 20 (the Solexa's sensor has no wind there).

In `v` this looked like a different formula for each value (a sign bit in byte 18, the temperature's high part in
the wind byte, bit 7 of the brightness only in byte 23), and a value that changed (the rain alarm) broke the next
one (the brightness while it rained).

### Resync

When the RC's counter is stale (it got an "err", for example after the gateway sent commands), it picks a new
counter at random and announces it. The station answers, and both count down from there. Nothing in it is secret:
the request names the new counter in clear.

| Bytes | Request (RC) | Answer (station) |
|---|---|---|
| 5..7 | the new counter (any value; byte 7 with the system's check) | the same |
| 10, 15 | RC, station address | station, RC address |
| 11, 12 | `82 ff` | `4d 01` |
| 16 | `2 * seed + 53 + p` | `2 * seed + 87 + p` |
| 17..35 | one bit per byte (`00` = 0, `ff` = 1) | the same, its own bits |

Bits 17..31 are a random tag. The rest follows from it: `b32 = b17 ^ b31`, `b33 = b32 ^ b18`, `b34 = b33 ^ b19`,
`b35 = b34 ^ b20`, and `p = b30 ^ b31 ^ 1`. The answer's tag is new, not a copy of the request's. The station does
not check the tag: it also answered requests with a wrong check bit.

This model rebuilds all 59 recorded requests bit for bit from the counter, the seed and the tag; in all 59 the RC's
next packets continue from the counter it announced.

Tested by sending requests from the gateway (2026-11-07):

- A recorded request, replayed as is: answered.
- The same with a new seed, or a new tag (check bits computed): answered.
- Made-up requests (counter, seed, tag): answered, also with a wrong check bit (`b35`, or `p` in byte 16). A Stop on
  such a counter was confirmed.
- Requests with a wrong counter check (byte 7): no answer, 3 tries each.

The component uses this: without a counter (after a start), or when no send of a command is answered, it announces
a random counter and sends the command on it. A resync can put the station's counter behind the RC's: then the
station takes the RC's next status (newer), and the RC needs no resync.
