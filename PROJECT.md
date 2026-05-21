# Project Context — MHI RC-EX3 ESPHome Integration

## What This Is

An [ESPHome](https://esphome.io) firmware project for an ESP32 that bridges a **Mitsubishi Heavy Industries (MHI) RC-EX3 HVAC wall controller** into Home Assistant as a native `climate` entity.

The RC-EX3 is a wired wall panel (not a handheld IR remote) found on MHI ducted and split systems. It communicates with the indoor unit over a proprietary serial bus. This project taps into that bus and speaks the same protocol, making the AC unit fully controllable and observable from Home Assistant without modifying the AC unit itself.

The upstream reverse-engineering work this is based on: https://github.com/mcchas/rc-ex3-esp  
That project targets the same hardware but uses a different firmware stack (Arduino + MQTT + WiFiManager). This project replaces all of that with ESPHome's native HA integration.

---

## Hardware Interface

The RC-EX3 PCB exposes four through-hole vias:

| Signal | Voltage | Description                        |
|--------|---------|------------------------------------|
| VCC    | 3.3 V   | Panel power rail                   |
| GND    | 0 V     |                                    |
| TX     | 3.3 V TTL | Panel → controller (we receive) |
| RX     | 3.3 V TTL | Controller → panel (we transmit)|

The ESP32 connects directly (no level shifting needed — it's already 3.3 V TTL). The original project warns against linear regulators for powering the ESP from this rail due to current draw; a small buck converter is required if powering from the panel.

---

## Serial Protocol

**Parameters:** 38400 baud, 8 data bits, Even parity, 1 stop bit (8E1)

All packets are framed:
```
0x02  [ASCII payload]  [2-char hex checksum]  0x03
```

The checksum is the 8-bit sum of all payload bytes, printed as two uppercase hex ASCII characters (e.g. sum=0x25 → `"25"`). The receiver strips `0x02`/`0x03`, finds the first `R` character, and keeps only printable ASCII from there — this is the "filtered" string that byte-position parsing is applied to.

### Control Commands

All control uses a single combined packet (mirrors the upstream `setClimate()` function):

```
RSSL13FF0001[pwr]02[mode]03[fan]04FF0503[temp]06FF0FFF43FF
```

Where each `[field]` is a 2-char lowercase hex byte:

| Field  | Values                                        |
|--------|-----------------------------------------------|
| `pwr`  | `00`=off, `01`=on                             |
| `mode` | `00`=auto, `01`=dry, `02`=cool, `03`=fan, `04`=heat |
| `fan`  | `00`=spd1, `01`=spd2, `02`=spd3, `06`=spd4, `07`=auto |
| `temp` | `actual_°C × 2` as hex (e.g. 22°C → `0x2C`)  |

### Status Query

A fixed packet with a pre-calculated embedded checksum (0x25):
```
0x02  RSSL12FF0001FF02FF03FF04FF05FF06FF0FFF43FF25  0x03
```

Response (filtered ASCII, positions 0-indexed from first `R`):

| Position | Meaning                                    |
|----------|--------------------------------------------|
| [13]     | Power: `'0'`=off, `'1'`=on                |
| [17]     | Mode: `'0'`–`'4'` (auto/dry/cool/fan/heat)|
| [21]     | Fan speed: `'0'`/`'1'`/`'2'`/`'6'`/other=auto |
| [30–31]  | Target temp as 2 hex chars; decode: `value × 0.5 = °C` |

### Operational Data Query

Requests a binary diagnostic data blob from the unit:
```
0x02  RSR10000E8  0x03
```

If the unit responds with `RSR2...`, a follow-up `RSR20000E9` is required.  
If the unit responds with `RSR1...`, the rest is hex-encoded binary data.

After stripping the 4-char `RSR1` header, the binary blob is decoded. Confirmed byte positions (from upstream reverse engineering):

| Position | Meaning                          | Decoding                        |
|----------|----------------------------------|---------------------------------|
| 9        | Indoor air temperature           | `int8_t` → °C                  |
| 26       | Outdoor air temperature          | `(uint8_t / 4) - 22` → °C      |
| 27       | Return air temperature           | `uint8_t / 4` → °C             |
| 32       | Compressor frequency             | raw uint8_t → Hz                |
| 44       | Compressor hours (MSB)           | combined with LSB × 100         |
| 45       | Compressor hours (LSB) / indoor fan speed | ⚠ aliased — see below |

> **⚠ Uncertainty:** Several byte positions are marked `// ?` in the original source because they were not fully confirmed. Notably, `POS_COMPRESSOR_HZ` and `POS_OUTDOOR_FAN_SPEED` both resolve to byte 32, and `POS_INDOOR_FAN_SPEED` and `POS_COMPRESSOR_HOURS_LSB` both resolve to byte 45. These fields may read the same hardware value until tested on a real unit.

---

## ESPHome Architecture Choices

### External Component (not a PR to ESPHome core)

The component lives in `components/rc_ex3/` and is loaded via `external_components:` in the YAML using `type: git`, pointing to `https://github.com/hump-coder/mhi-rc-ex3-esphome`. ESPHome fetches and caches it automatically at compile time — no local file management needed. A user only needs the YAML and secrets in the ESPHome dashboard. The component could be submitted upstream to ESPHome core later.

### Single C++ Class — `RcEx3Climate`

The class inherits from three ESPHome base classes:

- `climate::Climate` — exposes the HA climate entity (modes, fan, temp setpoint)
- `uart::UARTDevice` — owns the UART peripheral and provides `read_byte`/`write_byte`
- `PollingComponent` — drives the periodic status poll via `update()`

This is the standard pattern for ESPHome HVAC components (e.g. Mitsubishi, Daikin).

### Non-Blocking RX via State Machine

The `loop()` method accumulates incoming bytes into `rx_buf_[]` using a two-state machine (`WAITING_FOR_SOF` / `READING_PAYLOAD`). A complete packet is only dispatched to `parse_packet()` when `0x03` (EOF) is seen. No `delay()` calls anywhere — the ESP32 main loop is never blocked.

### Two-Phase Polling

Each polling cycle (default 30 s) does two serial transactions:

1. **Status query** → fired immediately in `update()`
2. **Operational data query** → fired on the next `loop()` tick after the status response arrives (via `op_data_pending_` flag)

This avoids sending both requests simultaneously and overlapping their responses. The operational data is always requested (not just when sensors are configured), so `current_temperature` in the HA climate card is always populated.

### Combined Control Packet

When Home Assistant sends a control action, all four parameters (power, mode, fan, temperature) are packed into a single `RSSL13...` packet rather than sending individual field-update packets. This matches the upstream `setClimate()` approach and ensures the unit always receives a coherent state.

### Fan Speed Mapping

The protocol has 4 discrete speeds plus auto. ESPHome's built-in `ClimateFanMode` only covers Auto/Low/Medium/High, so speeds 1–4 are exposed as ESPHome *custom fan modes* (`"1"`–`"4"`). Auto remains a standard built-in mode.

| HA fan mode       | Wire value | Protocol speed |
|-------------------|-----------|----------------|
| AUTO (built-in)   | 0x07      | Auto           |
| "1" (custom)      | 0x00      | Speed 1        |
| "2" (custom)      | 0x01      | Speed 2        |
| "3" (custom)      | 0x02      | Speed 3        |
| "4" (custom)      | 0x06      | Speed 4        |

On receive, `apply_wire_fan_mode()` maps wire chars `'0'`–`'2'`/`'6'` to custom modes and clears `fan_mode`, or sets `CLIMATE_FAN_AUTO` and clears `custom_fan_mode` for any other value.

### Diagnostic Sensors

Seven optional sensors can be declared inside the `climate:` block in YAML. They are wired to `sensor::Sensor*` pointers on the class via `set_*_sensor()` methods generated by `climate.py`. If a pointer is null the corresponding `publish_state()` call is skipped — no runtime overhead for unconfigured sensors.

---

## File Structure

```
MHI-RC-EX3/
├── mhi-rc-ex3.yaml              # Main ESPHome config — edit board/pins/names here
├── secrets.yaml                 # Credentials (gitignored)
├── DEPLOY.md                    # Step-by-step flashing and HA setup guide
├── PROJECT.md                   # This file
├── .gitignore
└── components/
    └── rc_ex3/
        ├── __init__.py          # ESPHome namespace declaration
        ├── climate.py           # Component schema + sensor wiring (Python codegen)
        ├── rc_ex3.h             # C++ class declaration, protocol constants, structs
        └── rc_ex3.cpp           # Full protocol implementation
```

---

## What Is Not Implemented

- **Off-timer** — the protocol supports a 1–12 hour delay-off timer (`RSJ802...`). Not exposed because ESPHome has no standard timer slot in the climate platform.
- **Swing / vane control** — not present in the RC-EX3 protocol.
- **Current draw sensor** — byte position 42 in the operational data, marked uncertain in the original source. Omitted until confirmed on hardware.
- **EEV opening** — bytes 46–47, position confirmed in upstream source but low diagnostic value. Easy to add as an additional sensor if needed.
