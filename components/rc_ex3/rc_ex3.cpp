#include "rc_ex3.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rc_ex3 {

static const char *const TAG = "rc_ex3";

// set_supports_current_temperature was added in a later ESPHome release;
// call it when present, no-op otherwise.
namespace {
template<typename T>
auto set_current_temp_support(T &t, bool v)
    -> decltype(t.set_supports_current_temperature(v), void()) {
  t.set_supports_current_temperature(v);
}
void set_current_temp_support(...) {}
}

// ─── Traits ──────────────────────────────────────────────────────────────────

climate::ClimateTraits RcEx3Climate::traits() {
  auto traits = climate::ClimateTraits();
  set_current_temp_support(traits, true);
  traits.set_supported_modes({
    climate::CLIMATE_MODE_OFF,
    climate::CLIMATE_MODE_HEAT_COOL,
    climate::CLIMATE_MODE_COOL,
    climate::CLIMATE_MODE_HEAT,
    climate::CLIMATE_MODE_DRY,
    climate::CLIMATE_MODE_FAN_ONLY,
  });
  traits.set_supported_fan_modes({
    climate::CLIMATE_FAN_AUTO,
    climate::CLIMATE_FAN_LOW,
    climate::CLIMATE_FAN_MEDIUM,
    climate::CLIMATE_FAN_HIGH,
  });
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_temperature_step(0.5f);
  return traits;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void RcEx3Climate::setup() {
  this->mode                = climate::CLIMATE_MODE_OFF;
  this->target_temperature  = 22.0f;
  this->current_temperature = NAN;
}

void RcEx3Climate::update() {
  send_status_request();
  if (op_data_enabled_)
    op_data_requested_ = true;
}

// ─── Serial RX loop ──────────────────────────────────────────────────────────

void RcEx3Climate::loop() {
  while (this->available()) {
    uint8_t c;
    if (!this->read_byte(&c))
      break;

    if (rx_state_ == RxState::WAITING_FOR_SOF) {
      if (c == 0x02) {
        rx_len_   = 0;
        rx_state_ = RxState::READING_PAYLOAD;
      }
    } else {
      if (c == 0x03) {
        rx_buf_[rx_len_] = '\0';
        parse_packet(rx_buf_, rx_len_);
        rx_state_ = RxState::WAITING_FOR_SOF;
        rx_len_   = 0;
      } else if (rx_len_ < RX_BUF_SIZE - 1) {
        rx_buf_[rx_len_++] = static_cast<char>(c);
      }
    }
  }

  // Send the op_data request once the status response has been received,
  // separated by one loop tick to avoid overlapping Tx.
  if (op_data_pending_) {
    op_data_pending_ = false;
    send_operational_data_request(false);
  }
}

// ─── HA control call ─────────────────────────────────────────────────────────

void RcEx3Climate::control(const climate::ClimateCall &call) {
  if (call.get_mode().has_value())
    this->mode = *call.get_mode();
  if (call.get_target_temperature().has_value())
    this->target_temperature = *call.get_target_temperature();
  if (call.get_fan_mode().has_value())
    this->fan_mode = *call.get_fan_mode();

  uint8_t power    = (this->mode == climate::CLIMATE_MODE_OFF) ? 0 : 1;
  uint8_t mode     = climate_mode_to_wire(this->mode);
  uint8_t fan      = fan_mode_to_wire(this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO));
  uint8_t temp_wire = static_cast<uint8_t>(this->target_temperature * 2.0f);

  char buf[64];
  size_t len = snprintf(buf, sizeof(buf),
    "RSSL13FF0001%.2x02%.2x03%.2x04FF0503%.2x06FF0FFF43FF",
    power, mode, fan, temp_wire);

  ESP_LOGI(TAG, "tx → power=%d mode=%d fan=0x%02x temp_wire=%d (%.1f°C)",
           power, mode, fan, temp_wire, this->target_temperature);

  send_command(buf, len);
  this->publish_state();
}

// ─── Packet dispatch ─────────────────────────────────────────────────────────

