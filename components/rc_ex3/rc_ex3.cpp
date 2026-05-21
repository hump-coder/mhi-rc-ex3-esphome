#include "rc_ex3.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rc_ex3 {

static const char *const TAG = "rc_ex3";

// ─── Traits ──────────────────────────────────────────────────────────────────

climate::ClimateTraits RcEx3Climate::traits() {
  auto traits = climate::ClimateTraits();
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
  });
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_temperature_step(0.5f);
  return traits;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void RcEx3Climate::setup() {
  this->set_supported_custom_fan_modes({"1", "2", "3", "4"});
  this->mode               = climate::CLIMATE_MODE_OFF;
  this->target_temperature = 22.0f;
  this->current_temperature = NAN;
  send_status_request();
}

void RcEx3Climate::update() {
  // Called on the polling interval (default 30 s).
  // Send status request first; operational data request follows after we get
  // the status response (op_data_pending_ flag), so we avoid overlapping Tx.
  send_status_request();
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

  // Fire op_data once 10 s after startup regardless of yaml op_data_interval setting.
  if (!op_data_ever_requested_ && millis() >= 10000) {
    op_data_pending_        = true;
    op_data_ever_requested_ = true;
    last_op_data_ms_        = millis();
  }

  // Send the operational data request once the status has been received,
  // keeping the two transactions separated by one loop tick.
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
    this->set_fan_mode_(*call.get_fan_mode());
  if (call.has_custom_fan_mode())
    this->set_custom_fan_mode_(call.get_custom_fan_mode());

  // Build combined setClimate packet (mirrors rc3.cpp setClimate).
  uint8_t power = (this->mode == climate::CLIMATE_MODE_OFF) ? 0 : 1;
  uint8_t mode  = climate_mode_to_wire(this->mode);
  uint8_t fan   = this->has_custom_fan_mode()
                    ? custom_fan_mode_to_wire(this->get_custom_fan_mode().c_str())
                    : fan_mode_to_wire(this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO));

  // Temperature encoded as (°C * 2) → wire byte, matching original degrees/5
  // where degrees was passed in tenths: wire = (temp_tenths / 5) = (temp * 10 / 5) = temp * 2
  uint8_t temp_wire = static_cast<uint8_t>(this->target_temperature * 2.0f);

  char buf[64];
  size_t len = snprintf(buf, sizeof(buf),
    "RSSL13FF0001%.2x02%.2x03%.2x04FF0503%.2x06FF0FFF43FF",
    power, mode, fan, temp_wire);

  ESP_LOGI(TAG, "tx → power=%d mode=%d fan=0x%02x temp_wire=%d (%.1f°C) payload=%s",
           power, mode, fan, temp_wire, this->target_temperature, buf);

  suppress_next_status_ = true;
  send_command(buf, len);
  this->publish_state();

  if (post_command_delay_ms_ > 0) {
    this->set_timeout("post_cmd_status", post_command_delay_ms_, [this]() {
      ESP_LOGD(TAG, "post-command status request");
      send_status_request();
    });
  }
}

// ─── Packet dispatch ─────────────────────────────────────────────────────────

void RcEx3Climate::parse_packet(const char *raw, size_t len) {
  // Log raw bytes as hex so unsolicited controller packets are visible
  char hex_dump[512];
  size_t hpos = 0;
  for (size_t i = 0; i < len && hpos < sizeof(hex_dump) - 3; i++)
    hpos += snprintf(hex_dump + hpos, sizeof(hex_dump) - hpos, "%02X ", (uint8_t)raw[i]);
  ESP_LOGD(TAG, "rx raw (%d bytes): %s", (int)len, hex_dump);

  // Filter raw bytes: skip until first 'R', keep printable ASCII only.
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

  if (buflen < 5) {
    ESP_LOGD(TAG, "rx filtered too short (%d chars), discarding", (int)buflen);
    return;
  }

  ESP_LOGD(TAG, "rx filtered: %s", buf);

  // RSSL1x → climate status response
  if (buf[0] == 'R' && buf[1] == 'S' && buf[2] == 'S' && buf[3] == 'L' && buf[4] == '1') {
    parse_status_response(buf, buflen);
    return;
  }

  // RSR1x → operational data response
  if (buf[0] == 'R' && buf[1] == 'S' && buf[2] == 'R') {
    if (buf[3] == '2') {
      // Unit signals page 2 required; re-request with RSR20000E9
      send_operational_data_request(true);
    } else if (buf[3] == '1') {
      parse_operational_data(buf, buflen);
    }
    return;
  }

  ESP_LOGD(TAG, "rx unhandled packet: %s", buf);
}

// ─── Status response parser ───────────────────────────────────────────────────
//
// Response (filtered ASCII starting at 'R') character positions:
//   [0-3]  "RSSL"
//   [4]    '1'  (length nibble hi)
//   [13]   power  ('0'=off, '1'=on)
//   [17]   mode   ('0'=auto,'1'=dry,'2'=cool,'3'=fan,'4'=heat)
//   [21]   fan    ('0'=spd1,'1'=spd2,'2'=spd3,'6'=spd4,other=auto)
//   [30-31] temp  (2 hex chars, value * 0.5 = °C)

