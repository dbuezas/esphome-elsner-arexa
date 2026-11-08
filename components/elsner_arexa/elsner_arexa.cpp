#include "elsner_arexa.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cmath>

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
static constexpr int N = 38;
// RC commands: values of bytes 17, 18, 19 (the same on a second installation's Solexa recordings)
static const uint8_t COMMANDS[5][3] = {
    {0x25, 0xba, 0x30},  // open
    {0x27, 0xb6, 0x30},  // stop
    {0x36, 0x98, 0x30},  // close
    {0x6a, 0x1f, 0x52},  // automatic mode
    {0x6a, 0x13, 0x6a},  // manual mode
};
// in the order of the Command enum
static const char *const COMMAND_NAMES[5] = {"open", "stop", "close", "auto", "manual"};

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

static uint8_t parity_bit(uint8_t seed, int k) { return __builtin_parity(seed & MASK[k]) ^ INV[k]; }

// The counter: 16 bits counting down, n = k << 8 | (v5 - 1) with k = v6 + 2 v5. Byte 7 holds a check: v7 + 2 k is
// the same in every packet of a system (f7 on mine, 48 on a Solexa). See the README, Counter.
static uint16_t counter_of(const uint8_t *v) {
  const uint8_t k = v[6] + 2 * v[5];
  return k << 8 | static_cast<uint8_t>(v[5] - 1);
}
static uint8_t counter_check_of(const uint8_t *v) { return v[7] + 2 * (v[6] + 2 * v[5]); }

// the pairing: pairing[0..8] = the values of bytes 8..16 of the RC's packets: 8, 9 the system ID (repeated in 13, 14), 10 the RC's
// address, 15 the station's address (the RC's packets go to the station), 11, 12, 16 the device type
void ElsnerArexa::use_pairing_(const uint8_t *pairing) {
  for (int i = 0; i < 9; i++)
    this->pairing_[i] = pairing[i];
  this->rc_address_ = pairing[2];
  this->station_address_ = pairing[7];
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
  this->start_listening();
  // the window's last state survives a reboot
  if (std::isnan(this->outside_lock_))
    this->locked_ = 0;
  this->window_pref_ = global_preferences->make_preference<int8_t>(fnv1_hash("elsner_arexa_window"));
  int8_t window;
  if (this->window_pref_.load(&window) && window >= 0 && window <= 1) {
    this->window_ = window;
#ifdef USE_TEXT_SENSOR
    if (this->window_text_ != nullptr)
      this->window_text_->publish_state(window ? "open" : "closed");
#endif
  }
  // The station answers the RC 14 ms after its packet, and the CC1101 driver listens again only when the main loop
  // has read the RC's packet. With the default 16 ms loop most station packets are missed.
  if (App.get_loop_interval() > 2)
    App.set_loop_interval(2);
}

// Receive settings. The chips are 10.000 us (100 kbaud), 2-FSK with ~+-35 kHz deviation, Manchester coded (1 = chips
// 10, 0 = 01); the chip decodes the Manchester. The preamble is only ~13 alternating chips, which fit both chip
// phases: the sync word, the first two frame bytes ce ad, fixes the phase. Then 36 bytes: frame bytes 2..37, with
// the CRC. Measured against raw chips decoded here (sync on the 16 chips of ce): 42% against 29% of the station's
// packets with a matching CRC (2026-11-04, 4 h alternating).
void ElsnerArexa::start_listening() {
  auto *r = this->radio_;
  r->set_frequency(this->frequency_);
  r->set_modulation_type(cc1101::Modulation::MODULATION_2_FSK);
  r->set_symbol_rate(100000);
  r->set_fsk_deviation(47.6e3f);
  r->set_filter_bandwidth(270e3f);
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
  r->set_max_dvga_gain(cc1101::MaxDvgaGain::MAX_DVGA_GAIN_MINUS_3);  // see fix_gain_
  this->fix_gain_();
}

// The gain is fixed (no AGC): 1 ms of receiving noise brings it to the maximum, then it is frozen. With the AGC
// (frozen on the sync word) the gain was still low from the RC's packet when the station answered 8 ms later, 16 dB
// weaker: 43% of its packets came with wrong bits, against 6% with the gain fixed (2026-11-06, 4 h alternating). FSK
// needs no gain control: the bits are in the frequency. Done again after each station packet (the next RC packet is
// a minute away), in case a signal was there while the gain settled.
// Without a limit the gain did not always settle at the same level (the RC's RSSI jumped between -61, -56 and
// -52 dBm from one fixing to the next), and the higher ones broke the station's packets: 22%, 62% and 90% of them
// with wrong bits. The DVGA's 3 highest steps are left out: then the noise brings it to the same level every time.
// Against no limit, with the gain fixed again every 5 min: 87% against 46% of the station's packets usable, 5
// against 46 lost (2026-11-08, 6 h alternating; 2 steps: 83%, LNA limits: no better).
void ElsnerArexa::fix_gain_() {
  auto *r = this->radio_;
  r->set_freeze(cc1101::Freeze::FREEZE_DEFAULT);
  r->begin_rx();
  delayMicroseconds(1000);
  r->set_freeze(cc1101::Freeze::FREEZE_ANALOG_AND_DIGITAL);
  r->begin_rx();
}