void RcEx3Climate::parse_packet(const char *raw, size_t len) {
  char buf[256];
  size_t buflen = 0;
  bool started = false;

  for (size_t i = 0; i < len && buflen < sizeof(buf) - 1; i++) {
    uint8_t c = static_cast<uint8_t>(raw[i]);
    if (!started) {
      if (raw[i] == 'R') { buf[buflen++] = raw[i]; started = true; }
    } else {
      if (c > 32 && c < 127) buf[buflen++] = raw[i];
    }
  }
  buf[buflen] = '\0';

  if (buflen < 5)
    return;

  ESP_LOGV(TAG, "rx: %s", buf);

  // RSSL1x → climate status; queue op_data only if this update() cycle requested it
  if (buf[0] == 'R' && buf[1] == 'S' && buf[2] == 'S' && buf[3] == 'L' && buf[4] == '1') {
    parse_status_response(buf, buflen);
    if (op_data_requested_) {
      op_data_requested_ = false;
      op_data_pending_   = true;
    }
    return;
  }

  // RSR → operational data handshake / response
  if (buf[0] == 'R' && buf[1] == 'S' && buf[2] == 'R') {
    if (buf[3] == '2') {
      // Unit not yet ready; echo RSR2 immediately and it will eventually respond RSR1
      send_operational_data_request(true);
    } else if (buf[3] == '1') {
      parse_operational_data(buf, buflen);
    }
    return;
  }

  ESP_LOGD(TAG, "rx unhandled: %s", buf);
}

// ─── Status response parser ───────────────────────────────────────────────────
//
//   [0-3]  "RSSL"
//   [4]    '1'
//   [13]   power  ('0'=off, '1'=on)
//   [17]   mode   ('0'=auto,'1'=dry,'2'=cool,'3'=fan,'4'=heat)
//   [21]   fan    ('0'=spd1,'1'=spd2,'2'=spd3,'6'=spd4,other=auto)
//   [30-31] temp  (2 hex chars, value * 0.5 = °C)

void RcEx3Climate::parse_status_response(const char *buf, size_t len) {
  if (len < 32)
    return;

  char pwr_c  = buf[13];
  char mode_c = buf[17];
  char fan_c  = buf[21];

  char tmp[3] = {buf[30], buf[31], '\0'};
  unsigned int raw_temp = static_cast<unsigned int>(strtol(tmp, nullptr, 16));
  float temp_c = raw_temp * 0.5f;

  bool is_on = (pwr_c == '1');
  climate::ClimateMode new_mode = is_on ? wire_to_climate_mode(mode_c - '0') : climate::CLIMATE_MODE_OFF;

  ESP_LOGD(TAG, "status: power=%c mode=%c fan=%c temp=%.1f°C", pwr_c, mode_c, fan_c, temp_c);

  this->mode               = new_mode;
  this->fan_mode           = wire_to_fan_mode(fan_c);
  this->target_temperature = temp_c;
  this->publish_state();
}

// ─── Operational data parser ──────────────────────────────────────────────────
//
// Request:  RSR10000E8  → page 1
// Response: RSR1<hex-encoded binary blob>
// After HEADER_LEN=4 ("RSR1"), the rest is hex pairs encoding raw bytes.

void RcEx3Climate::parse_operational_data(const char *buf, size_t len) {
  const char *hex_data = buf + HEADER_LEN;
  uint8_t data[256] = {};
  size_t data_len = hex_to_bytes(hex_data, data, sizeof(data));

  if (data_len < (POS_INDOOR_FAN_SPEED - HEADER_LEN + 1)) {
    ESP_LOGW(TAG, "op-data too short (%d bytes)", (int)data_len);
    return;
  }

  auto idx = [](uint8_t pos) { return pos - HEADER_LEN; };

  float indoor_air  = static_cast<float>(static_cast<int8_t>(data[idx(POS_INDOOR_AIR_TEMP)]));
  float outdoor_air = static_cast<float>(static_cast<uint8_t>(data[idx(POS_OUTDOOR_AIR_TEMP)]) / 4 - 22);
  float return_air  = static_cast<float>(data[idx(POS_RETURN_AIR_TEMP)]) / 10.0f;
  uint8_t comp_hz   = data[idx(POS_COMPRESSOR_HZ)];
  uint8_t in_fan    = data[idx(POS_INDOOR_FAN_SPEED)];

  ESP_LOGD(TAG, "op-data raw: indoor=%d outdoor=%d return=%d",
           data[idx(POS_INDOOR_AIR_TEMP)], data[idx(POS_OUTDOOR_AIR_TEMP)], data[idx(POS_RETURN_AIR_TEMP)]);
  ESP_LOGI(TAG, "op-data → indoor=%.1f°C outdoor=%.1f°C return=%.1f°C comp=%dHz fan=%d",
           indoor_air, outdoor_air, return_air, comp_hz, in_fan);

  if (indoor_temperature_sensor_)     indoor_temperature_sensor_->publish_state(indoor_air);
  if (outdoor_temperature_sensor_)    outdoor_temperature_sensor_->publish_state(outdoor_air);
  if (return_air_temperature_sensor_) return_air_temperature_sensor_->publish_state(return_air);
  if (compressor_frequency_sensor_)   compressor_frequency_sensor_->publish_state(comp_hz);
  if (indoor_fan_speed_sensor_)       indoor_fan_speed_sensor_->publish_state(in_fan);

  ESP_LOGD(TAG, "after sensors: current_temperature=%.2f", this->current_temperature);
  this->current_temperature = indoor_air;
  ESP_LOGD(TAG, "before publish: current_temperature=%.2f", this->current_temperature);
  this->publish_state();
  ESP_LOGD(TAG, "after publish: current_temperature=%.2f", this->current_temperature);
}

