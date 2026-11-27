#include "elsner_arexa.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome::elsner_arexa {

static const char *const TAG = "elsner_arexa";

// The frame (38 bytes after the anchor 0x0a): ce ad 93 ba, seed, 31 payload bytes, CRC-16 (poly 0x1021, init 0x68b3).
// With d[k] = c[k] - 2 c[k-1] (mod 256), every payload byte is d[k] = value[k] + (parity(seed & MASK[k]) ^ INV[k]),
// where seed = c[4] is random per packet. The masks are the same on every installation I have data from.
static const uint8_t MASK[36] = {0,    0,    0,    0,    0,    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                                 0x81, 0xc1, 0xe1, 0xf1, 0xf9, 0xfd, 0xff, 0xfe, 0xff, 0xfe, 0xff, 0xfe,
                                 0xff, 0xfe, 0xff, 0x7e, 0xbf, 0x5e, 0xaf, 0x56, 0xab, 0x54, 0xaa, 0x55};
static const uint8_t INV[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0,
                                1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1};
// frame bytes 0..3, the same in every packet
static const uint8_t HEADER[4] = {0xce, 0xad, 0x93, 0xba};
// frame bytes 0..37 (36, 37 the CRC)
static constexpr int N = FRAME_LEN;
// a value byte not in use
static constexpr uint8_t FILLER = 0x30;
// byte 17 of the RC's status packet (and of its mode commands)
static constexpr uint8_t RC_STATUS = 0x6a;
// the station's mode, byte 25
static constexpr uint8_t MODE_MANUAL = 0xb3;
static constexpr uint8_t MODE_AUTO = 0xbf;
// The RC's commands, in the order of the Command enum: the name (as in COMMANDS, __init__.py) and the values of
// bytes 17, 18, 19 (the same on a second installation's Solexa recordings)
static const struct {
  const char *name;
  uint8_t bytes[3];
} COMMANDS[] = {
    {"open", {0x25, 0xba, FILLER}},  {"stop", {0x27, 0xb6, FILLER}}, {"close", {0x36, 0x98, FILLER}},
    {"auto", {RC_STATUS, 0x1f, 0x52}}, {"manual", {RC_STATUS, 0x13, 0x6a}},
};
// Sending (README: Sending and its result): a send is COPIES copies COPY_MS apart; without the answer a new send
// every SEND_MS, SENDS at most (the 4 is also in the result texts of loop())
static constexpr int COPIES = 3;
static constexpr int SENDS = 4;
static constexpr uint32_t COPY_MS = 30;
static constexpr uint32_t SEND_MS = 300;
// the automatic mode's settings A and B switch back this far from their limit (°C)
static constexpr float HYSTERESIS = 2.0f;

static uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0x68b3;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i] << 8;
    for (int k = 0; k < 8; k++)
      crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

// Repairs a single wrong bit in bytes 2..37 (the chip's Manchester decoder guesses a bit when a chip pair is
// broken: most damaged packets have one wrong bit). The CRC-16 detects any 3 wrong bits at this length, so a single
// wrong bit is found for sure (but 3 or more can look like one: see decode_). The CRC is linear: a wrong bit at
// position p changes it by x^(288 - 1 - p + 16) mod the polynomial, whatever the data.
// True if the CRC matches (afterwards); *fixed tells whether a bit was flipped.
static bool crc_check_and_fix(uint8_t *c, bool *fixed) {
  const uint16_t syndrome = crc16(c, 36) ^ (c[36] << 8 | c[37]);
  *fixed = syndrome != 0;
  if (syndrome == 0)
    return true;
  if ((syndrome & (syndrome - 1)) == 0) {  // one bit of the CRC bytes themselves
    if (syndrome >> 8)
      c[36] ^= syndrome >> 8;
    else
      c[37] ^= syndrome;
    return true;
  }
  uint16_t r = 0x1021;  // the change for the last data bit (the lowest bit of byte 35, sent last)
  for (int p = 36 * 8 - 1; p >= 16; p--) {
    if (r == syndrome) {
      c[p / 8] ^= 0x80 >> (p % 8);
      return true;
    }
    r = r & 0x8000 ? (r << 1) ^ 0x1021 : r << 1;
  }
  return false;
}

static bool is_rc_status(const uint8_t *v) { return v[17] == RC_STATUS && v[18] == FILLER; }

static uint8_t parity_bit(uint8_t seed, int k) { return __builtin_parity(seed & MASK[k]) ^ INV[k]; }

// The counter: 16 bits counting down, n = k << 8 | (v5 - 1) with k = v6 + 2 v5. Byte 7 holds a check: v7 + 2 k is
// the same in every packet of a system (f7 on mine, 48 on a Solexa). See the README, Counter.
static uint8_t counter_k(const uint8_t *v) { return v[6] + 2 * v[5]; }
static uint16_t counter_of(const uint8_t *v) { return counter_k(v) << 8 | static_cast<uint8_t>(v[5] - 1); }
static uint8_t counter_check_of(const uint8_t *v) { return v[7] + 2 * counter_k(v); }