// Called by the CC1101 driver before it listens again: keep it short (the station answers the RC 14 ms later).
void ElsnerArexa::on_packet(const std::vector<uint8_t> &packet, float freq_offset, float rssi, uint8_t lqi) {
  if (this->queue_.size() < 8)
    this->queue_.push_back({packet, rssi, freq_offset});
}

void ElsnerArexa::loop() {
  for (auto &item : this->queue_)
    this->decode_(item.bytes, item.rssi, item.freq_offset);
  this->queue_.clear();
  if (this->cmd_ < 0)
    return;
  // the station answers 14 ms after a copy it takes; it ignores the later copies (same counter)
  const uint32_t t = millis() - this->send_ms_;
  if (this->copies_ < 3 && t >= 30u * this->copies_)
    this->transmit_();
  else if (t >= 300 && this->sends_ < 4)
    this->new_send_();
  else if (t >= 300 && this->resync_)
    this->finish_("no answer to a resync");
  else if (t >= 300 && !this->answered_ && !this->resynced_) {
    // maybe the station has a newer counter than mine: announce a new block, then send the command again
    this->resync_ = true;
    this->sends_ = 0;
    this->new_send_();
  } else if (t >= 300)
    this->finish_(!this->answered_                                       ? "no answer after 4 sends"
                  : this->cmd_ == CMD_AUTO || this->cmd_ == CMD_MANUAL ? "mode unchanged after 4 sends"
                                                                         : "confirmed");
}

