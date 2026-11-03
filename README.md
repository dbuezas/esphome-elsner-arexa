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

The repo has a copy of ESPHome's `cc1101` driver (from 2026.10.1) with one addition: fixed-length packets longer
than the 64-byte FIFO, read in parts while they arrive. An Arexa frame is 74 FIFO bytes (the CRC is in the last
4), so the stock driver drops them. With `components: [elsner_arexa, cc1101]` the copy replaces the stock driver
for every CC1101 on the node; without the addition, a packet length of 64 or less behaves as in the stock driver.

```yaml
external_components:
  - source: github://dbuezas/esphome-elsner-arexa
    components: [elsner_arexa, cc1101]  # cc1101: my copy of ESPHome's driver, see below

cc1101:
  - id: radio_arexa
    cs_pin: D8
    gdo0_pin: D2
    packet_mode: true    # needed: elsner_arexa sets all other radio settings itself
    packet_length: 64    # any value: elsner_arexa sets 74

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
| `text_sensor` | `system`: the system bytes of the RC heard on the air; `station_raw`: the station's value bytes 17..32 (for working out new values); `last_command`: the result of the last command sent (see below) |

**System (the pairing):** by default (`system: auto`) the component learns your RC's system ID and the RC's and
the station's addresses from the first RC status packet whose CRC matches. The `system` text sensor shows what arrives, as 18 hex
digits (bytes 8 to 16 of the RC's packets: the system ID, the RC's address, the station's address and the device
types). After that, only packets with this system ID (bytes 8, 9 and 13, 14) count, so a neighbour's Arexa does
not feed its weather into the sensors or move the counter. To skip the learning, put that value into the hub, for example `system: 1a2b3c4d5e1a2b6f70` (made up: use yours).
Then the buttons work as soon as any packet of that system (RC or station) has brought the counter.

**Frequency:** cheap CC1101 modules are often 20 to 40 kHz off. Mine needs `frequency: 868.227MHz` to come out at
868.19 MHz. If the buttons do nothing or only the RC's packets come in, try steps of 10 kHz between 868.15 and
868.24 MHz. The `rc_rssi` / `station_rssi` sensors and the log (`Learned my RC: ...`) show whether packets arrive.

**Sharing the radio:** if something else uses the same CC1101 (for example a `remote_transmitter` in OOK mode), call
`id(<your elsner_arexa id>).start_listening();` after it, to put the radio back into Arexa mode.

**Sending and its result:** a command goes out as 3 copies 30 ms apart. The station answers the first copy it
hears, 14 ms later, with the command's counter. Without that answer, I send again with a new counter every 300 ms,
at most 4 times. A mode command needs one more send, because the station answers with its mode from before the
command: it counts as done when an answer shows the new mode. About 1 station answer in 5 is not heard, so a
second send happens now and then. The `last_command` text sensor shows the result, so an automation can alert:
`stop: confirmed`, `stop: no answer after 4 sends`, `auto: mode unchanged after 4 sends`,
`stop: not sent, RC not heard yet`, or `open: replaced by stop` (a new command stops the one being sent). Failures
are also logged as warnings.

**Open, Stop and Close end automatic mode**, like on the RC: the station switches to manual (seen with Stop). Send
Auto afterwards if the station should go on reacting to rain and wind by itself.

**After a restart** the buttons work once the component has heard an RC status packet (the RC sends one every
minute), or with a fixed `system` once any packet of that system came in. Until then they log a warning and send nothing.

**"err" on the RC display:** right after a command from Home Assistant, the RC's next exchange can show "err" for
a second. The RC sleeps between its packets, so it does not see the new counter; it learns it from the station's
answer, and the next exchange is fine.

## Accuracy

The value scales come from my own RC display readings, so they are a first fit:

| Value | Formula | Based on |
|---|---|---|
| inside temperature | bytes 22, 23 (see Protocol) | the RC cooled in a freezer and warmed up: every display step from -1 to 13 °C, plus 20 to 25 °C; a thermometer next to the RC agrees within 0.1 °C |
| outside temperature | bytes 17, 18 (see Protocol) | 30 readings, 7 to 11 °C; below 0 °C not seen yet |
| brightness | (byte 24 - 2) × 135 lx | 5 readings from 0 to 6.5 klux (the RC shows whole klux) |
| wind | (256 - byte 19) / 10 m/s | 3 readings: 0, 1 and 2 m/s (the RC shows whole m/s) |

If you have more pairs (RC display, the `raw_*` sensors), please open an issue.

## How it works

I decoded the protocol with an RTL-SDR, starting from the rtl_433 decoder `elsner_solexa` (364), which knows the
frame and the CRC. The Solexa (the shade version) uses the same radio protocol: my model also decodes its
recordings in rtl_433_tests, so the buttons may work there too, but its sensor bytes are different.

Receiving: the chip hands over raw chips (its own Manchester decoder locks on the wrong chip phase about half the
time with this short preamble), synced on the 16 chips of `ce`, and the ESP decodes them. All 37 frame bytes after the sync word arrive (74 FIFO
bytes, read while the packet comes in), so the CRC is checked. A packet with a broken chip pair is still used for
the fields with a check of their own (the temperatures and the wind are pairs); everything else (rain, wind alarm,
brightness, mode, rain sensor, the counter, the learning) comes only from packets whose CRC matches. The station answers the RC 14 ms after its packet, so the component sets the main loop interval to 2 ms (the
default 16 ms misses most answers). The gain is frozen once the sync word is found: the station is often much
weaker than the RC, and gain steps inside its packets broke them into bursts of wrong chips. Sending: the chip does
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

| Bytes | Content |
|---|---|
| 5..7 | packet counter: `v5` counts down (the RC and the station share it), `v6 = K - 2 * v5`, `v7` changes rarely (K belongs to it). When `v5` reaches `00`, a new block starts: `v7 + 2` and `K - 1` (seen: `01 17` K `70`, then `00 19` K `6f`, `ff 19` K `6f`). After an "err" the RC resyncs with a new `v7` and K. The station only takes a command whose counter is newer than the last one. |
| 8, 9 | system ID (set by the teach-in), repeated in 13, 14 |
| 10 | sender address |
| 15 | receiver address |
| 11, 12, 16 | device type |
| 17..19 | RC: the command (below). Station: the measurements start here. |
| 20.. | filler `30` where nothing is sent |

RC commands (bytes 17, 18, 19), the same on the Solexa recordings:

| Command | 17 | 18 | 19 |
|---|---|---|---|
| status (once a minute) | 6a | 30 | 30 |
| open | 25 | ba | 30 |
| stop | 27 | b6 | 30 |
| close | 36 | 98 | 30 |
| automatic mode | 6a | 1f | 52 |
| manual mode | 6a | 13 | 6a |

Measurements (Arexa; the Solexa's sensor has a different layout):

| Sender | Byte | Content |
|---|---|---|
| RC status | 22, 23 | inside temperature (below) |
| station | 17, 18 | outside temperature (below) |
| station | 19, 20 | wind in 0.1 m/s: `(256 - v19) & 0xff`, byte 20 = 2 × that |
| station | 21 | rain alarm: `b2` dry, `ae` wet |
| station | 24 | brightness, about (v - 2) × 135 lx |
| station | 25 | mode: `b3` manual, `bf` automatic |
| station | 28 | wind flag: `27` calm |
| station | 29 | rain sensor reading: `a6` dry, lower = wetter |
| station | 31..35 | `30` in a status packet (`00`/`ff` in the start-up packets after a battery change) |

A temperature is two bytes `a, b` in tenths of a degree: `low = (base - a) & 0xff`, `b = 2 * low + sign`, and
`tenths = low - 256 * sign`. `base` is 168 for the RC's inside sensor and 154 for the station. The RC also sends the
high byte in byte 24 (`d0 + 2 * high`: `d0` above 0 °C, `ce` below), so the inside has no range limit. Checked from
-1 to 25 °C inside. The station has no such byte: with the sign bit alone, its range would be -25.6 to +25.5 °C,
so something else must change above 25.5 °C (not seen yet), and its sign is not seen yet either. Other value bytes are followed by one that holds about `constant - 2 × value` in the same way.