// A 16-bit value from the running sum u (decode_): (b0 - u[k]) | (b1 - u[k+1]) << 8, with bases per value (README:
// Values).
static uint16_t value16(const uint8_t *u, int k, uint8_t b0, uint8_t b1) {
  return static_cast<uint8_t>(b0 - u[k]) | static_cast<uint8_t>(b1 - u[k + 1]) << 8;
}

// the pairing: pairing[0..8] = the values of bytes 8..16 of the RC's packets: 8, 9 the system ID (repeated in 13, 14), 10 the RC's
// address, 15 the station's address (the RC's packets go to the station), 11, 12, 16 the device type
void ElsnerArexa::use_pairing_(const uint8_t *pairing) {
  std::copy(pairing, pairing + 9, this->pairing_);
  this->learned_ = true;
}

// 9 bytes (the pairing), or 10 with the counter's check byte (then a command can resync before any packet is heard)
void ElsnerArexa::set_fixed_pairing(const std::vector<uint8_t> &pairing) {
  if (pairing.size() != 9 && pairing.size() != 10)
    return;
  this->use_pairing_(pairing.data());
  if (pairing.size() == 10)
    this->counter_check_ = pairing[9];
  this->fixed_ = true;
}

void ElsnerArexa::setup() {
  if (this->radio_->is_failed()) {
    this->mark_failed();
    return;
  }
  this->radio_->register_listener(this);
  // the slot of a removed setting: on the ESP8266 the order of make_preference places the later ones
  global_preferences->make_preference<uint8_t>(fnv1_hash("elsner_arexa_gain_5ms"));
  // TEST2/TEST1 as TI gives them for RX filters up to 325 kHz (DN022; the driver keeps the reset values, meant for
  // wider ones). With the attenuator and the DC filter off I measured no difference (TODO.md). The driver writes
  // these registers only in its own setup, so once is enough.
  this->write_register_(0x2C, 0x81);  // TEST2
  this->write_register_(0x2D, 0x35);  // TEST1
  this->start_listening();
  if (std::isnan(this->outside_lock_))
    this->locked_ = 0;  // no outside lock: never locked
  // the window's last position survives a reboot (from before the position: open or closed; the old one first: on
  // the ESP8266 the order of make_preference places them)
  auto window_pref = global_preferences->make_preference<int8_t>(fnv1_hash("elsner_arexa_window"));
  this->position_pref_ = global_preferences->make_preference<float>(fnv1_hash("elsner_arexa_position"));
  float position;
  int8_t window;
  if (this->position_pref_.load(&position) && position >= 0 && position <= 1)
    this->pos_ = position;
  else if (window_pref.load(&window) && window >= 0 && window <= 1)
    this->pos_ = window;
  this->publish_position_();
  // The station answers the RC 14 ms after its packet, and the CC1101 driver listens again only when the main loop
  // has read the RC's packet. With the default 16 ms loop most station packets are missed.
  if (App.get_loop_interval() > 2)
    App.set_loop_interval(2);
}

// Receive settings. The chips are 10.000 us (100 kbaud), 2-FSK, Manchester coded (1 = chips 10, 0 = 01); the chip
// decodes the Manchester. The preamble is only ~13 alternating chips, which fit both chip phases: the sync word, the
// first two frame bytes ce ad, fixes the phase. Then 36 bytes: frame bytes 2..37, with the CRC.
// The numbers are measured, not derived (README: How it works, with the results; TODO.md has the history).
void ElsnerArexa::start_listening() {
  auto *r = this->radio_;
  r->set_frequency(this->frequency_);
  r->set_modulation_type(cc1101::Modulation::MODULATION_2_FSK);
  r->set_symbol_rate(100000);
  r->set_fsk_deviation(41.3e3f);  // the senders swing +-33 to +-46 kHz; 47.6 kHz was worse
  r->set_filter_bandwidth(232e3f);  // 270 and 325 kHz were worse
  r->set_sync_mode(cc1101::SyncMode::SYNC_MODE_16_16);
  r->set_manchester(true);
  r->set_sync1(HEADER[0]);
  r->set_sync0(HEADER[1]);
  r->set_num_preamble(2);
  r->set_foc_bs_cs_gate(false);  // the preamble is short: correct frequency and bit timing at once
  r->set_wait_time(cc1101::WaitTime::WAIT_TIME_8_SAMPLES);
  r->set_crc_enable(false);  // the Arexa's CRC is not the chip's: checked in decode_
  r->set_whitening(false);
  r->set_packet_length(N - 2);
  r->set_packet_mode(true);
  r->set_output_power(this->output_power_);
  // Two settings make the station's packets arrive (it answers a few ms after another packet; the RC's come through
  // either way): the attenuator at the chip's input (lower gain behind it, LNA or DVGA limits or a fixed gain, does
  // not help), and the DC blocking filter off. The AGC runs as the chip does by default; the DVGA's 3 highest steps
  // stay left out, as in every test with the attenuator.
  r->set_rx_attenuation(this->rx_attenuation_);
  r->set_dc_blocking_filter(false);
  r->set_max_dvga_gain(cc1101::MaxDvgaGain::MAX_DVGA_GAIN_MINUS_3);
  r->set_freeze(cc1101::Freeze::FREEZE_DEFAULT);
  r->begin_rx();
}