// ─── Packet send helpers ─────────────────────────────────────────────────────

uint8_t RcEx3Climate::calc_checksum(const char *data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++)
    sum += static_cast<uint8_t>(data[i]);
  return sum;
}

void RcEx3Climate::send_command(const char *payload, size_t len) {
  uint8_t sum = calc_checksum(payload, len);
  char hex_sum[3];
  snprintf(hex_sum, sizeof(hex_sum), "%02X", sum);

  this->write_byte(0x02);
  for (size_t i = 0; i < len; i++)
    this->write_byte(static_cast<uint8_t>(payload[i]));
  this->write_byte(static_cast<uint8_t>(hex_sum[0]));
  this->write_byte(static_cast<uint8_t>(hex_sum[1]));
  this->write_byte(0x03);
}

void RcEx3Climate::send_status_request() {
  const char *query = "RSSL12FF0001FF02FF03FF04FF05FF06FF0FFF43FF25";
  this->write_byte(0x02);
  for (const char *p = query; *p; p++)
    this->write_byte(static_cast<uint8_t>(*p));
  this->write_byte(0x03);
}

void RcEx3Climate::send_operational_data_request(bool second_page) {
  const char *query = second_page ? "RSR20000E9" : "RSR10000E8";
  this->write_byte(0x02);
  for (const char *p = query; *p; p++)
    this->write_byte(static_cast<uint8_t>(*p));
  this->write_byte(0x03);
}

// ─── Encoding helpers ─────────────────────────────────────────────────────────

uint8_t RcEx3Climate::climate_mode_to_wire(climate::ClimateMode mode) {
  switch (mode) {
    case climate::CLIMATE_MODE_HEAT_COOL: return 0;
    case climate::CLIMATE_MODE_DRY:       return 1;
    case climate::CLIMATE_MODE_COOL:      return 2;
    case climate::CLIMATE_MODE_FAN_ONLY:  return 3;
    case climate::CLIMATE_MODE_HEAT:      return 4;
    default:                              return 0;
  }
}

climate::ClimateMode RcEx3Climate::wire_to_climate_mode(uint8_t v) {
  switch (v) {
    case 0: return climate::CLIMATE_MODE_HEAT_COOL;
    case 1: return climate::CLIMATE_MODE_DRY;
    case 2: return climate::CLIMATE_MODE_COOL;
    case 3: return climate::CLIMATE_MODE_FAN_ONLY;
    case 4: return climate::CLIMATE_MODE_HEAT;
    default: return climate::CLIMATE_MODE_HEAT_COOL;
  }
}

uint8_t RcEx3Climate::fan_mode_to_wire(climate::ClimateFanMode mode) {
  switch (mode) {
    case climate::CLIMATE_FAN_LOW:    return 0x00;
    case climate::CLIMATE_FAN_MEDIUM: return 0x02;
    case climate::CLIMATE_FAN_HIGH:   return 0x06;
    case climate::CLIMATE_FAN_AUTO:
    default:                          return 0x07;
  }
}

climate::ClimateFanMode RcEx3Climate::wire_to_fan_mode(char c) {
  switch (c) {
    case '0': return climate::CLIMATE_FAN_LOW;
    case '1': return climate::CLIMATE_FAN_LOW;
    case '2': return climate::CLIMATE_FAN_MEDIUM;
    case '6': return climate::CLIMATE_FAN_HIGH;
    default:  return climate::CLIMATE_FAN_AUTO;
  }
}

size_t RcEx3Climate::hex_to_bytes(const char *hex, uint8_t *out, size_t max_out) {
  size_t count = 0;
  char tmp[3] = {0};
  while (hex[0] && hex[1] && count < max_out) {
    if (!isxdigit(static_cast<uint8_t>(hex[0])) || !isxdigit(static_cast<uint8_t>(hex[1])))
      break;
    tmp[0] = hex[0];
    tmp[1] = hex[1];
    out[count++] = static_cast<uint8_t>(strtol(tmp, nullptr, 16));
    hex += 2;
  }
  return count;
}

}  // namespace rc_ex3
}  // namespace esphome
