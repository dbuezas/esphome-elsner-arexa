#include "elsner_arexa.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include <algorithm>

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

// A temperature in tenths of a degree: a = base - low, b = 2 * low + sign (base 168 for the RC, 154 for the
// station). The RC also sends the high byte: byte 24 = 0xd0 + 2 * high; the station only has the sign bit.
static float temperature(uint8_t a, uint8_t b, uint8_t base) {
  uint8_t low = base - a;
  return (low - ((b & 1) ? 256 : 0)) / 10.0f;
}
static float temperature_with_high(uint8_t a, uint8_t high_byte, uint8_t base) {
  uint8_t low = base - a;
  int8_t high = static_cast<int8_t>(high_byte - 0xd0) / 2;
  return (high * 256 + low) / 10.0f;
}

// the pairing: pairing[0..8] = the values of bytes 8..16 of the RC's packets: 8, 9 the system ID (repeated in 13, 14), 10 the RC's
// address, 15 the station's address (the RC's packets go to the station), 11, 12, 16 the device type
void ElsnerArexa::use_pairing_(const uint8_t *pairing) {
  for (int i = 0; i < 9; i++)
    this->pairing_[i] = pairing[i];
  this->rc_address_ = pairing[2];
  this->station_address_ = pairing[7];
  this->learned_ = true;
}

void ElsnerArexa::set_fixed_pairing(const std::vector<uint8_t> &pairing) {
  if (pairing.size() != 9)
    return;
  this->use_pairing_(pairing.data());
  this->fixed_ = true;
}

void ElsnerArexa::setup() {
  if (this->radio_->is_failed()) {
    this->mark_failed();
    return;
  }
  this->radio_->register_listener(this);
  this->start_listening();
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
  // keep the gain from the sync word on: gain steps during the packet broke the station's weaker packets into
  // bursts of wrong chips
  r->set_freeze(cc1101::Freeze::FREEZE_ON_SYNC);
  r->set_crc_enable(false);  // the Arexa's CRC is not the chip's: checked in decode_
  r->set_whitening(false);
  r->set_packet_length(N - 2);
  r->set_packet_mode(true);
  r->set_output_power(this->output_power_);
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
  else if (t >= 300)
    this->finish_(this->answered_ ? "mode unchanged after 4 sends" : "no answer after 4 sends");
}

