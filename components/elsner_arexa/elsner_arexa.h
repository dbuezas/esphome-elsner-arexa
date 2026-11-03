#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/cc1101/cc1101.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_EVENT
#include "esphome/components/event/event.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#include <utility>
#include <vector>

namespace esphome::elsner_arexa {

// the same order in COMMANDS and COMMAND_NAMES (elsner_arexa.cpp), TYPES (button.py) and EVENT_TYPES (event.py)
enum Command : uint8_t { CMD_OPEN = 0, CMD_STOP, CMD_CLOSE, CMD_AUTO, CMD_MANUAL };

// Elsner Arexa window control (RC handset + weather station) on a CC1101 in packet mode.
// Listens to the RC and the station, learns the pairing (system ID, both addresses) and the packet counter from the RC's
// own packets, and sends RC commands built from them. See the README for the radio format.
class ElsnerArexa : public Component, public cc1101::CC1101Listener {
 public:
  void set_radio(cc1101::CC1101Component *radio) { this->radio_ = radio; }
  void set_frequency(float frequency) { this->frequency_ = frequency; }
  void set_output_power(float power) { this->output_power_ = power; }
  // the pairing (bytes 8..16) to use instead of learning it from the RC; see the README
  void set_fixed_pairing(const std::vector<uint8_t> &pairing);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void on_packet(const std::vector<uint8_t> &packet, float freq_offset, float rssi, uint8_t lqi) override;

  // Puts the radio back into Arexa receive mode. Call it after something else used the same CC1101
  // (for example a remote_transmitter in OOK mode).
  void start_listening();
  // Sends an RC command (current counter, random seed) until the station answers it: a new counter every 300 ms, at
  // most 4 sends. The result goes to the log and the last command text sensor. False if no RC packet was heard yet.
  bool send_command(Command cmd);

#ifdef USE_SENSOR
  void set_inside_temperature_sensor(sensor::Sensor *s) { this->inside_temperature_ = s; }
  void set_outside_temperature_sensor(sensor::Sensor *s) { this->outside_temperature_ = s; }
  void set_wind_sensor(sensor::Sensor *s) { this->wind_ = s; }
  void set_brightness_sensor(sensor::Sensor *s) { this->brightness_ = s; }
  void set_rain_sensor_sensor(sensor::Sensor *s) { this->rain_sensor_ = s; }
  void set_raw_inside_sensor(sensor::Sensor *s) { this->raw_inside_ = s; }
  void set_raw_outside_sensor(sensor::Sensor *s) { this->raw_outside_ = s; }
  void set_raw_brightness_sensor(sensor::Sensor *s) { this->raw_brightness_ = s; }
  void set_rc_rssi_sensor(sensor::Sensor *s) { this->rc_rssi_ = s; }
  void set_station_rssi_sensor(sensor::Sensor *s) { this->station_rssi_ = s; }
  void set_rc_freq_offset_sensor(sensor::Sensor *s) { this->rc_freq_offset_ = s; }
  void set_station_freq_offset_sensor(sensor::Sensor *s) { this->station_freq_offset_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_rain_binary_sensor(binary_sensor::BinarySensor *s) { this->rain_ = s; }
  void set_wind_alarm_binary_sensor(binary_sensor::BinarySensor *s) { this->wind_alarm_ = s; }
  void set_auto_mode_binary_sensor(binary_sensor::BinarySensor *s) { this->auto_mode_ = s; }
#endif
#ifdef USE_SWITCH
  void set_auto_mode_switch(switch_::Switch *s) { this->auto_mode_switch_ = s; }
#endif
#ifdef USE_EVENT
  void set_remote_event(event::Event *e) { this->remote_ = e; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_pairing_text_sensor(text_sensor::TextSensor *s) { this->pairing_text_ = s; }
  void set_station_raw_text_sensor(text_sensor::TextSensor *s) { this->station_raw_ = s; }
  void set_last_command_text_sensor(text_sensor::TextSensor *s) { this->last_command_ = s; }
#endif

 protected:
  void decode_(const std::vector<uint8_t> &chips, float rssi, float freq_offset);
  void new_send_();
  void transmit_();
  void finish_(const char *result);

  cc1101::CC1101Component *radio_{nullptr};
  float frequency_{868.19e6f};
  float output_power_{11.0f};
  struct Received {
    std::vector<uint8_t> chips;
    float rssi;
    float freq_offset;
  };
  std::vector<Received> queue_;

  // the command being sent (-1: none). Each send has a new counter and goes out as 3 copies 30 ms apart.
  int cmd_{-1};
  int sends_{0};
  int copies_{0};  // copies of the current send so far
  uint8_t sent_v5_{0};  // the counter of the current send
  uint32_t send_ms_{0};  // when the current send started
  bool answered_{false};  // the station answered some send of this command (for a mode command: with the old mode)
  std::vector<uint8_t> frame_;

  void use_pairing_(const uint8_t *pairing);

  // learned from the RC's status packets, or fixed in the configuration
  bool learned_{false};
  bool fixed_{false};
  uint8_t pairing_[9]{};  // values of bytes 8..16 of the RC's packets: system ID, addresses, device type
  uint8_t rc_address_{0};
  uint8_t station_address_{0};
  // the packet counter of the last packet heard (RC or station)
  int v5_{-1};
  uint8_t v7_{0};
  uint8_t k_{0};

  int last_remote_{-1};
  int last_remote_counter_{-1};  // v7 << 8 | v5 of the last RC command, -1 if not clean
  uint32_t last_remote_ms_{0};

#ifdef USE_SENSOR
  sensor::Sensor *inside_temperature_{nullptr};
  sensor::Sensor *outside_temperature_{nullptr};
  sensor::Sensor *wind_{nullptr};
  sensor::Sensor *brightness_{nullptr};
  sensor::Sensor *rain_sensor_{nullptr};
  sensor::Sensor *raw_inside_{nullptr};
  sensor::Sensor *raw_outside_{nullptr};
  sensor::Sensor *raw_brightness_{nullptr};
  sensor::Sensor *rc_rssi_{nullptr};
  sensor::Sensor *station_rssi_{nullptr};
  sensor::Sensor *rc_freq_offset_{nullptr};
  sensor::Sensor *station_freq_offset_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *rain_{nullptr};
  binary_sensor::BinarySensor *wind_alarm_{nullptr};
  binary_sensor::BinarySensor *auto_mode_{nullptr};
#endif
#ifdef USE_SWITCH
  switch_::Switch *auto_mode_switch_{nullptr};
#endif
#ifdef USE_EVENT
  event::Event *remote_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *pairing_text_{nullptr};
  text_sensor::TextSensor *station_raw_{nullptr};
  text_sensor::TextSensor *last_command_{nullptr};
  std::string pairing_seen_;
#endif
};

#ifdef USE_BUTTON
class ArexaButton : public button::Button, public Parented<ElsnerArexa> {
 public:
  void set_command(uint8_t cmd) { this->cmd_ = static_cast<Command>(cmd); }

 protected:
  void press_action() override { this->parent_->send_command(this->cmd_); }
  Command cmd_{CMD_STOP};
};
#endif

#ifdef USE_SWITCH
// on: automatic mode, off: manual. Its state is the station's (byte 25): the station answers the second mode send
// with the new mode within a second.
class ArexaAutoModeSwitch : public switch_::Switch, public Parented<ElsnerArexa> {
 protected:
  void write_state(bool state) override { this->parent_->send_command(state ? CMD_AUTO : CMD_MANUAL); }
};
#endif

}  // namespace esphome::elsner_arexa
