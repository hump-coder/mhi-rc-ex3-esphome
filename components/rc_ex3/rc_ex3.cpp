#include "rc_ex3.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cmath>

namespace esphome {
namespace rc_ex3 {

static const char *const TAG = "rc_ex3";

// ─── Traits ──────────────────────────────────────────────────────────────────

climate::ClimateTraits RcEx3Climate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_supported_modes({
    climate::CLIMATE_MODE_OFF,
    climate::CLIMATE_MODE_HEAT_COOL,
    climate::CLIMATE_MODE_COOL,
    climate::CLIMATE_MODE_HEAT,
    climate::CLIMATE_MODE_DRY,
    climate::CLIMATE_MODE_FAN_ONLY,
  });
  traits.set_supported_fan_modes({climate::CLIMATE_FAN_AUTO});
  traits.set_visual_min_temperature(TEMP_MIN_C);
  traits.set_visual_max_temperature(TEMP_MAX_C);
  traits.set_visual_temperature_step(0.5f);
  return traits;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void RcEx3Climate::setup() {
  this->set_supported_custom_fan_modes({"1", "2", "3", "4"});
  this->mode                = climate::CLIMATE_MODE_OFF;
  this->target_temperature  = 22.0f;
  this->current_temperature = NAN;
  this->status_pending_ = true;  // read the unit's state now, not at the first update()
}

void RcEx3Climate::update() {
  status_pending_ = true;
  status_retry_left_ = true;

  if (op_data_interval_minutes_ == 0)
    return;
  // An op-data cycle already requested, queued or running covers this update;
  // requesting again would start a second ~40 s handshake straight after it.
  if (op_data_requested_ || op_data_pending_ || op_data_active_)
    return;

  // Measured from the update() that requested the last successful op-data,
  // with slack for scheduler jitter, so op_data_interval: 5 with a 5 min
  // update_interval really runs every cycle (the ~40 s handshake no longer
  // pushes it to every other cycle).
  const uint32_t now = millis();
  const uint32_t interval_ms = op_data_interval_minutes_ * 60000UL;  // >= 60 s > slack
  if (!op_data_ever_received_ || (now - last_op_data_ms_) >= interval_ms - OP_DATA_INTERVAL_SLACK_MS) {
    op_data_requested_ = true;
    op_data_cycle_ms_ = now;
  }
}

// ─── Serial RX loop ──────────────────────────────────────────────────────────

void RcEx3Climate::loop() {
  while (this->available()) {
    uint8_t c;
    if (!this->read_byte(&c))
      break;

    if (rx_state_ == RxState::WAITING_FOR_SOF) {
      if (c == 0x02) {
        rx_len_ = 0;
        rx_overflowed_ = false;
        rx_state_ = RxState::READING_PAYLOAD;
      }
    } else if (c == 0x02) {
      // STX mid-frame: the previous frame lost its ETX; start over so this one survives.
      ESP_LOGW(TAG, "rx frame without ETX dropped");
      rx_len_ = 0;
      rx_overflowed_ = false;
    } else {
      if (c == 0x03) {
        if (rx_overflowed_) {
          ESP_LOGW(TAG, "rx frame dropped after overflow");
        } else {
          rx_buf_[rx_len_] = '\0';
          parse_packet(rx_buf_, rx_len_);
        }
        rx_state_ = RxState::WAITING_FOR_SOF;
        rx_len_   = 0;
      } else if (rx_len_ < RX_BUF_SIZE - 1) {
        rx_buf_[rx_len_++] = static_cast<char>(c);
      } else {
        rx_overflowed_ = true;
      }
    }
  }

  if (!status_received_ && !status_pending_ && inflight_ == TxKind::NONE &&
      millis() - status_sent_ms_ >= STATUS_STARTUP_RETRY_MS)
    status_pending_ = true;

  service_op_data_handshake_();
  service_command_confirm_();
  service_tx_();
}

// ─── Request scheduling ──────────────────────────────────────────────────────
//
// Only one request is ever outstanding: a command sent while the unit was
// still answering a status poll was observed to be silently dropped. Each
// reply completes the in-flight request and is interpreted according to what
// was sent. When the bus is free, pending work goes out in priority order:
// HA command, op-data echo, status poll, op-data start.

void RcEx3Climate::service_tx_() {
  const uint32_t now = millis();

  if (inflight_ != TxKind::NONE) {
    if (now - inflight_ms_ < TX_REPLY_TIMEOUT_MS)
      return;
    ESP_LOGW(TAG, "no reply to %s within %u ms", tx_kind_name_(inflight_), (unsigned) TX_REPLY_TIMEOUT_MS);
    if (inflight_ == TxKind::OP_DATA && op_data_active_ && rsr2_retries_ > 0) {
      // Abandoning the handshake part-way is what we must avoid: echo again
      // (after the usual delay) until the stall detector gives up.
      op_data_rsr2_rx_ms_ = now;
      op_data_echo_scheduled_ = true;
    } else if (inflight_ == TxKind::COMMAND && (command_pending_ || cmd_retry_left_)) {
      // Commands are rebuilt from the current state, so resending is harmless.
      // (A newer queued command covers this one and keeps the retry for
      // itself.) Either way the resend must also carry this command's fields.
      cmd_fields_ |= cmd_inflight_fields_;
      if (!command_pending_) {
        cmd_retry_left_ = false;
        command_pending_ = true;
      }
    } else if (inflight_ == TxKind::STATUS && status_retry_left_ && !status_pending_) {
      status_retry_left_ = false;
      status_pending_ = true;
    }
    finish_tx_();
  }
  if (now - bus_idle_ms_ < TX_GAP_MS)
    return;

  if (command_pending_) {
    command_pending_ = false;
    send_command_();
  } else if (op_data_active_ && op_data_echo_scheduled_) {
    if (now - op_data_rsr2_rx_ms_ >= op_data_echo_delay_ms_) {
      op_data_echo_scheduled_ = false;
      send_operational_data_request(true);
    }
  } else if (status_pending_ && !op_data_active_) {
    // Status polls wait for the op-data handshake to finish rather than interleave with it.
    status_pending_ = false;
    send_status_request();
  } else if (op_data_pending_ && !op_data_active_ && !status_pending_) {
    op_data_pending_ = false;
    send_operational_data_request(false);
  }
}

void RcEx3Climate::begin_tx_(TxKind kind) {
  inflight_ = kind;
  inflight_ms_ = millis();
}

void RcEx3Climate::finish_tx_() {
  inflight_ = TxKind::NONE;
  bus_idle_ms_ = millis();
}

const char *RcEx3Climate::tx_kind_name_(TxKind kind) {
  switch (kind) {
    case TxKind::STATUS:  return "status poll";
    case TxKind::COMMAND: return "command";
    case TxKind::OP_DATA: return "op-data request";
    default:              return "nothing";
  }
}

// ─── HA control call ─────────────────────────────────────────────────────────

void RcEx3Climate::control(const climate::ClimateCall &call) {
  // Every command carries the setpoint. Until the first status reply we'd send
  // a default and silently change the unit, so drop the command instead.
  if (!status_received_) {
    ESP_LOGW(TAG, "ignoring HA command: unit state not yet read");
    this->publish_state();  // revert HA to the last known state
    return;
  }
  if (call.get_mode().has_value()) {
    this->mode = *call.get_mode();
    // Off is power only: the unit keeps its mode for the next power-on.
    cmd_fields_ |= CMD_FIELD_POWER;
    if (this->mode != climate::CLIMATE_MODE_OFF)
      cmd_fields_ |= CMD_FIELD_MODE;
  }
  if (call.get_target_temperature().has_value()) {
    // Store what will actually be sent: 0.5 °C steps within the unit's range.
    const float t = *call.get_target_temperature();
    if (std::isfinite(t))
      this->target_temperature = roundf(std::min(std::max(t, TEMP_MIN_C), TEMP_MAX_C) * 2.0f) / 2.0f;
  }
  if (call.get_fan_mode().has_value()) {
    this->set_fan_mode_(*call.get_fan_mode());  // clears any custom speed
    cmd_fields_ |= CMD_FIELD_FAN;
  }
  if (call.has_custom_fan_mode()) {
    this->set_custom_fan_mode_(call.get_custom_fan_mode());
    cmd_fields_ |= CMD_FIELD_FAN;
  }

  // Commands are built when sent, so a command still waiting for the bus
  // simply picks up this change too.
  if (command_pending_)
    ESP_LOGD(TAG, "HA change merged into the queued command");
  if (!raw_command_.empty()) {
    ESP_LOGW(TAG, "HA command replaces queued raw command");
    raw_command_.clear();
  }
  command_pending_ = true;
  cmd_retry_left_ = true;
  this->publish_state();
}

void RcEx3Climate::send_raw_command(const std::string &body) {
  // Only set commands, as hex field pairs, so a typo can't send another message type.
  bool ok = body.size() >= 10 && body.size() <= 64 && body.size() % 2 == 0 &&
            body.compare(0, 6, "RSSL13") == 0;
  for (size_t i = 6; ok && i < body.size(); i++)
    ok = isxdigit(static_cast<uint8_t>(body[i]));
  if (!ok) {
    ESP_LOGW(TAG, "raw command rejected (want RSSL13 + hex, no checksum): %s", body.c_str());
    return;
  }
  if (!status_received_) {
    ESP_LOGW(TAG, "raw command rejected: unit state not yet read");
    return;
  }
  if (command_pending_)
    ESP_LOGW(TAG, "raw command replaces the queued HA command");
  raw_command_ = body;
  cmd_fields_ = 0;
  command_pending_ = true;
  cmd_retry_left_ = false;  // a resend would rebuild the HA state, not this body
}

void RcEx3Climate::send_command_() {
  if (!raw_command_.empty()) {
    ESP_LOGI(TAG, "tx raw → %s", raw_command_.c_str());
    send_command(raw_command_.c_str(), raw_command_.size());
    raw_command_.clear();
    cmd_inflight_fields_ = 0;
    begin_tx_(TxKind::COMMAND);
    cmd_sent_ms_ = millis();
    cmd_confirm_pending_ = true;
    return;
  }

  // Fields HA didn't change go out as FF so they can't revert a panel change
  // made since the last poll.
  const uint8_t fields = cmd_fields_;
  cmd_fields_ = 0;
  cmd_inflight_fields_ = fields;
  const bool off = this->mode == climate::CLIMATE_MODE_OFF;
  uint8_t power = (fields & CMD_FIELD_POWER) ? (off ? 0 : 1) : 0xFF;
  uint8_t mode  = (fields & CMD_FIELD_MODE) && !off ? climate_mode_to_wire(this->mode) : 0xFF;
  uint8_t fan   = (fields & CMD_FIELD_FAN) ? custom_fan_mode_to_wire(this->get_custom_fan_mode()) : 0xFF;
  const float temp_c = std::isfinite(this->target_temperature)
                           ? std::min(std::max(this->target_temperature, TEMP_MIN_C), TEMP_MAX_C)
                           : 22.0f;
  uint8_t temp_wire = static_cast<uint8_t>(lroundf(temp_c * 2.0f));

  char buf[64];
  size_t len = snprintf(buf, sizeof(buf),
    "RSSL13FF0001%.2X02%.2X03%.2X04FF0503%.2X06FF0FFF43FF",
    power, mode, fan, temp_wire);

  ESP_LOGI(TAG, "tx → power=0x%02X mode=0x%02X fan=0x%02X temp_wire=%d (%.1f°C) (FF = unchanged)",
           power, mode, fan, temp_wire, this->target_temperature);
  if (op_data_active_)
    ESP_LOGD(TAG, "tx command during op-data handshake (%u ms in, %u retries)",
             (unsigned) (millis() - op_data_started_ms_), (unsigned) rsr2_retries_);

  send_command(buf, len);
  begin_tx_(TxKind::COMMAND);
  // The reply to a command reflects the unit's state at reply time, which may
  // or may not include the command yet, so it's treated as an ack only; a
  // status poll shortly afterwards reads the applied state. HA keeps the
  // commanded state meanwhile.
  cmd_sent_ms_ = millis();
  cmd_confirm_pending_ = true;
}

void RcEx3Climate::service_command_confirm_() {
  const uint32_t now = millis();
  if (!cmd_confirm_pending_ || command_pending_ || now - cmd_sent_ms_ < CMD_CONFIRM_DELAY_MS)
    return;
  cmd_confirm_pending_ = false;
  ESP_LOGD(TAG, "confirming command with status poll (%u ms after tx)", (unsigned) (now - cmd_sent_ms_));
  status_pending_ = true;  // sent once the bus is free (and any op-data handshake has ended)
  status_retry_left_ = true;
}

// ─── Packet dispatch ─────────────────────────────────────────────────────────

void RcEx3Climate::parse_packet(const char *raw, size_t len) {
  char payload[256];
  size_t payload_len = 0;
  if (!validate_checksum_and_extract_payload_(raw, len, payload, sizeof(payload), payload_len))
    return;

  char buf[256];
  size_t buflen = 0;
  bool started = false;

  for (size_t i = 0; i < payload_len && buflen < sizeof(buf) - 1; i++) {
    uint8_t c = static_cast<uint8_t>(payload[i]);
    if (!started) {
      if (payload[i] == 'R') {
        buf[buflen++] = payload[i];
        started = true;
      }
    } else if (c >= 32 && c < 127) {
      buf[buflen++] = payload[i];
    }
  }
  buf[buflen] = '\0';

  if (buflen < 5)
    return;

  ESP_LOGV(TAG, "rx: %s", buf);

  const bool is_rssl = buf[0] == 'R' && buf[1] == 'S' && buf[2] == 'S' && buf[3] == 'L';
  const bool is_rsr  = buf[0] == 'R' && buf[1] == 'S' && buf[2] == 'R';
  const TxKind was = inflight_;

  if (is_rssl && (was == TxKind::STATUS || was == TxKind::COMMAND)) {
    finish_tx_();
  } else if (is_rsr && was == TxKind::OP_DATA) {
    finish_tx_();
  } else {
    // Late reply to a timed-out request, or not what we asked for.
    ESP_LOGW(TAG, "rx while awaiting %s: %s", tx_kind_name_(was), buf);
    if (is_rssl)
      return;  // can't tell which state it reflects; never apply it
  }

  // Reply to a command: an ack only (RSSL11 state at reply time, or
  // occasionally RSSL08), never applied.
  if (is_rssl && was == TxKind::COMMAND) {
    ESP_LOGD(TAG, "command ack after %u ms (not applied): %s", (unsigned) (millis() - cmd_sent_ms_), buf);
    return;
  }

  if (is_rssl && buf[4] != '1') {
    ESP_LOGD(TAG, "rx non-status RSSL reply to status poll: %s", buf);
    return;
  }

  // RSSL1x → climate status; queue op_data only if this update() cycle requested it
  if (is_rssl) {
    if (!parse_status_response(buf, buflen)) {
      // Corrupt reply: poll again (startup keeps polling until a valid one) and
      // don't start op-data off it.
      if (status_retry_left_ && !status_pending_) {
        status_retry_left_ = false;
        status_pending_ = true;
      }
      return;
    }
    if (op_data_requested_) {
      op_data_requested_ = false;
      if (op_data_active_)
        ESP_LOGW(TAG, "op-data due but previous handshake still active (%u ms in); skipping",
                 (unsigned) (millis() - op_data_started_ms_));
      else
        op_data_pending_ = true;
    }
    return;
  }

  // RSR → operational data handshake / response
  if (is_rsr) {
    if (buf[3] == '2') {
      handle_op_data_not_ready_(buf);
    } else if (buf[3] == '1') {
      const uint32_t now = millis();
      ESP_LOGD(TAG, "op-data ready after %u RSR2 retries (%u ms); reply latency %u-%u ms, echo delay %u ms",
               (unsigned) rsr2_retries_, (unsigned) (now - op_data_started_ms_),
               (unsigned) op_data_reply_min_ms_, (unsigned) op_data_reply_max_ms_,
               (unsigned) op_data_echo_delay_ms_);
      op_data_active_ = false;
      op_data_echo_scheduled_ = false;
      parse_operational_data(buf, buflen);
    }
    return;
  }

  ESP_LOGD(TAG, "rx unhandled: %s", buf);
}


bool RcEx3Climate::validate_checksum_and_extract_payload_(const char *raw, size_t len, char *payload,
                                                          size_t payload_size, size_t &payload_len) {
  payload_len = 0;
  if (len < 3) {
    ESP_LOGW(TAG, "rx frame too short for checksum");
    return false;
  }

  const size_t body_len = len - 2;
  const char rx_hi = raw[body_len];
  const char rx_lo = raw[body_len + 1];
  if (!isxdigit(static_cast<uint8_t>(rx_hi)) || !isxdigit(static_cast<uint8_t>(rx_lo))) {
    ESP_LOGW(TAG, "rx frame missing checksum hex");
    return false;
  }

  char rx_sum_hex[3] = {rx_hi, rx_lo, '\0'};
  uint8_t rx_sum = static_cast<uint8_t>(strtol(rx_sum_hex, nullptr, 16));
  uint8_t calc_sum = calc_checksum(raw, body_len);
  if (rx_sum != calc_sum) {
    ESP_LOGW(TAG, "rx checksum mismatch: got=%02X expected=%02X", rx_sum, calc_sum);
    return false;
  }

  payload_len = (body_len < (payload_size - 1)) ? body_len : (payload_size - 1);
  memcpy(payload, raw, payload_len);
  payload[payload_len] = '\0';
  return true;
}

// ─── Status response parser ───────────────────────────────────────────────────
//
//   [0-3]  "RSSL"
//   [4]    '1'
//   [13]   power  ('0'=off, '1'=on)
//   [17]   mode   ('0'=auto,'1'=dry,'2'=cool,'3'=fan,'4'=heat)
//   [21]   fan    ('0'=spd1,'1'=spd2,'2'=spd3,'6'=spd4,other=auto)
//   [30-31] temp  (2 hex chars, value * 0.5 = °C)

bool RcEx3Climate::parse_status_response(const char *buf, size_t len) {
  if (len < 32)
    return false;

  char pwr_c  = buf[13];
  char mode_c = buf[17];
  char fan_c  = buf[21];

  if ((pwr_c != '0' && pwr_c != '1') || mode_c < '0' || mode_c > '4' ||
      !isxdigit(static_cast<uint8_t>(buf[30])) || !isxdigit(static_cast<uint8_t>(buf[31]))) {
    ESP_LOGW(TAG, "status: unexpected field values, ignoring: %s", buf);
    return false;
  }

  char tmp[3] = {buf[30], buf[31], '\0'};
  unsigned int raw_temp = static_cast<unsigned int>(strtol(tmp, nullptr, 16));
  float temp_c = raw_temp * 0.5f;
  // The additive checksum misses swapped characters (2C ↔ C2 = 22 ↔ 97 °C), so
  // reject implausible setpoints rather than store and later resend them.
  if (temp_c < TEMP_MIN_C - 6.0f || temp_c > TEMP_MAX_C + 5.0f) {
    ESP_LOGW(TAG, "status: implausible setpoint %.1f°C, ignoring: %s", temp_c, buf);
    return false;
  }

  ESP_LOGD(TAG, "status: power=%c mode=%c fan=%c temp=%.1f°C", pwr_c, mode_c, fan_c, temp_c);
  this->status_received_ = true;

  // Commands are built from these fields when sent, and a reply soon after a
  // command may predate it: don't let it overwrite a queued or unconfirmed HA
  // change. The confirming poll is sent after cmd_confirm_pending_ clears.
  if (command_pending_ || cmd_confirm_pending_) {
    ESP_LOGD(TAG, "status not applied: HA command awaiting confirmation");
    return true;
  }

  this->mode = (pwr_c == '1') ? wire_to_climate_mode(mode_c - '0') : climate::CLIMATE_MODE_OFF;
  const char *custom_fan = wire_to_custom_fan_mode(fan_c);
  if (custom_fan != nullptr)
    this->set_custom_fan_mode_(custom_fan);
  else
    this->set_fan_mode_(climate::CLIMATE_FAN_AUTO);
  this->target_temperature = temp_c;
  if (std::isnan(this->current_temperature) && indoor_temperature_sensor_ &&
      !std::isnan(indoor_temperature_sensor_->state)) {
    this->current_temperature = indoor_temperature_sensor_->state;
  }
  this->publish_state();
  return true;
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
  float outdoor_air = data[idx(POS_OUTDOOR_AIR_TEMP)] / 4.0f - 22.0f;
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

  last_op_data_ms_ = op_data_cycle_ms_;
  op_data_ever_received_ = true;
  this->current_temperature = indoor_air;
  this->publish_state();
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
  ESP_LOGV(TAG, "tx: %.*s%s", (int) len, payload, hex_sum);

  this->write_byte(0x02);
  for (size_t i = 0; i < len; i++)
    this->write_byte(static_cast<uint8_t>(payload[i]));
  this->write_byte(static_cast<uint8_t>(hex_sum[0]));
  this->write_byte(static_cast<uint8_t>(hex_sum[1]));
  this->write_byte(0x03);
}

void RcEx3Climate::send_status_request() {
  if (op_data_active_)
    ESP_LOGW(TAG, "status poll during op-data handshake (%u ms in, %u retries)",
             (unsigned) (millis() - op_data_started_ms_), (unsigned) rsr2_retries_);
  const char *query = "RSSL12FF0001FF02FF03FF04FF05FF06FF0FFF43FF25";
  ESP_LOGV(TAG, "tx: %s", query);
  this->write_byte(0x02);
  for (const char *p = query; *p; p++)
    this->write_byte(static_cast<uint8_t>(*p));
  this->write_byte(0x03);
  begin_tx_(TxKind::STATUS);
  status_sent_ms_ = millis();
}

// ─── Op-data handshake ───────────────────────────────────────────────────────
//
// RSR2 means "not ready": echo RSR20000E9 and the unit eventually answers RSR1.
// Deliberately unbounded: the unit can take ~40 s (1800+ immediate echoes) to
// become ready, and abandoning the handshake part-way appeared to leave the
// panel ignoring commands. op_data_echo_delay only paces the echoes.

void RcEx3Climate::handle_op_data_not_ready_(const char *buf) {
  const uint32_t now = millis();
  const uint32_t latency = now - op_data_last_tx_ms_;
  if (!op_data_active_) {
    // Late reply after the stall detector gave up: resume rather than leave the
    // unit mid-handshake.
    ESP_LOGW(TAG, "op-data not ready reply after handshake ended (%u ms since last request); resuming",
             (unsigned) latency);
    op_data_active_ = true;
  }
  if (rsr2_retries_ == 0) {
    ESP_LOGD(TAG, "op-data not ready (first reply after %u ms): %s", (unsigned) latency, buf);
    op_data_reply_min_ms_ = op_data_reply_max_ms_ = latency;
  } else {
    if (latency < op_data_reply_min_ms_) op_data_reply_min_ms_ = latency;
    if (latency > op_data_reply_max_ms_) op_data_reply_max_ms_ = latency;
  }
  rsr2_retries_++;
  op_data_rsr2_rx_ms_ = now;
  op_data_last_reply_ms_ = now;
  op_data_echo_scheduled_ = true;  // sent by service_tx_() once the delay elapses
}

void RcEx3Climate::service_op_data_handshake_() {
  if (!op_data_active_)
    return;
  const uint32_t now = millis();

  if (inflight_ == TxKind::COMMAND)
    return;  // waiting behind a command

  if (now - op_data_last_progress_ms_ >= OP_DATA_PROGRESS_LOG_MS) {
    op_data_last_progress_ms_ = now;
    ESP_LOGD(TAG, "op-data still not ready: %u retries, %u ms", (unsigned) rsr2_retries_,
             (unsigned) (now - op_data_started_ms_));
  }

  // Lost replies are echoed again (service_tx_()); stop only once the unit has
  // been silent this long.
  const uint32_t last_heard = rsr2_retries_ > 0 ? op_data_last_reply_ms_ : op_data_started_ms_;
  if (now - last_heard >= OP_DATA_STALL_MS) {
    ESP_LOGW(TAG, "op-data handshake stalled: no reply for %u ms (%u retries, %u ms in)",
             (unsigned) (now - last_heard), (unsigned) rsr2_retries_,
             (unsigned) (now - op_data_started_ms_));
    op_data_active_ = false;
    op_data_echo_scheduled_ = false;
  }
}

void RcEx3Climate::send_operational_data_request(bool second_page) {
  const uint32_t now = millis();
  if (!second_page) {
    op_data_started_ms_ = now;
    op_data_last_progress_ms_ = now;
    rsr2_retries_ = 0;
    op_data_active_ = true;
    op_data_echo_scheduled_ = false;
  }
  op_data_last_tx_ms_ = now;
  const char *query = second_page ? "RSR20000E9" : "RSR10000E8";
  ESP_LOGV(TAG, "tx: %s", query);
  this->write_byte(0x02);
  for (const char *p = query; *p; p++)
    this->write_byte(static_cast<uint8_t>(*p));
  this->write_byte(0x03);
  begin_tx_(TxKind::OP_DATA);
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

// Custom fan modes "1".."4" ↔ wire speeds 0/1/2/6; anything else is auto (7).
uint8_t RcEx3Climate::custom_fan_mode_to_wire(StringRef mode) {
  if (mode == "1") return 0x00;
  if (mode == "2") return 0x01;
  if (mode == "3") return 0x02;
  if (mode == "4") return 0x06;
  return 0x07;
}

const char *RcEx3Climate::wire_to_custom_fan_mode(char c) {
  switch (c) {
    case '0': return "1";
    case '1': return "2";
    case '2': return "3";
    case '6': return "4";
    default:  return nullptr;
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