void RcEx3Climate::parse_status_response(const char *buf, size_t len) {
  if (len < 32)
    return;

  if (suppress_next_status_) {
    suppress_next_status_ = false;
    ESP_LOGI(TAG, "rx ← suppressed echo (post-command), ignoring");
    return;
  }

  char pwr_c  = buf[13];
  char mode_c = buf[17];
  char fan_c  = buf[21];

  char tmp[3] = {buf[30], buf[31], '\0'};
  unsigned int raw_temp = static_cast<unsigned int>(strtol(tmp, nullptr, 16));
  float temp_c = raw_temp * 0.5f;  // wire value = °C * 2

  bool is_on = (pwr_c == '1');
  climate::ClimateMode new_mode = is_on ? wire_to_climate_mode(mode_c - '0') : climate::CLIMATE_MODE_OFF;

  ESP_LOGI(TAG, "rx ← power=%c mode=%c fan=%c temp=%.1f°C", pwr_c, mode_c, fan_c, temp_c);

  this->mode             = new_mode;
  this->target_temperature = temp_c;
  apply_wire_fan_mode(fan_c);
  this->publish_state();

  if (op_data_interval_ms_ > 0) {
    uint32_t now = millis();
    if (op_data_ever_requested_ && (now - last_op_data_ms_) >= op_data_interval_ms_) {
      op_data_pending_  = true;
      last_op_data_ms_  = now;
    }
  }
}

// ─── Operational data parser ──────────────────────────────────────────────────
//
// Request:  RSR10000E8  → page 1 (most useful data)
// Response: RSR1<hex-encoded binary blob>
// After HEADER_LEN=4 chars ("RSR1"), the rest is hex pairs encoding raw bytes.

void RcEx3Climate::parse_operational_data(const char *buf, size_t len) {
  const char *hex_data = buf + HEADER_LEN;
  uint8_t data[256] = {};
  size_t data_len = hex_to_bytes(hex_data, data, sizeof(data));

  if (data_len < (POS_INDOOR_FAN_SPEED - HEADER_LEN + 1)) {
    ESP_LOGW(TAG, "op-data response too short (%d bytes)", (int)data_len);
    return;
  }

  auto idx = [](uint8_t pos) { return pos - HEADER_LEN; };

  float indoor_air   = static_cast<float>(static_cast<int8_t>(data[idx(POS_INDOOR_AIR_TEMP)]));
  float outdoor_air  = static_cast<float>(static_cast<uint8_t>(data[idx(POS_OUTDOOR_AIR_TEMP)]) / 4 - 22);
  float return_air   = static_cast<float>(data[idx(POS_RETURN_AIR_TEMP)]) / 10.0f;

  ESP_LOGD(TAG, "op-data raw: indoor=%d outdoor=%d return=%d",
           data[idx(POS_INDOOR_AIR_TEMP)], data[idx(POS_OUTDOOR_AIR_TEMP)], data[idx(POS_RETURN_AIR_TEMP)]);
  uint8_t comp_hz    = data[idx(POS_COMPRESSOR_HZ)];
  uint8_t in_fan     = data[idx(POS_INDOOR_FAN_SPEED)];

  // Update current_temperature from indoor sensor and republish climate state
  this->current_temperature = indoor_air;
  this->publish_state();

  if (indoor_temperature_sensor_)    indoor_temperature_sensor_->publish_state(indoor_air);
  if (outdoor_temperature_sensor_)   outdoor_temperature_sensor_->publish_state(outdoor_air);
  if (return_air_temperature_sensor_) return_air_temperature_sensor_->publish_state(return_air);
  if (compressor_frequency_sensor_)  compressor_frequency_sensor_->publish_state(comp_hz);
  if (indoor_fan_speed_sensor_)      indoor_fan_speed_sensor_->publish_state(in_fan);
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
  // Payload + pre-calculated checksum 0x25 ("RSSL12FF0001FF02FF03FF04FF05FF06FF0FFF43FF" → sum=0x25)
  const char *query = "RSSL12FF0001FF02FF03FF04FF05FF06FF0FFF43FF25";
  this->write_byte(0x02);
  for (const char *p = query; *p; p++)
    this->write_byte(static_cast<uint8_t>(*p));
  this->write_byte(0x03);
}

void RcEx3Climate::send_operational_data_request(bool second_page) {
  // "RSR10000E8" or "RSR20000E9" — checksums are pre-embedded in the literals
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
  // Only CLIMATE_FAN_AUTO is a built-in mode; custom modes "1"–"4" handled separately.
  return 0x07;
}

uint8_t RcEx3Climate::custom_fan_mode_to_wire(const char *mode) {
  if (mode == nullptr)          return 0x07;
  if (strcmp(mode, "1") == 0)   return 0x00;
  if (strcmp(mode, "2") == 0)   return 0x01;
  if (strcmp(mode, "3") == 0)   return 0x02;
  if (strcmp(mode, "4") == 0)   return 0x06;
  return 0x07;
}

void RcEx3Climate::apply_wire_fan_mode(char c) {
  switch (c) {
    case '0': this->set_custom_fan_mode_("1"); break;
    case '1': this->set_custom_fan_mode_("2"); break;
    case '2': this->set_custom_fan_mode_("3"); break;
    case '6': this->set_custom_fan_mode_("4"); break;
    default:  this->set_fan_mode_(climate::CLIMATE_FAN_AUTO); break;
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
