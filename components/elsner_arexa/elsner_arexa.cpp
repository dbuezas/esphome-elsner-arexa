#include "elsner_arexa.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"

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
// RC commands: values of bytes 17, 18, 19 (the same on a second installation's Solexa recordings)
static const uint8_t COMMANDS[5][3] = {
    {0x25, 0xba, 0x30},  // open
    {0x27, 0xb6, 0x30},  // stop
    {0x36, 0x98, 0x30},  // down
    {0x6a, 0x1f, 0x52},  // automatic mode
    {0x6a, 0x13, 0x6a},  // manual mode
};
static const char *const COMMAND_NAMES[5] = {"open", "stop", "down", "auto", "manual"};

static uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0x68b3;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i] << 8;
    for (int k = 0; k < 8; k++)
      crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
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

// system[0..8] = the values of bytes 8..16 of the RC's packets: 8, 9 the system ID (repeated in 13, 14), 10 the RC's
// address, 15 the station's address (the RC's packets go to the station), 11, 12, 16 the device type
void ElsnerArexa::use_system_(const uint8_t *system) {
  for (int i = 0; i < 9; i++)
    this->system_[i] = system[i];
  this->rc_address_ = system[2];
  this->station_address_ = system[7];
  this->learned_ = true;
}

void ElsnerArexa::set_fixed_system(const std::vector<uint8_t> &system) {
  if (system.size() != 9)
    return;
  this->use_system_(system.data());
  this->fixed_ = true;
}

void ElsnerArexa::setup() {
  this->radio_->register_listener(this);
  this->start_listening();
  // The station answers the RC 14 ms after its packet, and the CC1101 driver listens again only when the main loop
  // has read the RC's packet. With the default 16 ms loop most station packets are missed.
  if (App.get_loop_interval() > 2)
    App.set_loop_interval(2);
}

// Receive settings. The chips are 10.000 us (100 kbaud), 2-FSK with ~+-35 kHz deviation, Manchester coded (1 = chips
// 10, 0 = 01). The preamble is only ~13 alternating chips, which fit both chip phases, so the chip's own Manchester
// decoder often locks on the wrong phase: it hands over raw chips instead. The sync word is the 16 chips of the first
// frame byte 0xce, which fixes the phase. 64 FIFO bytes = 512 chips = frame bytes 1..32 (the CRC does not fit).
void ElsnerArexa::start_listening() {
  auto *r = this->radio_;
  r->set_frequency(this->frequency_);
  r->set_modulation_type(cc1101::Modulation::MODULATION_2_FSK);
  r->set_symbol_rate(100000);
  r->set_fsk_deviation(47.6e3f);
  r->set_filter_bandwidth(270e3f);
  r->set_manchester(false);
  r->set_sync_mode(cc1101::SyncMode::SYNC_MODE_16_16);
  r->set_sync1(0xA5);
  r->set_sync0(0xA9);
  r->set_num_preamble(2);
  r->set_foc_bs_cs_gate(false);  // the preamble is short: correct frequency and bit timing at once
  r->set_wait_time(cc1101::WaitTime::WAIT_TIME_8_SAMPLES);
  r->set_bs_limit(cc1101::BsLimit::BS_LIMIT_3P125_PERCENT);  // follow a small chip rate error (the station)
  r->set_crc_enable(false);
  r->set_whitening(false);
  r->set_packet_length(64);
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
    this->decode_(item.chips, item.rssi, item.freq_offset);
  this->queue_.clear();
}