void ElsnerArexa::write_register_(uint8_t address, uint8_t value) {
  this->radio_->enable();
  this->radio_->write_byte(address);
  this->radio_->write_byte(value);
  this->radio_->disable();
}

// Called by the CC1101 driver before it listens again: keep it short (the station answers the RC 14 ms later).
void ElsnerArexa::on_packet(const std::vector<uint8_t> &packet, float freq_offset, float rssi, uint8_t lqi) {
  if (packet.size() != N - 2 || this->queue_.size() >= 8)
    return;
  this->queue_.emplace_back();
  auto &item = this->queue_.back();
  std::copy(packet.begin(), packet.end(), item.bytes);
  item.rssi = rssi;
  item.freq_offset = freq_offset;
}

void ElsnerArexa::loop() {
  for (const auto &item : this->queue_)
    this->decode_(item.bytes, item.rssi, item.freq_offset);
  this->queue_.clear();
  if (this->dir_ != 0) {
    const float p = this->position_();
    if (this->dir_ > 0 ? p >= 1 : p <= 0) {  // at the end: still
      this->pos_ = p;
      this->dir_ = 0;
      this->publish_position_();
    } else if (!std::isnan(this->target_) && this->cmd_ < 0 &&
               (this->dir_ > 0 ? p >= this->target_ : p <= this->target_)) {
      // (the stop waits while the open or close is still being sent)
      this->target_ = NAN;
      this->send_command(CMD_STOP);
    } else if (millis() - this->publish_ms_ >= 1000) {
      this->publish_position_();
    }
  }
  if (this->cmd_ < 0)
    return;
  // the station answers 14 ms after a copy it takes; it ignores the later copies (same counter)
  const uint32_t t = millis() - this->send_ms_;
  if (this->copies_ < COPIES && t >= COPY_MS * this->copies_) {
    this->transmit_();
    return;
  }
  if (t < SEND_MS)
    return;
  if (this->sends_ < SENDS) {
    this->new_send_();
  } else if (this->resync_) {
    this->finish_("no answer to a resync");
  } else if (!this->answered_ && !this->resynced_) {
    // maybe the station has a newer counter than mine: announce a new block, then send the command again
    this->resync_ = true;
    this->sends_ = 0;
    this->new_send_();
  } else if (!this->answered_) {
    this->finish_("no answer after 4 sends");
  } else if (this->cmd_ == CMD_AUTO || this->cmd_ == CMD_MANUAL) {
    this->finish_("mode unchanged after 4 sends");
  } else {
    this->finish_("confirmed");
  }
}