void ElsnerArexa::decode_(const std::vector<uint8_t> &x, float rssi, float freq_offset) {
  // frame bytes 0..37: the sync word ce ad, then what the chip handed over. A packet is used only with a matching
  // CRC (bytes 36, 37 over 0..35): the chip's Manchester decoder does not tell which bytes are broken. One wrong bit
  // is repaired, but such a packet only confirms a send (see below).
  if (x.size() != N - 2)
    return;
  uint8_t c[N] = {HEADER[0], HEADER[1]};
  std::copy(x.begin(), x.end(), c + 2);
  bool fixed;
  if (!crc_check_and_fix(c, &fixed) || c[2] != HEADER[2] || c[3] != HEADER[3]) {
    // noise often matches the sync word (about once a second); with the rest of the header too it is an Arexa packet
    if (c[2] == HEADER[2] && c[3] == HEADER[3])
      ESP_LOGD(TAG, "Packet with a wrong CRC, %.0f dBm: %s", rssi, format_hex(c, N).c_str());
    return;
  }
  const uint8_t seed = c[4];
  uint8_t v[36] = {0};
  for (int k = 5; k < 36; k++)
    v[k] = c[k] - 2 * c[k - 1] - parity_bit(seed, k);
  const uint8_t sender = v[10];
  const char *from = !this->learned_                      ? "?"
                     : sender == this->rc_address_      ? "RC"
                     : sender == this->station_address_ ? "station"
                                                         : "other";
  ESP_LOGD(TAG, "Packet from %s %02x: counter %02x %02x k %02x, %.0f dBm%s", from, sender, v[5], v[7],
           static_cast<uint8_t>(v[6] + 2 * v[5]), rssi, fixed ? ", one bit repaired" : "");

  // not from a repaired packet: a wrong pairing would stay until a reboot
  const bool rc_status = !fixed && v[17] == 0x6a && v[18] == 0x30 && v[19] == 0x30 && v[20] == 0x30;
  if (rc_status) {
#ifdef USE_TEXT_SENSOR
    // the pairing of the RC heard on the air (also with a fixed pairing: to see what arrives)
    char hex[19];
    for (int i = 0; i < 9; i++)
      snprintf(hex + 2 * i, 3, "%02x", v[8 + i]);
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

  // the counter (RC and station share it): v5 counts down, v6 = k - 2 v5, v7 changes rarely. Keep the newest one: after
  // a command, the RC's next status has an older counter (the station ignores it, the RC shows ER and resyncs; a
  // resync starts a new v7). A send with that older counter would be ignored too.
  if (this->v5_ < 0 || v[7] != this->v7_ || static_cast<int8_t>(v[5] - this->v5_) < 0) {
    this->v5_ = v[5];
    this->v7_ = v[7];
    this->k_ = v[6] + 2 * v[5];
  }

  if (sender == this->rc_address_) {
#ifdef USE_SENSOR
    if (this->rc_rssi_ != nullptr)
      this->rc_rssi_->publish_state(rssi);
    if (this->rc_freq_offset_ != nullptr)
      this->rc_freq_offset_->publish_state(freq_offset / 1000.0f);
#endif
    if (v[17] == 0x6a && v[18] == 0x30) {  // status: the RC's own temperature sensor
#ifdef USE_SENSOR
      // byte 23 repeats the low byte (2 * low + sign), like byte 18 for the station
      const bool pair = ((v[23] ^ static_cast<uint8_t>(2 * static_cast<uint8_t>(168 - v[22]))) & 0xfe) == 0;
      if (!pair)
        ESP_LOGW(TAG, "Inside temperature: bytes 22..24 %02x %02x %02x do not pass the pair check", v[22], v[23], v[24]);
      if (this->inside_temperature_ != nullptr)
        this->inside_temperature_->publish_state(temperature_with_high(v[22], v[24], 168));
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
    const int counter = v[7] << 8 | v[5];
    if (cmd == this->last_remote_ && counter == this->last_remote_counter_)
      return;
    this->last_remote_ = cmd;
    this->last_remote_counter_ = counter;
    ESP_LOGD(TAG, "RC button: %s", COMMAND_NAMES[cmd]);
#ifdef USE_EVENT
    if (this->remote_ != nullptr)
      this->remote_->trigger(COMMAND_NAMES[cmd]);
#endif
  } else if (sender == this->station_address_ && v[31] == 0x30 && v[32] == 0x30) {
    this->check_answer_(v);
    // the station's status (the start-up packets after a battery change have 00/ff in bytes 31..35)
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
    // byte 18 repeats the temperature (2 * low + sign)
    const bool outside_ok = ((v[18] ^ static_cast<uint8_t>(2 * static_cast<uint8_t>(154 - v[17]))) & 0xfe) == 0;
    // the decoded range is -25.6 to +25.5 degrees; beyond it the encoding is unknown: report the bytes, publish nothing
    if (!outside_ok)
      ESP_LOGW(TAG, "Outside temperature: bytes 17, 18 %02x %02x do not pass the pair check (out of the known range?)",
               v[17], v[18]);
    if (outside_ok && this->outside_temperature_ != nullptr)
      this->outside_temperature_->publish_state(temperature(v[17], v[18], 154));
    if (this->raw_outside_ != nullptr)
      this->raw_outside_->publish_state(v[17]);
    if (static_cast<uint8_t>(2 * v[19] + v[20]) == 0 && this->wind_ != nullptr)
      this->wind_->publish_state(static_cast<uint8_t>(-v[19]) / 10.0f);  // a pair like the temperatures, base 0
    if (this->brightness_ != nullptr)
      this->brightness_->publish_state(v[24] <= 2 ? 0 : (v[24] - 2) * 135.0f);
    if (this->raw_brightness_ != nullptr)
      this->raw_brightness_->publish_state(v[24]);
    if (this->rain_sensor_ != nullptr)
      this->rain_sensor_->publish_state(v[29]);
#endif
#ifdef USE_BINARY_SENSOR
    if (this->rain_ != nullptr)
      this->rain_->publish_state(v[21] < 0xb0);  // 0xb2 dry, 0xae while the rain alarm holds
    if (this->wind_alarm_ != nullptr)
      this->wind_alarm_->publish_state(v[28] != 0x27);  // 0x27 calm
#endif
    if (v[25] == 0xb3 || v[25] == 0xbf) {
      const bool automatic = v[25] == 0xbf;  // 0xb3 manual, 0xbf automatic
#ifdef USE_BINARY_SENSOR
      if (this->auto_mode_ != nullptr)
        this->auto_mode_->publish_state(automatic);
#endif
#ifdef USE_SWITCH
      if (this->auto_mode_switch_ != nullptr)
        this->auto_mode_switch_->publish_state(automatic);
#endif
    }
  }
}

// The answer to the command being sent carries its counter. A mode command is done when the answer has the new mode:
// the station answers with its state from before the command, so that takes a second send.
void ElsnerArexa::check_answer_(const uint8_t *v) {
  if (this->cmd_ < 0 || v[5] != this->sent_v5_ || v[7] != this->sent_v7_)
    return;
  const bool mode = this->cmd_ == CMD_AUTO || this->cmd_ == CMD_MANUAL;
  if (!mode || v[25] == (this->cmd_ == CMD_AUTO ? 0xbf : 0xb3)) {
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
  if (!this->learned_ || this->v5_ < 0) {
    this->cmd_ = cmd;
    this->finish_("not sent, no counter heard yet");
    return false;
  }
  this->cmd_ = cmd;
  this->sends_ = 0;
  this->answered_ = false;
  this->new_send_();
  return true;
}

void ElsnerArexa::finish_(const char *result) {
  if (strcmp(result, "confirmed") == 0)
    ESP_LOGI(TAG, "%s: confirmed after %d send%s", COMMAND_NAMES[this->cmd_], this->sends_, this->sends_ == 1 ? "" : "s");
  else
    ESP_LOGW(TAG, "%s: %s", COMMAND_NAMES[this->cmd_], result);
#ifdef USE_TEXT_SENSOR
  if (this->last_command_ != nullptr)
    this->last_command_->publish_state(std::string(COMMAND_NAMES[this->cmd_]) + ": " + result);
#endif
  this->cmd_ = -1;
}

// A fresh frame with the next counter (the station takes a packet only if its counter is newer than the last one)
void ElsnerArexa::new_send_() {
  uint8_t value[36] = {0};
  for (int k = 17; k < 36; k++)
    value[k] = 0x30;
  for (int i = 0; i < 9; i++)
    value[8 + i] = this->pairing_[i];
  for (int i = 0; i < 3; i++)
    value[17 + i] = COMMANDS[this->cmd_][i];
  // a new block starts at v5 = 00: v7 + 2 and k - 1 (the RC: 01 17 k 70, then 00 19 k 6f, ff 19 k 6f)
  const uint8_t v5 = this->v5_ - 2;
  if (this->v5_ == 1 || this->v5_ == 2) {
    this->v7_ += 2;
    this->k_ -= 1;
  }
  this->v5_ = v5;
  this->sent_v5_ = v5;
  this->sent_v7_ = this->v7_;
  value[5] = v5;
  value[6] = this->k_ - 2 * v5;
  value[7] = this->v7_;
  const uint8_t seed = random_uint32() & 0xff;
  auto &c = this->frame_;
  c = {0xce, 0xad, 0x93, 0xba, seed};
  for (int k = 5; k < 36; k++)
    c.push_back(value[k] + parity_bit(seed, k) + 2 * c[k - 1]);
  const uint16_t crc = crc16(c.data(), 36);
  c.push_back(crc >> 8);
  c.push_back(crc & 0xff);
  this->sends_++;
  this->copies_ = 0;
  this->send_ms_ = millis();
  ESP_LOGD(TAG, "Sending %s with counter %02x %02x k %02x (send %d)", COMMAND_NAMES[this->cmd_], v5, this->v7_, this->k_,
           this->sends_);
  this->transmit_();
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
}

}  // namespace esphome::elsner_arexa
