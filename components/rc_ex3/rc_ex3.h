#pragma once

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace rc_ex3 {

static const uint8_t HEADER_LEN           = 4;
static const uint8_t POS_INDOOR_AIR_TEMP  = 9;
static const uint8_t POS_OUTDOOR_AIR_TEMP = 26;
static const uint8_t POS_RETURN_AIR_TEMP  = 27;
static const uint8_t POS_COMPRESSOR_HZ    = 32;
static const uint8_t POS_INDOOR_FAN_SPEED = 45;

enum class RxState : uint8_t {
  WAITING_FOR_SOF,
  READING_PAYLOAD,
};

class RcEx3Climate : public climate::Climate, public uart::UARTDevice, public PollingComponent {
 public:
  RcEx3Climate() = default;

  void setup() override;
  void loop() override;
  void update() override;
  void control(const climate::ClimateCall &call) override;
  climate::ClimateTraits traits() override;

  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_enable_op_data(bool enable)  { op_data_enabled_ = enable; }

  // No-ops kept for YAML/climate.py compatibility
  void set_op_data_interval(uint32_t) {}
  void set_post_command_delay(uint32_t) {}

  void set_indoor_temperature_sensor(sensor::Sensor *s)    { indoor_temperature_sensor_    = s; }
  void set_outdoor_temperature_sensor(sensor::Sensor *s)   { outdoor_temperature_sensor_   = s; }
  void set_return_air_temperature_sensor(sensor::Sensor *s){ return_air_temperature_sensor_ = s; }
  void set_compressor_frequency_sensor(sensor::Sensor *s)  { compressor_frequency_sensor_  = s; }
  void set_indoor_fan_speed_sensor(sensor::Sensor *s)      { indoor_fan_speed_sensor_      = s; }

 protected:
  void send_command(const char *payload, size_t len);
  void send_status_request();
  void send_operational_data_request(bool second_page = false);

  void parse_packet(const char *raw, size_t len);
  void parse_status_response(const char *buf, size_t len);
  void parse_operational_data(const char *buf, size_t len);

  uint8_t calc_checksum(const char *data, size_t len);
  size_t  hex_to_bytes(const char *hex, uint8_t *out, size_t max_out);

  static uint8_t              fan_mode_to_wire(climate::ClimateFanMode mode);
  static climate::ClimateFanMode wire_to_fan_mode(char c);
  static uint8_t              climate_mode_to_wire(climate::ClimateMode mode);
  static climate::ClimateMode wire_to_climate_mode(uint8_t wire_val);

  static const size_t RX_BUF_SIZE = 256;
  char    rx_buf_[RX_BUF_SIZE];
  size_t  rx_len_{0};
  RxState rx_state_{RxState::WAITING_FOR_SOF};

  bool op_data_enabled_{true};
  bool op_data_pending_{false};
  bool op_data_requested_{false};  // set in update(); cleared when status response chains op_data

  sensor::Sensor *indoor_temperature_sensor_    {nullptr};
  sensor::Sensor *outdoor_temperature_sensor_   {nullptr};
  sensor::Sensor *return_air_temperature_sensor_{nullptr};
  sensor::Sensor *compressor_frequency_sensor_  {nullptr};
  sensor::Sensor *indoor_fan_speed_sensor_      {nullptr};
};

}  // namespace rc_ex3
}  // namespace esphome