// x: frame bytes 2..37, as the chip handed them over
void ElsnerArexa::decode_(const uint8_t *x, float rssi, float freq_offset) {
  // frame bytes 0..37: the sync word ce ad, then x. A packet is used only with a matching CRC (bytes 36, 37 over
  // 0..35): the chip's Manchester decoder does not tell which bytes are broken. One wrong bit is repaired, but such
  // a packet only confirms a send (see below).
  uint8_t received[N] = {HEADER[0], HEADER[1]};
  std::copy(x, x + N - 2, received + 2);
  uint8_t c[N];
  std::copy(received, received + N, c);
  bool fixed;
  const bool crc_ok = crc_check_and_fix(c, &fixed);
  // noise often matches the sync word (about 20 times a minute); with the rest of the header too it is an Arexa packet
  if (c[2] != HEADER[2] || c[3] != HEADER[3])
    return;
  if (!crc_ok) {
    char hex[format_hex_size(N)];
    ESP_LOGD(TAG, "Packet with a wrong CRC, %.0f dBm: %s", rssi, format_hex_to(hex, c, N));
    this->publish_frame_("bad", rssi, received);
    return;
  }
  this->publish_frame_(fixed ? "repaired" : "ok", rssi, received);
  const uint8_t seed = c[4];
  uint8_t v[36] = {0};
  for (int k = 5; k < 36; k++)
    v[k] = c[k] - 2 * c[k - 1] - parity_bit(seed, k);
  const uint8_t sender = v[10];
  const char *from = !this->learned_                      ? "?"
                     : sender == this->rc_address_()      ? "RC"
                     : sender == this->station_address_() ? "station"
                                                         : "other";
  ESP_LOGD(TAG, "Packet from %s %02x: counter %04x, %.0f dBm%s", from, sender, counter_of(v), rssi,
           fixed ? ", one bit repaired" : "");

  // not from a repaired packet: a wrong pairing would stay until a reboot
  if (!fixed && is_rc_status(v) && v[19] == FILLER && v[20] == FILLER)
    this->learn_pairing_(v);
  if (!this->learned_)
    return;

  // only packets of my own pairing: the system ID is in bytes 8, 9 and again in 13, 14
  if (v[8] != this->pairing_[0] || v[9] != this->pairing_[1] || v[13] != this->pairing_[5] ||
      v[14] != this->pairing_[6]) {
    ESP_LOGD(TAG, "Not my system ID, dropped");
    return;
  }
  // A packet with 3 or more wrong bits can look like a repairable one (about 1 in 230), and then the repair makes it
  // wrong. So a repaired packet only confirms a send: that needs the station's address and the exact counter sent.
  if (fixed) {
    if (sender == this->station_address_())
      this->check_answer_(v);
    return;
  }

  this->take_counter_(v, sender == this->station_address_());

  // Value bytes 17..35: with the running sum u[k] = v[k] + 2 u[k-1] from byte 17 on, each value is a plain number
  // (see value16).
  uint8_t u[36] = {0};
  u[17] = v[17];
  for (int k = 18; k < 36; k++)
    u[k] = v[k] + 2 * u[k - 1];
  if (sender == this->rc_address_())
    this->decode_rc_(v, u, rssi, freq_offset);
  else if (sender == this->station_address_())
    this->decode_station_(v, u, rssi, freq_offset);
}

void ElsnerArexa::learn_pairing_(const uint8_t *v) {
#ifdef USE_TEXT_SENSOR
  // the pairing of the RC heard on the air (also with a fixed pairing: to see what arrives)
  uint8_t seen[10];
  std::copy(v + 8, v + 17, seen);
  seen[9] = counter_check_of(v);
  char hex[format_hex_size(10)];
  format_hex_to(hex, seen, 10);
  if (this->pairing_text_ != nullptr && this->pairing_seen_ != hex) {
    this->pairing_seen_ = hex;
    this->pairing_text_->publish_state(hex);
  }
#endif
  if (!this->learned_) {
    this->use_pairing_(v + 8);
    ESP_LOGI(TAG, "Learned my RC: system ID %02x%02x, RC address %02x, station address %02x", v[8], v[9], v[10], v[15]);
  }
}

// The counter (RC and station share it, README: Counter). The station takes a packet whose counter is newer (lower,
// mod 2^16) than its last one, and a resync sets it to any value: so its packets always bring the current counter,
// the RC's only a newer one (after a command, the RC repeats its stale status before it resyncs).
void ElsnerArexa::take_counter_(const uint8_t *v, bool from_station) {
  if (this->counter_check_ < 0)
    this->counter_check_ = counter_check_of(v);
  if (counter_check_of(v) != this->counter_check_) {
    ESP_LOGD(TAG, "Counter check %02x, not %02x: counter not taken", counter_check_of(v), this->counter_check_);
    return;
  }
  const uint16_t n = counter_of(v);
  if (!this->have_counter_ || from_station || static_cast<int16_t>(this->n_ - n) > 0) {
    this->n_ = n;
    this->have_counter_ = true;
  }
}

// The RC's packets: its status once a minute (with its temperature sensor) and the button presses.
void ElsnerArexa::decode_rc_(const uint8_t *v, const uint8_t *u, float rssi, float freq_offset) {
#ifdef USE_SENSOR
  if (this->rc_rssi_ != nullptr)
    this->rc_rssi_->publish_state(rssi);
  if (this->rc_freq_offset_ != nullptr)
    this->rc_freq_offset_->publish_state(freq_offset / 1000.0f);
#endif
  if (is_rc_status(v)) {  // with the RC's own temperature sensor
    const float inside = static_cast<int16_t>(value16(u, 22, 0x40, 0x80)) / 10.0f;
    if (inside > this->open_above_)
      this->warm_ = 1;
    else if (inside < this->open_above_ - HYSTERESIS)
      this->warm_ = 0;
    this->window_update_();
#ifdef USE_SENSOR
    if (this->inside_temperature_ != nullptr)
      this->inside_temperature_->publish_state(inside);
    if (this->raw_inside_ != nullptr)
      this->raw_inside_->publish_state(v[22]);
#endif
    return;
  }
  int cmd = -1;
  for (int i = 0; i <= CMD_MANUAL; i++)
    if (v[17] == COMMANDS[i].bytes[0] && v[18] == COMMANDS[i].bytes[1])
      cmd = i;
  if (cmd < 0)
    return;  // for example the start-up packet after a battery change
  // the RC repeats a command with the same counter until the station answers: one event per press
  const int counter = counter_of(v);
  if (cmd == this->last_remote_ && counter == this->last_remote_counter_)
    return;
  this->last_remote_ = cmd;
  this->last_remote_counter_ = counter;
  ESP_LOGD(TAG, "RC button: %s", COMMANDS[cmd].name);
  this->window_command_(cmd);
#ifdef USE_EVENT
  if (this->remote_ != nullptr)
    this->remote_->trigger(COMMANDS[cmd].name);
#endif
}

