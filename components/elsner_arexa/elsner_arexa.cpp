#include "elsner_arexa.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

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
    this->queue_.emplace_back(packet, rssi);
}

void ElsnerArexa::loop() {
  for (auto &item : this->queue_)
    this->decode_(item.first, item.second);
  this->queue_.clear();
}

void ElsnerArexa::decode_(const std::vector<uint8_t> &x, float rssi) {
  // Manchester in software: stop at the first broken chip pair (the end of a weak or cut frame)
  uint8_t c[38] = {0xce};
  int n = 1;
  for (size_t k = 0; k + 1 < x.size() * 8 && n < 38; k += 2) {
    const int a = (x[k >> 3] >> (7 - (k & 7))) & 1, b = (x[(k + 1) >> 3] >> (7 - ((k + 1) & 7))) & 1;
    if (a == b)
      break;
    const size_t i = k / 2;
    c[1 + i / 8] |= a << (7 - i % 8);
    if (i % 8 == 7)
      n++;
  }
  // no CRC (it is past the end of the FIFO): the 32-bit header is the check, the fields end at byte 30
  if (n < 31 || c[1] != 0xad || c[2] != 0x93 || c[3] != 0xba)
    return;
  const uint8_t seed = c[4];
  uint8_t v[36] = {0};
  for (int k = 5; k < n && k < 36; k++)
    v[k] = c[k] - 2 * c[k - 1] - parity_bit(seed, k);

  // the counter (RC and station share it): v5 counts down, v6 = k - 2 v5, v7 changes rarely
  this->v5_ = v[5];
  this->v7_ = v[7];
  this->k_ = v[6] + 2 * v[5];

  const bool rc_status = v[17] == 0x6a && v[18] == 0x30 && v[19] == 0x30 && v[20] == 0x30;
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

#ifdef USE_SENSOR
  if (this->rssi_ != nullptr)
    this->rssi_->publish_state(rssi);
#endif
  if (v[10] == this->rc_address_) {
    if (v[17] == 0x6a && v[18] == 0x30) {  // status: the RC's own temperature sensor
#ifdef USE_SENSOR
      if (this->inside_temperature_ != nullptr)
        this->inside_temperature_->publish_state((428 - v[22]) / 10.0f);
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
  } else if (v[10] == this->station_address_ && n >= 33 && v[31] == 0x30 && v[32] == 0x30) {
    // the station's status (the start-up packets have 00/ff here)
#ifdef USE_SENSOR
    if (this->outside_temperature_ != nullptr)
      this->outside_temperature_->publish_state(14.65f - 0.0865f * v[17]);
    if (this->raw_outside_ != nullptr)
      this->raw_outside_->publish_state(v[17]);
    if (this->wind_ != nullptr)
      this->wind_->publish_state(v[19]);
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
