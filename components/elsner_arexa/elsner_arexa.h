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
#include <utility>
#include <vector>

namespace esphome::elsner_arexa {

enum Command : uint8_t { CMD_OPEN = 0, CMD_STOP, CMD_DOWN, CMD_AUTO, CMD_MANUAL };

// Elsner Arexa window control (RC handset + weather station) on a CC1101 in packet mode.
// Listens to the RC and the station, learns the system ID, both addresses and the packet counter from the RC's
// own packets, and sends RC commands built from them. See the README for the radio format.
class ElsnerArexa : public Component, public cc1101::CC1101Listener {
 public:
  void set_radio(cc1101::CC1101Component *radio) { this->radio_ = radio; }
  void set_frequency(float frequency) { this->frequency_ = frequency; }
  void set_output_power(float power) { this->output_power_ = power; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void on_packet(const std::vector<uint8_t> &packet, float freq_offset, float rssi, uint8_t lqi) override;

  // Puts the radio back into Arexa receive mode. Call it after something else used the same CC1101
  // (for example a remote_transmitter in OOK mode).
  void start_listening();
  // Builds a fresh RC command frame (current counter, random seed) and sends it. False if no RC packet was heard yet.
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
  void set_rssi_sensor(sensor::Sensor *s) { this->rssi_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_rain_binary_sensor(binary_sensor::BinarySensor *s) { this->rain_ = s; }
  void set_wind_alarm_binary_sensor(binary_sensor::BinarySensor *s) { this->wind_alarm_ = s; }
#endif
#ifdef USE_EVENT
  void set_remote_event(event::Event *e) { this->remote_ = e; }
#endif

 protected:
  void decode_(const std::vector<uint8_t> &chips, float rssi);
  void send_frame_(const std::vector<uint8_t> &frame);

  cc1101::CC1101Component *radio_{nullptr};
  float frequency_{868.19e6f};
  float output_power_{11.0f};
  std::vector<std::pair<std::vector<uint8_t>, float>> queue_;

  // learned from the RC's status packets
  bool learned_{false};
  uint8_t system_[9]{};  // values of bytes 8..16 of the RC's packets: system ID, addresses, device type
  uint8_t rc_address_{0};
  uint8_t station_address_{0};
  // the packet counter of the last packet heard (RC or station)
  int v5_{-1};
  uint8_t v7_{0};
  uint8_t k_{0};

  int last_remote_{-1};
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
  sensor::Sensor *rssi_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *rain_{nullptr};
  binary_sensor::BinarySensor *wind_alarm_{nullptr};
#endif
#ifdef USE_EVENT
  event::Event *remote_{nullptr};
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

}  // namespace esphome::elsner_arexa