void ElsnerArexa::decode_(const std::vector<uint8_t> &x, float rssi, float freq_offset) {
  // frame bytes 0..37: the sync word ce ad, then what the chip handed over. A packet is used only with a matching
  // CRC (bytes 36, 37 over 0..35): the chip's Manchester decoder does not tell which bytes are broken. One wrong bit
  // is repaired, but such a packet only confirms a send (see below).
  if (x.size() != N - 2)
    return;
  uint8_t c[N] = {HEADER[0], HEADER[1]};
  std::copy(x.begin(), x.end(), c + 2);
  uint8_t received[N];
  std::copy(c, c + N, received);
  bool fixed;
  if (!crc_check_and_fix(c, &fixed) || c[2] != HEADER[2] || c[3] != HEADER[3]) {
    // noise often matches the sync word (about once a second); with the rest of the header too it is an Arexa packet
    if (c[2] == HEADER[2] && c[3] == HEADER[3]) {
      ESP_LOGD(TAG, "Packet with a wrong CRC, %.0f dBm: %s", rssi, format_hex(c, N).c_str());
      this->publish_frame_("bad", rssi, received);
    }
    return;
  }
  this->publish_frame_(fixed ? "repaired" : "ok", rssi, received);
  const uint8_t seed = c[4];
  uint8_t v[36] = {0};
  for (int k = 5; k < 36; k++)
    v[k] = c[k] - 2 * c[k - 1] - parity_bit(seed, k);
  const uint8_t sender = v[10];
  const char *from = !this->learned_                      ? "?"
                     : sender == this->rc_address_      ? "RC"
                     : sender == this->station_address_ ? "station"
                                                         : "other";
  ESP_LOGD(TAG, "Packet from %s %02x: counter %04x, %.0f dBm%s", from, sender, counter_of(v), rssi,
           fixed ? ", one bit repaired" : "");

  // not from a repaired packet: a wrong pairing would stay until a reboot
  const bool rc_status = !fixed && v[17] == 0x6a && v[18] == 0x30 && v[19] == 0x30 && v[20] == 0x30;
  if (rc_status) {
#ifdef USE_TEXT_SENSOR
    // the pairing of the RC heard on the air (also with a fixed pairing: to see what arrives)
    char hex[21];
    for (int i = 0; i < 9; i++)
      snprintf(hex + 2 * i, 3, "%02x", v[8 + i]);
    snprintf(hex + 18, 3, "%02x", counter_check_of(v));
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
    if (sender == this->station_address_)
      this->check_answer_(v);
    return;
  }

  // The counter (RC and station share it, README: Counter). The station takes a packet whose counter is newer (lower,
  // mod 2^16) than its last one, and a resync sets it to any value: so its packets always bring the current counter,
  // the RC's only a newer one (after a command, the RC repeats its stale status before it resyncs).
  if (this->counter_check_ < 0)
    this->counter_check_ = counter_check_of(v);
  if (counter_check_of(v) != this->counter_check_) {
    ESP_LOGD(TAG, "Counter check %02x, not %02x: counter not taken", counter_check_of(v), this->counter_check_);
  } else {
    const uint16_t n = counter_of(v);
    if (!this->have_counter_ || sender == this->station_address_ || static_cast<int16_t>(this->n_ - n) > 0) {
      this->n_ = n;
      this->have_counter_ = true;
    }
  }

  // Value bytes 17..35: with the running sum u[k] = v[k] + 2 u[k-1] from byte 17 on, each value is a plain number;
  // 16 bits: (b0 - u[k]) | (b1 - u[k+1]) << 8, with bases per value (README: Values).
  uint8_t u[36] = {0};
  u[17] = v[17];
  for (int k = 18; k < 36; k++)
    u[k] = v[k] + 2 * u[k - 1];
  auto value16 = [&u](int k, uint8_t b0, uint8_t b1) -> uint16_t {
    return static_cast<uint8_t>(b0 - u[k]) | static_cast<uint8_t>(b1 - u[k + 1]) << 8;
  };

  if (sender == this->rc_address_) {
#ifdef USE_SENSOR
    if (this->rc_rssi_ != nullptr)
      this->rc_rssi_->publish_state(rssi);
    if (this->rc_freq_offset_ != nullptr)
      this->rc_freq_offset_->publish_state(freq_offset / 1000.0f);
#endif
    if (v[17] == 0x6a && v[18] == 0x30) {  // status: the RC's own temperature sensor
      const float inside = static_cast<int16_t>(value16(22, 0x40, 0x80)) / 10.0f;
      if (inside > this->open_above_)
        this->warm_ = 1;
      else if (inside < this->open_above_ - 2)
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
    for (int i = 0; i < 5; i++)
      if (v[17] == COMMANDS[i][0] && v[18] == COMMANDS[i][1])
        cmd = i;
    if (cmd < 0)
      return;  // for example the start-up packet after a battery change
    // the RC repeats a command with the same counter until the station answers: one event per press
    const int counter = counter_of(v);
    if (cmd == this->last_remote_ && counter == this->last_remote_counter_)
      return;
    this->last_remote_ = cmd;
    this->last_remote_counter_ = counter;
    ESP_LOGD(TAG, "RC button: %s", COMMAND_NAMES[cmd]);
    this->window_command_(cmd);
#ifdef USE_EVENT
    if (this->remote_ != nullptr)
      this->remote_->trigger(COMMAND_NAMES[cmd]);
#endif
  } else if (sender == this->station_address_) {
    this->check_answer_(v);
    if (this->cmd_ < 0)
      this->fix_gain_();
    // the rest is the station's status: not a resync answer or a start-up packet after a battery change (both have
    // 00/ff in bytes 31..35)
    if (v[31] != 0x30 || v[32] != 0x30)
      return;
    const float outside = static_cast<int16_t>(value16(17, 0x9a, 0x34)) / 10.0f;
    const float wind = value16(19, 0x68, 0xd0) / 10.0f;
    if (outside < this->outside_lock_)
      this->locked_ = 1;
    else if (outside > this->outside_lock_ + 2)
      this->locked_ = 0;
    if (wind > this->wind_limit_) {
      this->windy_ = true;
      this->windy_ms_ = millis();
    }
    this->raining_ = u[21] < 0x50;    // 0x52 dry, 0x4e while the rain alarm holds
    this->wind_flag_ = u[28] != 0x8b;  // 0x8b calm
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
    const uint16_t bright = value16(22, 0x3c, 0x80);
    if (this->brightness_ != nullptr)
      this->brightness_->publish_state(bright * (270.0f / 256.0f));
    if (this->raw_brightness_ != nullptr)
      this->raw_brightness_->publish_state(bright);
    if (this->rain_sensor_ != nullptr)
      this->rain_sensor_->publish_state(u[29]);  // 0xbc dry, lower = wetter
#endif
#ifdef USE_BINARY_SENSOR
    if (this->rain_ != nullptr)
      this->rain_->publish_state(this->raining_);
    if (this->wind_alarm_ != nullptr)
      this->wind_alarm_->publish_state(this->wind_flag_);
#endif
    this->publish_mode_(u[25]);
  }
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
  const bool done = mode ? v[25] == (this->cmd_ == CMD_AUTO ? 0xbf : 0xb3) : this->answered_ || v[25] != 0xbf;
  if (done) {
    // also from a repaired packet: it matched the exact counter sent (often the only answer with the new mode)
    this->publish_mode_(v[25]);
    this->finish_("confirmed");
  } else {
    this->answered_ = true;
    this->copies_ = 3;
  }
}

bool ElsnerArexa::send_command(Command cmd) {
  if (this->cmd_ >= 0) {
    std::string result = std::string("replaced by ") + COMMAND_NAMES[cmd];
    this->finish_(result.c_str());
  }
  if (!this->learned_) {
    this->cmd_ = cmd;
    this->finish_("not sent, pairing not known yet");
    return false;
  }
  this->cmd_ = cmd;
  this->sends_ = 0;
  this->answered_ = false;
  this->resynced_ = false;
  if (!this->have_counter_ && this->counter_check_ < 0) {
    this->finish_("not sent, no packet heard yet");  // the counter's check byte is not known (README: Setup)
    return false;
  }
  this->resync_ = !this->have_counter_;  // no counter heard since the start (for example without the RC): announce one
  this->new_send_();
  return true;
}

void ElsnerArexa::finish_(const char *result) {
  if (strcmp(result, "confirmed") == 0)
    ESP_LOGI(TAG, "%s: confirmed after %d send%s", COMMAND_NAMES[this->cmd_], this->sends_, this->sends_ == 1 ? "" : "s");
  else
    ESP_LOGW(TAG, "%s: %s", COMMAND_NAMES[this->cmd_], result);
  if (strcmp(result, "confirmed") == 0)
    this->window_command_(this->cmd_);
#ifdef USE_TEXT_SENSOR
  if (this->last_command_ != nullptr)
    this->last_command_->publish_state(std::string(COMMAND_NAMES[this->cmd_]) + ": " + result);
#endif
  this->cmd_ = -1;
  this->resync_ = false;
}

// A fresh frame: the command with the next counter (the station takes a packet only if its counter is newer than the
// last one), or a resync request with a new random block.
void ElsnerArexa::new_send_() {
  const uint8_t seed = random_uint32() & 0xff;
  uint8_t value[36] = {0};
  for (int i = 0; i < 9; i++)
    value[8 + i] = this->pairing_[i];
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
    for (int i = 17; i < 36; i++)
      value[i] = 0x30;
    for (int i = 0; i < 3; i++)
      value[17 + i] = COMMANDS[this->cmd_][i];
    this->n_ -= 2;
    this->sent_n_ = this->n_;
  }
  // the counter (README: Counter): n = k << 8 | (v5 - 1), v6 = k - 2 v5, v7 = check - 2 k
  const uint8_t v5 = (this->sent_n_ & 0xff) + 1;
  const uint8_t k = this->sent_n_ >> 8;
  value[5] = v5;
  value[6] = k - 2 * v5;
  value[7] = this->counter_check_ - 2 * k;
  auto &c = this->frame_;
  c = {0xce, 0xad, 0x93, 0xba, seed};
  for (int i = 5; i < 36; i++)
    c.push_back(value[i] + parity_bit(seed, i) + 2 * c[i - 1]);
  const uint16_t crc = crc16(c.data(), 36);
  c.push_back(crc >> 8);
  c.push_back(crc & 0xff);
  this->publish_frame_("tx", NAN, c.data());
  this->sends_++;
  this->copies_ = 0;
  this->send_ms_ = millis();
  ESP_LOGD(TAG, "Sending %s with counter %04x (send %d)", this->resync_ ? "a resync" : COMMAND_NAMES[this->cmd_],
           this->sent_n_, this->sends_);
  this->transmit_();
}