// The station's packets: an answer to every packet it takes, with its weather values and mode.
void ElsnerArexa::decode_station_(const uint8_t *v, const uint8_t *u, float rssi, float freq_offset) {
  this->check_answer_(v);
  // the rest is the station's status: not a resync answer or a start-up packet after a battery change (both have
  // 00/ff in bytes 31..35)
  if (v[31] != FILLER || v[32] != FILLER)
    return;
  const float outside = static_cast<int16_t>(value16(u, 17, 0x9a, 0x34)) / 10.0f;
  const float wind = value16(u, 19, 0x68, 0xd0) / 10.0f;
  if (outside < this->outside_lock_)
    this->locked_ = 1;
  else if (outside > this->outside_lock_ + HYSTERESIS)
    this->locked_ = 0;
  if (wind > this->wind_limit_) {
    this->windy_ = true;
    this->windy_ms_ = millis();
  }
  this->raining_ = u[21] < 0x50;    // 0x52 dry, 0x4e while the rain alarm holds
  this->wind_flag_ = u[27] != 0xb2;  // 0xb2 calm, 0xa9 while the wind alarm holds
#ifdef USE_TEXT_SENSOR
  if (this->station_raw_ != nullptr) {
    char hex[3 * 19];
    for (int k = 17; k < 36; k++)
      snprintf(hex + 3 * (k - 17), 4, k < 35 ? "%02x " : "%02x", v[k]);
    this->station_raw_->publish_state(hex);
  }
#endif
#ifdef USE_SENSOR
  if (this->station_rssi_ != nullptr)
    this->station_rssi_->publish_state(rssi);
  if (this->station_freq_offset_ != nullptr)
    this->station_freq_offset_->publish_state(freq_offset / 1000.0f);
  if (this->outside_temperature_ != nullptr)
    this->outside_temperature_->publish_state(outside);
  if (this->raw_outside_ != nullptr)
    this->raw_outside_->publish_state(v[17]);
  if (this->wind_ != nullptr)
    this->wind_->publish_state(wind);
  // about 1.05 lx per step (calibrated against the RC's klux display)
  const uint16_t bright = value16(u, 22, 0x3c, 0x80);
  if (this->brightness_ != nullptr)
    this->brightness_->publish_state(bright * (270.0f / 256.0f));
  if (this->raw_brightness_ != nullptr)
    this->raw_brightness_->publish_state(bright);
  if (this->rain_sensor_ != nullptr)
    this->rain_sensor_->publish_state(u[29]);  // 0xbc in every packet so far, rain or not
#endif
#ifdef USE_BINARY_SENSOR
  if (this->rain_ != nullptr)
    this->rain_->publish_state(this->raining_);
  if (this->wind_alarm_ != nullptr)
    this->wind_alarm_->publish_state(this->wind_flag_);
#endif
  this->publish_mode_(u[25]);
}

// The answer to the command being sent carries its counter. The station answers with its state from before the
// command, so a second send fetches the new mode: a mode command is done when an answer has the new mode, and open,
// close or stop in automatic mode (which switch the station to manual) send once more after the first answer.
void ElsnerArexa::check_answer_(const uint8_t *v) {
  if (this->cmd_ < 0 || counter_of(v) != this->sent_n_ || counter_check_of(v) != this->counter_check_)
    return;
  const bool resync_answer = v[11] == 0x4d && v[12] == 0x01;
  if (this->resync_ != resync_answer)
    return;
  if (this->resync_) {  // the station takes the new block: now the command
    ESP_LOGI(TAG, "Resync to counter %04x answered", this->sent_n_);
    this->n_ = this->sent_n_;
    this->have_counter_ = true;
    this->resync_ = false;
    this->resynced_ = true;
    this->sends_ = 0;
    this->new_send_();
    return;
  }
  const bool mode = this->cmd_ == CMD_AUTO || this->cmd_ == CMD_MANUAL;
  const bool done =
      mode ? v[25] == (this->cmd_ == CMD_AUTO ? MODE_AUTO : MODE_MANUAL) : this->answered_ || v[25] != MODE_AUTO;
  if (done) {
    // also from a repaired packet: it matched the exact counter sent (often the only answer with the new mode)
    const int cmd = this->cmd_;
    this->finish_("confirmed");
    if (mode)
      this->window_command_(cmd);  // before the mode: the window moves after the start delay
    this->publish_mode_(v[25]);
  } else {
    this->answered_ = true;
    this->copies_ = COPIES;  // no more copies of this send
  }
}

