[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/dbuezas)

Post in HA forum: https://community.home-assistant.io/t/elsner-arexa-solexa-868-mhz-radio-in-home-assistant-with-esphome-cc1101

# esphome-elsner-arexa

An ESPHome component for the **Elsner Arexa** window control (Arexa 230 V: a battery RC handset and a weather
station that drives the window motor). With a CC1101 868 MHz module it gives you in Home Assistant:

- **Buttons:** open, stop, down, automatic mode, manual mode. They work like the RC's own buttons.
- **Sensors:** inside temperature (from the RC), outside temperature, wind, brightness, rain alarm, rain sensor.
- **An event** for every press on the real RC.

Nothing has to be opened or re-paired: the component listens to your RC for up to a minute and then speaks with
its system ID and addresses. The RC keeps working as before.

## Setup

```yaml
external_components:
  - source: github://dbuezas/esphome-elsner-arexa
    components: [elsner_arexa]

cc1101:
  - id: radio_arexa
    cs_pin: D8
    gdo0_pin: D2
    packet_mode: true    # needed: elsner_arexa sets all other radio settings itself
    packet_length: 64

elsner_arexa:
  cc1101_id: radio_arexa
  # frequency: 868.19MHz   # 868.19 MHz + your module's crystal error, see below
  # output_power: 11

button:
  - platform: elsner_arexa
    open: { name: Arexa Open }
    stop: { name: Arexa Stop }
    down: { name: Arexa Down }
    auto: { name: Arexa Auto }
    manual: { name: Arexa Manual }
```

The full example with all sensors is in [examples/elsner-arexa.yaml](examples/elsner-arexa.yaml).

| Platform | Keys |
|---|---|
| `sensor` | `inside_temperature`, `outside_temperature`, `wind`, `brightness`, `rain_sensor`, `rc_rssi`, `station_rssi`, and `raw_inside`, `raw_outside`, `raw_brightness` (for calibration) |
| `binary_sensor` | `rain` (the station's rain alarm), `wind_alarm` |
| `button` | `open`, `stop`, `down`, `auto`, `manual` |
| `event` | `remote` (event types `open`, `stop`, `down`, `auto`, `manual`) |
| `text_sensor` | `system`: the system bytes of the RC heard on the air |

**System (the pairing):** by default (`system: auto`) the component learns your RC's system ID and the RC's and
the station's addresses from the RC's first status packet. The `system` text sensor shows what arrives, as 18 hex
digits (bytes 8 to 16 of the RC's packets: the system ID, the RC's address, the station's address and the device
types). To skip the learning, put that value into the hub, for example `system: 1a2b3c4d5e1a2b6f70` (made up: use yours).
Then the buttons work as soon as any Arexa packet (RC or station) has brought the counter.

**Frequency:** cheap CC1101 modules are often 20 to 40 kHz off. Mine needs `frequency: 868.227MHz` to come out at
868.19 MHz. If the buttons do nothing or only the RC's packets come in, try steps of 10 kHz between 868.15 and
868.24 MHz. The `rc_rssi` / `station_rssi` sensors and the log (`Learned my RC: ...`) show whether packets arrive.

**Sharing the radio:** if something else uses the same CC1101 (for example a `remote_transmitter` in OOK mode), call
`id(<your elsner_arexa id>).start_listening();` after it, to put the radio back into Arexa mode.

**After a restart** the buttons work once the component has heard one RC status packet (the RC sends one every
minute), or with a fixed `system` once any Arexa packet came in. Until then they log a warning and send nothing.

**"err" on the RC display:** right after a command from Home Assistant, the RC's next exchange can show "err" for
a second. The RC sleeps between its packets, so it does not see the new counter; it learns it from the station's
answer, and the next exchange is fine.

## Accuracy

The value scales come from my own RC display readings, so they are a first fit:

| Value | Formula | Based on |
|---|---|---|
| inside temperature | (428 - byte 22) / 10 °C | 8 readings, 20 to 25 °C |
| outside temperature | 14.65 - 0.0865 × byte 17 °C | 8 readings, 8 to 11 °C |
| brightness | (byte 24 - 2) × 135 lx | 2 readings, 0 and 6.5 klux |
| wind | raw (0 = calm) | no scale yet |

If you have more pairs (RC display, the `raw_*` sensors), please open an issue.

## How it works

I decoded the protocol with an RTL-SDR, starting from the rtl_433 decoder `elsner_solexa` (364), which knows the
frame and the CRC. The Solexa (the shade version) uses the same radio protocol: my model also decodes its
recordings in rtl_433_tests, so the buttons may work there too, but its sensor bytes are different.

Receiving: the chip hands over raw chips (its own Manchester decoder locks on the wrong chip phase about half the
time with this short preamble), synced on the 16 chips of `ce`, and the ESP decodes them. Sending: the chip does
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
| 5..7 | packet counter: `v5` counts down (the RC and the station share it), `v6 = K - 2 * v5`, `v7` changes rarely (K belongs to it). The station only takes a command whose counter is newer than the last one. |
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
| down | 36 | 98 | 30 |
| automatic mode | 6a | 1f | 52 |
| manual mode | 6a | 13 | 6a |

Measurements (Arexa; the Solexa's sensor has a different layout):

| Sender | Byte | Content |
|---|---|---|
| RC status | 22 | inside temperature, about (428 - v) / 10 °C |
| station | 17 | outside temperature, about 14.65 - 0.0865 × v °C |
| station | 19 | wind (0 = calm) |
| station | 21 | rain alarm: `b2` dry, `ae` wet |
| station | 24 | brightness, about (v - 2) × 135 lx |
| station | 28 | wind flag: `27` calm |
| station | 29 | rain sensor reading: `a6` dry, lower = wetter |
| station | 31..35 | `30` in a status packet (`00`/`ff` in the start-up packets after a battery change) |

In the RC's status packets and the station's packets, each value byte is followed by one that holds about
`constant - 2 × value`.