// The station's mode byte: 0xb3 manual, 0xbf automatic
void ElsnerArexa::publish_mode_(uint8_t mode) {
  if (mode != 0xb3 && mode != 0xbf)
    return;
  // the station answers a command with its mode from before it: after an open, stop or close it is in manual mode
  if (mode == 0xb3 || millis() - this->command_ms_ > 5000)
    this->auto_mode_on_ = mode == 0xbf;
  this->window_update_();
#ifdef USE_BINARY_SENSOR
  if (this->auto_mode_ != nullptr)
    this->auto_mode_->publish_state(mode == 0xbf);
#endif
#ifdef USE_SWITCH
  if (this->auto_mode_switch_ != nullptr)
    this->auto_mode_switch_->publish_state(mode == 0xbf);
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

// A confirmed open, stop or close, from the gateway or the RC. Each one puts the station in manual mode.
void ElsnerArexa::window_command_(int cmd) {
  if (cmd != CMD_OPEN && cmd != CMD_STOP && cmd != CMD_CLOSE)
    return;
  const uint32_t now = millis();
  this->command_ms_ = now;
  this->auto_mode_on_ = 0;
  if (this->alarm_()) {
    this->set_window_(0);
    return;
  }
  if (cmd == CMD_STOP) {
    // a stop while the window closes leaves it partly open; any other stop changes nothing
    if (this->last_move_ == CMD_CLOSE && now - this->last_move_ms_ < this->travel_ms_)
      this->set_window_(1);
    this->last_move_ = CMD_STOP;
    return;
  }
  const int8_t state = cmd == CMD_OPEN;
  if (this->window_ != state) {  // a move (not a close when it is closed already)
    this->last_move_ = cmd;
    this->last_move_ms_ = now;
  }
  this->set_window_(state);
}

// After new values: the alarms, and in automatic mode the RC's settings A (open above the inside temperature, close
// 2 °C below it) and B (closed below the outside temperature, until 2 °C above it).
void ElsnerArexa::window_update_() {
  if (this->alarm_()) {
    this->set_window_(0);
    return;
  }
  if (this->auto_mode_on_ != 1)
    return;  // manual mode: only the buttons move it
  if (this->locked_ == 1 || this->warm_ == 0)
    this->set_window_(0);
  else if (this->locked_ == 0 && this->warm_ == 1)
    this->set_window_(1);
  // else: not known yet (just started, between the two limits), or no temperature control: it stays as it is
}

void ElsnerArexa::set_window_(int8_t state) {
  if (state == this->window_)
    return;
  ESP_LOGD(TAG, "Window: %s", state ? "open" : "closed");
  if (this->window_ >= 0) {  // a move by the station; from unknown it may have been there already
    this->last_move_ = state ? CMD_OPEN : CMD_CLOSE;
    this->last_move_ms_ = millis();
  }
  this->window_ = state;
  this->window_pref_.save(&state);
#ifdef USE_TEXT_SENSOR
  if (this->window_text_ != nullptr)
    this->window_text_->publish_state(state ? "open" : "closed");
#endif
}

// For the last frame text sensor: "ok -62 cead93...", "tx cead93..." (see text_sensor.py)
void ElsnerArexa::publish_frame_(const char *kind, float rssi, const uint8_t *c) {
#ifdef USE_TEXT_SENSOR
  if (this->last_frame_ == nullptr)
    return;
  char text[16 + 2 * N + 1];
  int len = std::isnan(rssi) ? snprintf(text, sizeof(text), "%s ", kind)
                             : snprintf(text, sizeof(text), "%s %.0f ", kind, rssi);
  for (int i = 0; i < N; i++)
    len += snprintf(text + len, sizeof(text) - len, "%02x", c[i]);
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
  r->set_packet_length(this->frame_.size());
  r->set_num_preamble(0);  // 2 bytes, the minimum
  r->set_fsk_deviation(40e3f);  // comes out at about the RC's +-35 kHz
  r->set_output_power(this->output_power_);
  const auto err = r->transmit_packet(this->frame_);
  if (err != cc1101::CC1101Error::NONE)
    ESP_LOGW(TAG, "transmit_packet failed: %d", (int) err);
  this->start_listening();
  this->copies_++;
}

void ElsnerArexa::dump_config() {
  ESP_LOGCONFIG(TAG, "Elsner Arexa:");
  ESP_LOGCONFIG(TAG, "  Frequency: %.3f MHz", this->frequency_ / 1e6f);
  if (this->learned_)
    ESP_LOGCONFIG(TAG, "  Pairing: system ID %02x%02x, RC address %02x, station address %02x (%s)", this->pairing_[0],
                  this->pairing_[1],
                  this->rc_address_, this->station_address_, this->fixed_ ? "fixed" : "learned");
  else
    ESP_LOGCONFIG(TAG, "  RC not heard yet (it sends a status packet every minute)");
  ESP_LOGCONFIG(TAG, "  RC settings: open above %.1f °C, outside lock %.1f °C, wind alarm %.1f m/s, rain alarm %s",
                this->open_above_, this->outside_lock_, this->wind_limit_, YESNO(this->rain_alarm_));
  ESP_LOGCONFIG(TAG, "  Travel time: %u ms", (unsigned) this->travel_ms_);
}

}  // namespace esphome::elsner_arexa