bool ElsnerArexa::send_command(Command cmd) {
  if (this->cmd_ >= 0) {
    std::string result = std::string("replaced by ") + COMMANDS[cmd].name;
    this->finish_(result.c_str());
  }
  this->cmd_ = cmd;
  if (!this->learned_) {
    this->finish_("not sent, pairing not known yet");
    return false;
  }
  if (!this->have_counter_ && this->counter_check_ < 0) {
    this->finish_("not sent, no packet heard yet");  // the counter's check byte is not known (README: Setup)
    return false;
  }
  this->sends_ = 0;
  this->answered_ = false;
  this->resynced_ = false;
  this->resync_ = !this->have_counter_;  // no counter heard since the start (for example without the RC): announce one
  this->new_send_();
  // the window moves from the first send on (the station mostly takes it, the answer is what gets lost); undone in
  // finish_ if the station never answers
  if (cmd == CMD_OPEN || cmd == CMD_STOP || cmd == CMD_CLOSE) {
    this->undo_pos_ = this->pos_;
    this->undo_dir_ = this->dir_;
    this->undo_ms_ = this->move_ms_;
    this->undo_target_ = this->target_;
    this->moved_ = true;
    this->window_command_(cmd);
  }
  return true;
}

void ElsnerArexa::finish_(const char *result) {
  const bool confirmed = strcmp(result, "confirmed") == 0;
  if (confirmed)
    ESP_LOGI(TAG, "%s: confirmed after %d send%s", COMMANDS[this->cmd_].name, this->sends_, this->sends_ == 1 ? "" : "s");
  else
    ESP_LOGW(TAG, "%s: %s", COMMANDS[this->cmd_].name, result);
  if (this->moved_ && !confirmed) {
    // the station never took it: back to the position as it was (and still is, if it was moving)
    this->pos_ = this->undo_pos_;
    this->dir_ = this->undo_dir_;
    this->move_ms_ = this->undo_ms_;
    this->target_ = this->undo_target_;
    this->publish_position_();
  }
  this->moved_ = false;
#ifdef USE_TEXT_SENSOR
  if (this->last_command_ != nullptr)
    this->last_command_->publish_state(std::string(COMMANDS[this->cmd_].name) + ": " + result);
#endif
  this->cmd_ = -1;
  this->resync_ = false;
}

// A fresh frame: the command with the next counter (the station takes a packet only if its counter is newer than the
// last one), or a resync request with a new random block.
void ElsnerArexa::new_send_() {
  const uint8_t seed = random_uint32() & 0xff;
  uint8_t value[36] = {0};
  std::copy(this->pairing_, this->pairing_ + 9, value + 8);
  if (this->resync_) {
    // README, Resync: any counter; bytes 17..31 a random tag, 32..35 and the lowest bit of byte 16 check bits of it
    // (the station does not check them, the RC always sends them). My counter changes only when the station answers.
    this->sent_n_ = random_uint32();
    value[11] = 0x82;
    value[12] = 0xff;
    const uint32_t tag = random_uint32();
    uint8_t b[36] = {0};
    for (int i = 17; i < 32; i++)
      b[i] = (tag >> (i - 17)) & 1;
    b[32] = b[17] ^ b[31];
    b[33] = b[32] ^ b[18];
    b[34] = b[33] ^ b[19];
    b[35] = b[34] ^ b[20];
    value[16] = 2 * seed + 0x53 + (b[30] ^ b[31] ^ 1);
    for (int i = 17; i < 36; i++)
      value[i] = b[i] ? 0xff : 0x00;
  } else {
    std::fill(value + 17, value + 36, FILLER);
    std::copy(COMMANDS[this->cmd_].bytes, COMMANDS[this->cmd_].bytes + 3, value + 17);
    this->n_ -= 2;
    this->sent_n_ = this->n_;
  }
  // the counter (README: Counter): n = k << 8 | (v5 - 1), v6 = k - 2 v5, v7 = check - 2 k
  const uint8_t v5 = (this->sent_n_ & 0xff) + 1;
  const uint8_t k = this->sent_n_ >> 8;
  value[5] = v5;
  value[6] = k - 2 * v5;
  value[7] = this->counter_check_ - 2 * k;
  uint8_t *c = this->frame_;
  std::copy(HEADER, HEADER + 4, c);
  c[4] = seed;
  for (int i = 5; i < 36; i++)
    c[i] = value[i] + parity_bit(seed, i) + 2 * c[i - 1];
  const uint16_t crc = crc16(c, 36);
  c[36] = crc >> 8;
  c[37] = crc & 0xff;
  this->publish_frame_("tx", NAN, c);
  this->sends_++;
  this->copies_ = 0;
  this->send_ms_ = millis();
  ESP_LOGD(TAG, "Sending %s with counter %04x (send %d)", this->resync_ ? "a resync" : COMMANDS[this->cmd_].name,
           this->sent_n_, this->sends_);
  this->transmit_();
}

