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
// Tolerance when checking whether op_data_interval has elapsed at update().
static const uint32_t OP_DATA_INTERVAL_SLACK_MS = 5000;

// Poll status this long after an HA command to read the applied state.
static const uint32_t CMD_CONFIRM_DELAY_MS    = 2000;

// One request outstanding at a time: give up waiting for a reply after
// TX_REPLY_TIMEOUT_MS (replies measured at 11-155 ms), and leave TX_GAP_MS
// between a reply and the next request.
static const uint32_t TX_REPLY_TIMEOUT_MS     = 500;
static const uint32_t TX_GAP_MS               = 20;

enum class TxKind : uint8_t {
  NONE,
  STATUS,
  COMMAND,
  OP_DATA,
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

  void set_op_data_interval(uint32_t minutes) { op_data_interval_minutes_ = minutes; }
  void set_op_data_echo_delay(uint32_t ms) { op_data_echo_delay_ms_ = ms; }

  void set_indoor_temperature_sensor(sensor::Sensor *s)    { indoor_temperature_sensor_    = s; }
  void set_outdoor_temperature_sensor(sensor::Sensor *s)   { outdoor_temperature_sensor_   = s; }
  void set_return_air_temperature_sensor(sensor::Sensor *s){ return_air_temperature_sensor_ = s; }
  void set_compressor_frequency_sensor(sensor::Sensor *s)  { compressor_frequency_sensor_  = s; }
  void set_indoor_fan_speed_sensor(sensor::Sensor *s)      { indoor_fan_speed_sensor_      = s; }

 protected:
  void send_command(const char *payload, size_t len);
  void send_command_();
  void service_tx_();
  void begin_tx_(TxKind kind);
  void finish_tx_();
  static const char *tx_kind_name_(TxKind kind);
  void send_status_request();
  void send_operational_data_request(bool second_page = false);

  void parse_packet(const char *raw, size_t len);
  bool validate_checksum_and_extract_payload_(const char *raw, size_t len, char *payload, size_t payload_size, size_t &payload_len);
  void parse_status_response(const char *buf, size_t len);
  void parse_operational_data(const char *buf, size_t len);
  void handle_op_data_not_ready_(const char *buf);
  void service_op_data_handshake_();
  void service_command_confirm_();

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
  uint32_t last_op_data_ms_{0};     // op_data_cycle_ms_ of the last successful op-data
  uint32_t op_data_cycle_ms_{0};    // millis() of the update() that requested op-data
  bool op_data_ever_received_{false};
  bool op_data_pending_{false};     // op-data start waiting for the bus
  bool op_data_requested_{false};  // set in update(); cleared when status response chains op_data
  bool rx_overflowed_{false};
  uint32_t op_data_started_ms_{0};  // millis() of the last page-1 op-data request
  uint32_t rsr2_retries_{0};        // RSR2 echoes this cycle (for logging only)

  // Op-data handshake timing (diagnostics + optional echo pacing).
  uint32_t op_data_echo_delay_ms_{500};  // wait before echoing RSR2; 0 = echo immediately
  bool     op_data_active_{false};     // page-1 sent, RSR1 not yet received
  bool     op_data_echo_scheduled_{false};
  uint32_t op_data_rsr2_rx_ms_{0};     // millis() of the latest RSR2
  uint32_t op_data_last_tx_ms_{0};     // millis() of the latest RSR1/RSR2 request
  uint32_t op_data_last_progress_ms_{0};
  uint32_t op_data_reply_min_ms_{0};   // request → reply latency, this cycle
  uint32_t op_data_reply_max_ms_{0};

  // Mode to send alongside power=off so the unit keeps its mode for the next
  // power-on. Updated from every status reply (on or off) and from HA.
  climate::ClimateMode last_on_mode_{climate::CLIMATE_MODE_HEAT_COOL};
  bool status_received_{false};  // HA commands are dropped until the first status reply

  uint32_t cmd_sent_ms_{0};           // millis() of the latest HA command
  bool     cmd_confirm_pending_{false};  // status poll due CMD_CONFIRM_DELAY_MS after it

  // Request scheduling (see service_tx_()).
  TxKind   inflight_{TxKind::NONE};   // request awaiting its reply
  uint32_t inflight_ms_{0};
  uint32_t bus_idle_ms_{0};           // millis() the last request completed
  bool     command_pending_{false};   // HA command waiting for the bus
  bool     status_pending_{false};    // status poll waiting for the bus

  sensor::Sensor *indoor_temperature_sensor_    {nullptr};
  sensor::Sensor *outdoor_temperature_sensor_   {nullptr};
  sensor::Sensor *return_air_temperature_sensor_{nullptr};
  sensor::Sensor *compressor_frequency_sensor_  {nullptr};
  sensor::Sensor *indoor_fan_speed_sensor_      {nullptr};
};

}  // namespace rc_ex3
}  // namespace esphome
