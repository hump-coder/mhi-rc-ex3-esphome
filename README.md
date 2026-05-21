# MHI RC-EX3 — ESPHome Integration for Home Assistant

Native ESPHome firmware for an **ESP32** that bridges a **Mitsubishi Heavy Industries (MHI) RC-EX3 / RC-EXZ3A HVAC wall controller** into Home Assistant as a first-class `climate` entity — no MQTT broker, no custom integration, no YAML to manage after initial setup.

Based on the reverse-engineering work from [mcchas/rc-ex3-esp](https://github.com/mcchas/rc-ex3-esp) and [hump-coder/mhi-rc-ex3-esp32](https://github.com/hump-coder/mhi-rc-ex3-esp32) (Arduino + MQTT predecessor). This project replaces that stack with ESPHome's native HA API.

---

## Compatible devices

The RC-EX3 panel is fitted to most MHI ducted and split-system indoor units, including:

- SRK / SRR / SRF families
- FDTC / FDUM / FDEN / FDT / FDE families
- **FDUA** (the unit this hardware was built and tested on)

---

## What you get in Home Assistant

- Full **climate entity**: on/off, mode (Auto/Cool/Heat/Dry/Fan), fan speed (Auto/1/2/3/4), target temperature (16–30 °C in 0.5 °C steps)
- **Diagnostic sensors** (all optional): indoor temperature, outdoor temperature, return-air temperature, compressor frequency (Hz), compressor run-hours, indoor fan speed (raw), outdoor fan speed (raw)
- OTA firmware updates via the ESPHome dashboard
- Automatic reconnect / fallback AP if Wi-Fi drops

---

## Hardware

### Compatible boards

This project works with virtually any ESP32 board — the only requirements are 3.3 V I/O and two free GPIO pins for UART. The YAML is pre-configured for the **LOLIN S2 Mini**, but switching boards is a one-section edit (see [Board configuration](#board-configuration) below).

Popular options:

| Board | Notes |
|-------|-------|
| **ESP32-WROOM-32 DevKit** (30 or 38 pin) | The most common "ESP32" on AliExpress; easy to breadboard and widely documented |
| **LOLIN/Wemos D1 Mini ESP32** | Compact double-row format; great if space is tight |
| **LOLIN S2 Mini** (ESP32-S2) | Pre-configured in this YAML; native USB, no USB-UART chip needed |
| **ESP32-C3 Super Mini** | Very cheap (~$2 on AliExpress); tiny form factor — note different GPIO pins, see below |
| **ESP32-S3 DevKit / Super Mini** | Works fine; use `esp32-s3-devkitc-1` or the board ID for your specific module |

Any other ESP32, ESP32-S2, ESP32-S3, or ESP32-C3 board will also work — pick any two free GPIO pins for UART.

### What you need

- Any ESP32 board from the table above (or similar)
- Small **3.3 V buck converter** (e.g. MP1584 or similar AliExpress module) — a linear regulator draws too much quiescent current from the panel rail
- **470 µF / 10 V capacitor** across the 3.3 V and GND rails (the ESP32 can pull transient current spikes that brown-out the panel)
- 4 × jumper wires or bent header pins

### Wiring

The RC-EX3 PCB has four through-hole vias accessible without disassembling the unit:

| Panel via | ESP32 pin  | Notes                          |
|-----------|-----------|--------------------------------|
| VCC       | Buck converter IN → 3.3 V OUT → ESP32 3V3 | Do **not** use a linear reg |
| GND       | GND        |                                |
| TX        | **GPIO16** (UART RX) | Panel → ESP32     |
| RX        | **GPIO17** (UART TX) | ESP32 → Panel     |

Serial parameters: **38400 baud, 8E1** (8 data bits, Even parity, 1 stop bit) — 3.3 V TTL, no level shifting needed.

GPIO16 / GPIO17 work on most boards. For the ESP32-C3 Super Mini those pins are not broken out — use GPIO4 / GPIO5 instead (see Board configuration below).

### Board configuration

Only the `esp32:` block and `uart:` pins need to change between boards. Everything else in the YAML stays the same.

**ESP32-WROOM-32 / generic ESP32 DevKit** (most AliExpress "ESP32" boards):

```yaml
esp32:
  board: esp32dev
  framework:
    type: arduino
```

**LOLIN/Wemos D1 Mini ESP32:**

```yaml
esp32:
  board: wemos_d1_mini32
  framework:
    type: arduino
```

**LOLIN S2 Mini (ESP32-S2)** — pre-configured, no change needed:

```yaml
esp32:
  board: esp32-s2-saola-1
  framework:
    type: arduino
```

**ESP32-C3 Super Mini / DevKitM-1:**

```yaml
esp32:
  board: esp32-c3-devkitm-1
  framework:
    type: arduino
```

Also update the UART pins — GPIO16/17 are not available on the C3 Super Mini. Use GPIO4 (RX) and GPIO5 (TX):

```yaml
uart:
  id: rc_ex3_uart
  rx_pin: GPIO4
  tx_pin: GPIO5
  baud_rate: 38400
  parity: EVEN
  stop_bits: 1
  data_bits: 8
```

### Photo guide

Overview of the RC-EX3 panel and ESP module placement:

[<img src="https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/rc3-overview.png" width="60%"/>](https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/rc3-overview.png)

Buck converter — solder ground and positive leads to the regulator:

[<img src="https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/buck.png" width="60%"/>](https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/buck.png)

Regulator placement inside the enclosure (ESP module sits flat against the PCB once insulated):

[<img src="https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/rc3-regulator-placement.png" width="60%"/>](https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/rc3-regulator-placement.png)

TTL UART connections — jumper pins pressed into the four through-hole vias:

[<img src="https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/rc3-ttl-uart.png" width="60%"/>](https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/rc3-ttl-uart.png)

Regulator power connections:

[<img src="https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/rc3-regulator-power.png" width="60%"/>](https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/rc3-regulator-power.png)

### Capacitor

The ESP32 can draw current spikes that the panel's power rail can't handle cleanly. Add a **470 µF / 10 V capacitor** across GND and the 3.3 V rail (before or after the buck converter):

[<img src="https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/capacitor.jpg" width="60%"/>](https://raw.githubusercontent.com/hump-coder/mhi-rc-ex3-esp32/main/images/capacitor.jpg)

---

## Quick start

**Full step-by-step instructions are in [DEPLOY.md](DEPLOY.md).** Short version:

1. Install the **ESPHome add-on** in Home Assistant (Supervisor → Add-on Store).
2. Open the ESPHome dashboard and create a new device.
3. Replace the generated YAML with the contents of [`mhi-rc-ex3.yaml`](mhi-rc-ex3.yaml).
4. Add your credentials to `secrets.yaml` (Wi-Fi SSID/password, API key, OTA password).
5. Plug the ESP32 into your computer's USB port and flash via the ESPHome web flasher.
6. From then on, all updates are OTA.

The component is fetched automatically from this GitHub repo at compile time — no local files to manage.

---

## Configuration notes

The YAML is pre-configured for the hardware described above. The relevant settings:

```yaml
uart:
  rx_pin: GPIO16   # Panel TX → ESP32
  tx_pin: GPIO17   # Panel RX ← ESP32
  baud_rate: 38400
  parity: EVEN
  stop_bits: 1
  data_bits: 8

climate:
  - platform: rc_ex3
    update_interval: 5min  # Status poll period; reduce if you need faster updates
```

If you're using a different ESP32 board and want to reassign pins, change `rx_pin` / `tx_pin` here. The baud rate and parity must stay at 38400 / EVEN — this is fixed by the RC-EX3 panel.

---

## Project structure

```
mhi-rc-ex3-esphome/
├── mhi-rc-ex3.yaml          # Main ESPHome config — paste into the ESPHome dashboard
├── secrets.yaml             # Your credentials (gitignored)
├── DEPLOY.md                # Full deployment walkthrough
├── PROJECT.md               # Protocol spec and architecture notes
└── components/
    └── rc_ex3/
        ├── __init__.py      # ESPHome namespace declaration
        ├── climate.py       # Component schema + sensor wiring
        ├── rc_ex3.h         # C++ class declaration and protocol constants
        └── rc_ex3.cpp       # Full protocol implementation
```

---

## Credits

- [mcchas/rc-ex3-esp](https://github.com/mcchas/rc-ex3-esp) — original reverse-engineering of the RC-EX3 serial protocol
- [hump-coder/mhi-rc-ex3-esp32](https://github.com/hump-coder/mhi-rc-ex3-esp32) — Arduino/MQTT fork with hardware photos and FDUA-confirmed wiring