// The station's mode byte
void ElsnerArexa::publish_mode_(uint8_t mode) {
  if (mode != MODE_MANUAL && mode != MODE_AUTO)
    return;
  const bool automatic = mode == MODE_AUTO;
  // the station answers a command with its mode from before it: for 5 s the command says which mode it is in
  if (millis() - this->command_ms_ > 5000)
    this->auto_mode_on_ = automatic;
  this->window_update_();
#ifdef USE_BINARY_SENSOR
  if (this->auto_mode_ != nullptr)
    this->auto_mode_->publish_state(automatic);
#endif
#ifdef USE_SWITCH
  if (this->auto_mode_switch_ != nullptr)
    this->auto_mode_switch_->publish_state(automatic);
#endif
}

// README: Window state. The station closes the window on a wind or rain alarm (if switched on in the RC's settings),
// also in manual mode, and ignores the buttons meanwhile. The wind alarm holds 5 minutes after the last gust above
// the limit; I see the wind only once a minute, so a short gust can close the window unseen.
bool ElsnerArexa::alarm_() {
  if (this->windy_ && millis() - this->windy_ms_ >= 5 * 60 * 1000)
    this->windy_ = false;
  return (this->rain_alarm_ && this->raining_) || (!std::isnan(this->wind_limit_) && (this->windy_ || this->wind_flag_));
}

// A command, from the gateway or the RC. An open, stop or close puts the station in manual mode. The motor starts
// start_delay after an open or close; a stop is at once. In automatic mode the station acts on its settings at once.
void ElsnerArexa::window_command_(int cmd) {
  this->command_ms_ = millis();
  if (cmd == CMD_AUTO || cmd == CMD_MANUAL) {
    this->auto_mode_on_ = cmd == CMD_AUTO;
    this->window_update_(this->start_delay_ms_);
    return;
  }
  this->auto_mode_on_ = 0;
  this->target_ = NAN;
  if (this->alarm_()) {
    this->set_window_(0);  // the station ignores the buttons
    return;
  }
  if (cmd == CMD_STOP) {
    this->pos_ = this->position_();
    this->dir_ = 0;
    this->publish_position_();
    return;
  }
  const int8_t dir = cmd == CMD_OPEN ? 1 : -1;
  if (dir != this->dir_)
    this->move_(dir, this->start_delay_ms_);
}

// After new values: the alarms, and in automatic mode the RC's settings A (open above the inside temperature, close
// 2 °C below it) and B (closed below the outside temperature, until 2 °C above it).
void ElsnerArexa::window_update_(uint32_t delay) {
  if (this->alarm_()) {
    this->set_window_(0, delay);
    return;
  }
  if (this->auto_mode_on_ != 1)
    return;  // manual mode: only the buttons move it
  if (this->locked_ == 1 || this->warm_ == 0)
    this->set_window_(0, delay);
  else if (this->locked_ == 0 && this->warm_ == 1)
    this->set_window_(1, delay);
  // else: not known yet (just started, between the two limits), or no temperature control: it stays as it is
}

// The station's own move, all the way: after its packet (seen up to a minute late) from now, after a mode command
// with the start delay.
void ElsnerArexa::set_window_(int8_t state, uint32_t delay) {
  const int8_t dir = state ? 1 : -1;
  const float p = this->position_();
  if (dir == this->dir_ || p == (state ? 1.0f : 0.0f))
    return;
  ESP_LOGD(TAG, "Window: the station %s it", state ? "opens" : "closes");
  this->target_ = NAN;
  this->move_(dir, delay);
}

// The position now: from pos_ at move_ms_, at the speed of the direction; NAN if not known.
float ElsnerArexa::position_() {
  if (std::isnan(this->pos_) || this->dir_ == 0)
    return this->pos_;
  const int32_t t = static_cast<int32_t>(millis() - this->move_ms_);
  if (t <= 0)
    return this->pos_;
  const float p = this->pos_ + this->dir_ * t / static_cast<float>(this->dir_ > 0 ? this->open_ms_ : this->close_ms_);
  return std::max(0.0f, std::min(1.0f, p));
}

// Start moving in direction dir, the motor after delay. From an unknown position: as if from the other end.
void ElsnerArexa::move_(int8_t dir, uint32_t delay) {
  float p = this->position_();
  if (std::isnan(p))
    p = dir > 0 ? 0.0f : 1.0f;
  this->pos_ = p;
  this->dir_ = (dir > 0 ? p < 1 : p > 0) ? dir : 0;
  this->move_ms_ = millis() + delay;
  this->publish_position_();
}

