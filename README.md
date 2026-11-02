[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/dbuezas)

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
frame and CRC. The Solexa (the shade version) uses the same radio protocol; I checked my model against its
recordings in rtl_433_tests, so the buttons may work there too, but the sensor bytes are different.

- **Radio:** 868.19 MHz, 2-FSK, ±35 kHz, Manchester (1 = chips 10, 0 = 01), chips of exactly 10.000 µs
  (100 kbaud). A short preamble (~13 alternating chips), the anchor `0a`, then 38 bytes.
- **Frame:** `ce ad 93 ba`, a random seed byte, 31 payload bytes, CRC-16 (poly 0x1021, init 0x68b3).
- **Scrambling:** with `d[k] = c[k] - 2 c[k-1]` (mod 256), every payload byte is
  `d[k] = value[k] + (parity(seed & mask[k]) ^ inv[k])`. The masks are the same on every unit (see the source).
  This rebuilds all 353 frames of my RC bit for bit.
- **Payload values:** bytes 5 to 7 are a packet counter (byte 5 counts down; RC and station share it), bytes 8 to
  16 are the system ID and the sender and receiver addresses (set by the teach-in), bytes 17 to 19 the command,
  then the measurements.
- **The station takes a command only if its counter is newer** than the last one it saw: the component takes the
  counter of the last packet it heard and counts on from it.

Receiving: the chip hands over raw chips (its own Manchester decoder locks on the wrong chip phase about half the
time with this short preamble), synced on the 16 chips of `ce`; the ESP decodes them. Sending: the chip does the
Manchester coding itself (an ESP8266 cannot time 10 µs chips in software while WiFi runs).
