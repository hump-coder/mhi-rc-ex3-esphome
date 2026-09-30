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

// Op-data handshake diagnostics: log progress this often while waiting, and
// treat this long without any reply to our last request as a stalled handshake.
static const uint32_t OP_DATA_PROGRESS_LOG_MS = 10000;
static const uint32_t OP_DATA_STALL_MS        = 5000;

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

  void set_op_data_interval(uint32_t minutes) { op_data_interval_minutes_ = minutes; }
  void set_op_data_echo_delay(uint32_t ms) { op_data_echo_delay_ms_ = ms; }

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
  bool validate_checksum_and_extract_payload_(const char *raw, size_t len, char *payload, size_t payload_size, size_t &payload_len);
  void parse_status_response(const char *buf, size_t len);
  void parse_operational_data(const char *buf, size_t len);
  void handle_op_data_not_ready_(const char *buf);
  void service_op_data_handshake_();

  uint8_t calc_checksum(const char *data, size_t len);
  size_t  hex_to_bytes(const char *hex, uint8_t *out, size_t max_out);

  static uint8_t              custom_fan_mode_to_wire(StringRef mode);
  static const char          *wire_to_custom_fan_mode(char c);
  static uint8_t              climate_mode_to_wire(climate::ClimateMode mode);
  static climate::ClimateMode wire_to_climate_mode(uint8_t wire_val);

  static const size_t RX_BUF_SIZE = 256;
  char    rx_buf_[RX_BUF_SIZE];
  size_t  rx_len_{0};
  RxState rx_state_{RxState::WAITING_FOR_SOF};

  uint32_t op_data_interval_minutes_{0};
  uint32_t last_op_data_ms_{0};
  bool op_data_pending_{false};
  bool op_data_requested_{false};  // set in update(); cleared when status response chains op_data
  bool rx_overflowed_{false};
  uint32_t op_data_started_ms_{0};  // millis() of the last page-1 op-data request
  uint32_t rsr2_retries_{0};        // RSR2 echoes this cycle (for logging only)

  // Op-data handshake timing (diagnostics + optional echo pacing).
  uint32_t op_data_echo_delay_ms_{0};  // wait before echoing RSR2; 0 = echo immediately
  bool     op_data_active_{false};     // page-1 sent, RSR1 not yet received
  bool     op_data_echo_scheduled_{false};
  uint32_t op_data_rsr2_rx_ms_{0};     // millis() of the latest RSR2
  uint32_t op_data_last_tx_ms_{0};     // millis() of the latest RSR1/RSR2 request
  uint32_t op_data_last_progress_ms_{0};
  uint32_t op_data_reply_min_ms_{0};   // request → reply latency, this cycle
  uint32_t op_data_reply_max_ms_{0};

  // Mode to send alongside power=off so the unit keeps its mode for the next
  // power-on. Updated from status and from HA while the unit is on.
  climate::ClimateMode last_on_mode_{climate::CLIMATE_MODE_HEAT_COOL};

  sensor::Sensor *indoor_temperature_sensor_    {nullptr};
  sensor::Sensor *outdoor_temperature_sensor_   {nullptr};
  sensor::Sensor *return_air_temperature_sensor_{nullptr};
  sensor::Sensor *compressor_frequency_sensor_  {nullptr};
  sensor::Sensor *indoor_fan_speed_sensor_      {nullptr};
};

}  // namespace rc_ex3
}  // namespace esphome