void ElsnerArexa::move_to(float position) {
  const float p = this->position_();
  const bool known = !std::isnan(p);
  // 0 and 1 (close and open): no stop at all, the motor runs to its own end; only a position in between stops on time
  const bool between = position > 0.0f && position < 1.0f;
  if (between && known && std::abs(position - p) < 0.02f)
    return;
  // from an unknown position: all the way to the nearer end
  const bool open = !between ? position >= 1.0f : known ? position > p : position > 0.5f;
  if (this->send_command(open ? CMD_OPEN : CMD_CLOSE) && between && known)
    this->target_ = position;  // (the command cleared it)
}

// The cover, the window text sensor (open: some or all the way) and, when still, the saved position.
void ElsnerArexa::publish_position_() {
  this->publish_ms_ = millis();
  const float p = this->position_();
  if (this->dir_ == 0 && !std::isnan(p))
    this->position_pref_.save(&p);
  const int8_t window = std::isnan(p) ? -1 : p > 0;
  if (window != this->window_) {
    this->window_ = window;
#ifdef USE_TEXT_SENSOR
    if (this->window_text_ != nullptr && window >= 0)
      this->window_text_->publish_state(window ? "open" : "closed");
#endif
  }
#ifdef USE_COVER
  if (this->cover_ != nullptr) {
    this->cover_->position = std::isnan(p) ? 0.5f : p;
    this->cover_->current_operation = this->dir_ > 0   ? cover::COVER_OPERATION_OPENING
                                      : this->dir_ < 0 ? cover::COVER_OPERATION_CLOSING
                                                       : cover::COVER_OPERATION_IDLE;
    this->cover_->publish_state(false);
  }
#endif
}

// For the last frame text sensor: "ok -62 cead93...", "tx cead93..." (see text_sensor.py)
void ElsnerArexa::publish_frame_(const char *kind, float rssi, const uint8_t *c) {
#ifdef USE_TEXT_SENSOR
  if (this->last_frame_ == nullptr)
    return;
  char text[16 + 2 * N + 1];
  const int len = std::isnan(rssi) ? snprintf(text, sizeof(text), "%s ", kind)
                                   : snprintf(text, sizeof(text), "%s %.0f ", kind, rssi);
  format_hex_to(text + len, sizeof(text) - len, c, N);
  this->last_frame_->publish_state(text);
#endif
}

// One copy of the frame. The chip sends it: packet mode with its own Manchester. The sync word 00 0a: the zero byte
// gives 16 alternating chips like the RC's preamble, 0a is the anchor. Then the 38 frame bytes.
void ElsnerArexa::transmit_() {
  auto *r = this->radio_;
  r->set_manchester(true);
  r->set_sync1(0x00);
  r->set_sync0(0x0a);
  r->set_packet_length(N);
  r->set_num_preamble(0);  // 2 bytes, the minimum
  r->set_fsk_deviation(40e3f);  // comes out at about the RC's +-35 kHz
  r->set_output_power(this->output_power_);
  const auto err = r->transmit_packet(this->frame_, N);
  if (err != cc1101::CC1101Error::NONE)
    ESP_LOGW(TAG, "transmit_packet failed: %d", (int) err);
  this->start_listening();
  this->copies_++;
}

void ElsnerArexa::dump_config() {
  ESP_LOGCONFIG(TAG, "Elsner Arexa:");
  ESP_LOGCONFIG(TAG, "  Frequency: %.3f MHz", this->frequency_ / 1e6f);
  ESP_LOGCONFIG(TAG, "  RX attenuation: %d dB", 6 * static_cast<int>(this->rx_attenuation_));
  if (this->learned_)
    ESP_LOGCONFIG(TAG, "  Pairing: system ID %02x%02x, RC address %02x, station address %02x (%s)", this->pairing_[0],
                  this->pairing_[1], this->rc_address_(), this->station_address_(), this->fixed_ ? "fixed" : "learned");
  else
    ESP_LOGCONFIG(TAG, "  RC not heard yet (it sends a status packet every minute)");
  ESP_LOGCONFIG(TAG, "  RC settings: open above %.1f °C, outside lock %.1f °C, wind alarm %.1f m/s, rain alarm %s",
                this->open_above_, this->outside_lock_, this->wind_limit_, YESNO(this->rain_alarm_));
  ESP_LOGCONFIG(TAG, "  Window: opens in %u ms, closes in %u ms, the motor starts %u ms after a command",
                (unsigned) this->open_ms_, (unsigned) this->close_ms_, (unsigned) this->start_delay_ms_);
}

}  // namespace esphome::elsner_arexa
