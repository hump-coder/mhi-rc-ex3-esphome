#pragma once

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace rc_ex3 {

// Byte positions in the operational data response (after the 4-byte "RSR1" header)
static const uint8_t HEADER_LEN           = 4;
static const uint8_t POS_OPERATION_MODE   = 8;
static const uint8_t POS_INDOOR_AIR_TEMP  = 9;
static const uint8_t POS_TARGET_TEMP      = 7;
static const uint8_t POS_OUTDOOR_AIR_TEMP = 26;
static const uint8_t POS_RETURN_AIR_TEMP  = 27;
static const uint8_t POS_COMPRESSOR_HZ    = 32;
static const uint8_t POS_INDOOR_FAN_SPEED  = 45;
static const uint8_t POS_CURRENT          = 42;

struct HvacData {
  int8_t   indoor_air_temp;
  int8_t   outdoor_air_temp;
  int8_t   return_air_temp;
  int8_t   target_temp;
  uint8_t  operation_mode;
  uint8_t  compressor_hz;
  uint8_t  indoor_fan_speed;
  int8_t   current;
  bool     valid;
};

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

  void set_op_data_interval(uint32_t minutes)              { op_data_interval_ms_ = minutes * 60000UL; }

  void set_indoor_temperature_sensor(sensor::Sensor *s)   { indoor_temperature_sensor_   = s; }
  void set_outdoor_temperature_sensor(sensor::Sensor *s)  { outdoor_temperature_sensor_  = s; }
  void set_return_air_temperature_sensor(sensor::Sensor *s){ return_air_temperature_sensor_ = s; }
  void set_compressor_frequency_sensor(sensor::Sensor *s) { compressor_frequency_sensor_ = s; }
  void set_indoor_fan_speed_sensor(sensor::Sensor *s)     { indoor_fan_speed_sensor_     = s; }

 protected:
  void send_command(const char *payload, size_t len);
  void send_status_request();
  void send_operational_data_request(bool second_page = false);

  void parse_packet(const char *raw, size_t len);
  void parse_status_response(const char *buf, size_t len);
  void parse_operational_data(const char *buf, size_t len);

  uint8_t calc_checksum(const char *data, size_t len);
  size_t  hex_to_bytes(const char *hex, uint8_t *out, size_t max_out);

  static uint8_t fan_mode_to_wire(climate::ClimateFanMode mode);
  static uint8_t climate_mode_to_wire(climate::ClimateMode mode);
  static climate::ClimateMode wire_to_climate_mode(uint8_t wire_val);
  static climate::ClimateFanMode wire_to_fan_mode(char c);

  // RX ring buffer + state machine
  static const size_t RX_BUF_SIZE = 256;
  char     rx_buf_[RX_BUF_SIZE];
  size_t   rx_len_{0};
  RxState  rx_state_{RxState::WAITING_FOR_SOF};

  // Operational data polling
  uint32_t op_data_interval_ms_{0};
  uint32_t last_op_data_ms_{0};
  bool     op_data_ever_requested_{false};
  bool     op_data_pending_{false};

  // Suppress the controller echo that arrives after a control() command.
  // The echo reflects the pre-command state, so we ignore it to avoid
  // stomping on our own optimistic publish.
  bool     suppress_next_status_{false};

  sensor::Sensor *indoor_temperature_sensor_    {nullptr};
  sensor::Sensor *outdoor_temperature_sensor_   {nullptr};
  sensor::Sensor *return_air_temperature_sensor_{nullptr};
  sensor::Sensor *compressor_frequency_sensor_  {nullptr};
  sensor::Sensor *indoor_fan_speed_sensor_      {nullptr};
};

}  // namespace rc_ex3
}  // namespace esphome