void ElsnerArexa::decode_(const std::vector<uint8_t> &x, float rssi, float freq_offset) {
  // Manchester in software. A broken chip pair (10 or 01 expected) marks its byte as bad, and decoding goes on:
  // the station's packets often have a glitch somewhere and still carry clean fields after it.
  uint8_t c[33] = {0xce};
  bool bad[33] = {false};
  int first_bad = 33;
  for (size_t k = 0; k + 1 < x.size() * 8 && k / 16 < 32; k += 2) {
    const int a = (x[k >> 3] >> (7 - (k & 7))) & 1, b = (x[(k + 1) >> 3] >> (7 - ((k + 1) & 7))) & 1;
    const size_t i = k / 2, byte = 1 + i / 8;
    if (a == b && !bad[byte]) {
      bad[byte] = true;
      if (byte < static_cast<size_t>(first_bad))
        first_bad = byte;
    }
    c[byte] |= a << (7 - i % 8);
  }
  // no CRC (it is past the end of the FIFO): the 32-bit header and the seed byte must be clean
  if (first_bad <= 4 || c[1] != 0xad || c[2] != 0x93 || c[3] != 0xba)
    return;
  const uint8_t seed = c[4];
  uint8_t v[33] = {0};
  for (int k = 5; k < 33; k++)
    v[k] = c[k] - 2 * c[k - 1] - parity_bit(seed, k);
  // value k needs bytes k and k-1. Bytes after a glitch count only if the frame is still in step there: a known
  // filler byte (0x30) near the end must have arrived clean (after a lost chip, everything later is shifted).
  bool in_step = first_bad == 33;
  for (int k = 25; k < 33 && !in_step; k++)
    in_step = k > first_bad && !bad[k] && !bad[k - 1] && v[k] == 0x30;
  bool ok[33] = {false};
  for (int k = 5; k < 33; k++)
    ok[k] = !bad[k] && !bad[k - 1] && (k < first_bad || in_step);
  auto have = [&ok](int from, int to) {
    for (int k = from; k <= to; k++)
      if (!ok[k])
        return false;
    return true;
  };
  if (!have(5, 16))
    return;  // the counter and the addresses

  // the counter (RC and station share it): v5 counts down, v6 = k - 2 v5, v7 changes rarely
  this->v5_ = v[5];
  this->v7_ = v[7];
  this->k_ = v[6] + 2 * v[5];

  const bool rc_status = have(17, 20) && v[17] == 0x6a && v[18] == 0x30 && v[19] == 0x30 && v[20] == 0x30;
  if (rc_status) {
#ifdef USE_TEXT_SENSOR
    // the system bytes of the RC heard on the air (also with a fixed system: to see what arrives)
    char hex[19];
    for (int i = 0; i < 9; i++)
      snprintf(hex + 2 * i, 3, "%02x", v[8 + i]);
    if (this->system_text_ != nullptr && this->system_seen_ != hex) {
      this->system_seen_ = hex;
      this->system_text_->publish_state(hex);
    }
#endif
    if (!this->learned_) {
      this->use_system_(v + 8);
      ESP_LOGI(TAG, "Learned my RC: system %02x%02x, RC address %02x, station address %02x", v[8], v[9], v[10], v[15]);
    }
  }
  if (!this->learned_)
    return;

  if (v[10] == this->rc_address_) {
#ifdef USE_SENSOR
    if (this->rc_rssi_ != nullptr)
      this->rc_rssi_->publish_state(rssi);
    if (this->rc_freq_offset_ != nullptr)
      this->rc_freq_offset_->publish_state(freq_offset / 1000.0f);
#endif
    if (!have(17, 18))
      return;
    if (v[17] == 0x6a && v[18] == 0x30) {  // status: the RC's own temperature sensor
#ifdef USE_SENSOR
      if (have(22, 24) && this->inside_temperature_ != nullptr)
        this->inside_temperature_->publish_state(temperature_with_high(v[22], v[24], 168));
      if (have(22, 22) && this->raw_inside_ != nullptr)
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
    // the RC repeats a command until the station answers: one event per press
    if (cmd == this->last_remote_ && millis() - this->last_remote_ms_ < 3000)
      return;
    this->last_remote_ = cmd;
    this->last_remote_ms_ = millis();
    ESP_LOGD(TAG, "RC button: %s", COMMAND_NAMES[cmd]);
#ifdef USE_EVENT
    if (this->remote_ != nullptr)
      this->remote_->trigger(COMMAND_NAMES[cmd]);
#endif
  } else if (v[10] == this->station_address_ && (!ok[31] || v[31] == 0x30) && (!ok[32] || v[32] == 0x30)) {
    // the station's status (the start-up packets after a battery change have 00/ff in bytes 31..35). The station
    // is often weaker than the RC and its packets are often damaged: use each field whose bytes are clean.
#ifdef USE_SENSOR
    if (this->station_rssi_ != nullptr)
      this->station_rssi_->publish_state(rssi);
    if (this->station_freq_offset_ != nullptr)
      this->station_freq_offset_->publish_state(freq_offset / 1000.0f);
    // byte 18 repeats the temperature (2 * low + sign): a check for a damaged byte 17
    const bool outside_ok = have(17, 18) && ((v[18] ^ static_cast<uint8_t>(2 * static_cast<uint8_t>(154 - v[17]))) & 0xfe) == 0;
    if (outside_ok && this->outside_temperature_ != nullptr)
      this->outside_temperature_->publish_state(temperature(v[17], v[18], 154));
    if (outside_ok && this->raw_outside_ != nullptr)
      this->raw_outside_->publish_state(v[17]);
    if (have(19, 20) && static_cast<uint8_t>(2 * v[19] + v[20]) == 0 && this->wind_ != nullptr)
      this->wind_->publish_state(v[19]);
    if (have(24, 24) && this->brightness_ != nullptr)
      this->brightness_->publish_state(v[24] <= 2 ? 0 : (v[24] - 2) * 135.0f);
    if (have(24, 24) && this->raw_brightness_ != nullptr)
      this->raw_brightness_->publish_state(v[24]);
    if (have(29, 29) && this->rain_sensor_ != nullptr)
      this->rain_sensor_->publish_state(v[29]);
#endif
#ifdef USE_BINARY_SENSOR
    if (have(21, 21) && this->rain_ != nullptr)
      this->rain_->publish_state(v[21] < 0xb0);  // 0xb2 dry, 0xae while the rain alarm holds
    if (have(28, 28) && this->wind_alarm_ != nullptr)
      this->wind_alarm_->publish_state(v[28] != 0x27);  // 0x27 calm
#endif
  }
}

bool ElsnerArexa::send_command(Command cmd) {
  if (!this->learned_ || this->v5_ < 0) {
    ESP_LOGW(TAG, "No Arexa packet heard yet (the RC sends one every minute): not sending");
    return false;
  }
  uint8_t value[36] = {0};
  for (int k = 17; k < 36; k++)
    value[k] = 0x30;
  for (int i = 0; i < 9; i++)
    value[8 + i] = this->system_[i];
  for (int i = 0; i < 3; i++)
    value[17 + i] = COMMANDS[cmd][i];
  // the station takes a packet only if its counter is newer than the last one
  const uint8_t v5 = this->v5_ - 2;
  this->v5_ = v5;
  value[5] = v5;
  value[6] = this->k_ - 2 * v5;
  value[7] = this->v7_;
  const uint8_t seed = random_uint32() & 0xff;
  std::vector<uint8_t> c = {0xce, 0xad, 0x93, 0xba, seed};
  for (int k = 5; k < 36; k++)
    c.push_back(value[k] + parity_bit(seed, k) + 2 * c[k - 1]);
  const uint16_t crc = crc16(c.data(), 36);
  c.push_back(crc >> 8);
  c.push_back(crc & 0xff);
  ESP_LOGD(TAG, "Sending %s with counter %02x", COMMAND_NAMES[cmd], v5);
  this->send_frame_(c);
  return true;
}

// The chip sends it: packet mode with its own Manchester. The sync word 00 0a: the zero byte gives 16 alternating
// chips like the RC's preamble, 0a is the anchor. Then the 38 frame bytes. Three copies, like the RC repeats a
// command until the station answers.
void ElsnerArexa::send_frame_(const std::vector<uint8_t> &frame) {
  auto *r = this->radio_;
  for (int copy = 0; copy < 3; copy++) {
    r->set_manchester(true);
    r->set_sync1(0x00);
    r->set_sync0(0x0a);
    r->set_packet_length(frame.size());
    r->set_num_preamble(0);  // 2 bytes, the minimum
    r->set_fsk_deviation(40e3f);  // comes out at about the RC's +-35 kHz
    r->set_output_power(this->output_power_);
    const auto err = r->transmit_packet(frame);
    if (err != cc1101::CC1101Error::NONE)
      ESP_LOGW(TAG, "transmit_packet failed: %d", (int) err);
    this->start_listening();
    if (copy < 2)
      delay(30);  // the station answers in between
  }
}

void ElsnerArexa::dump_config() {
  ESP_LOGCONFIG(TAG, "Elsner Arexa:");
  ESP_LOGCONFIG(TAG, "  Frequency: %.3f MHz", this->frequency_ / 1e6f);
  if (this->learned_)
    ESP_LOGCONFIG(TAG, "  System %02x%02x, RC address %02x, station address %02x (%s)", this->system_[0], this->system_[1],
                  this->rc_address_, this->station_address_, this->fixed_ ? "fixed" : "learned");
  else
    ESP_LOGCONFIG(TAG, "  RC not heard yet (it sends a status packet every minute)");
}

}  // namespace esphome::elsner_arexa
