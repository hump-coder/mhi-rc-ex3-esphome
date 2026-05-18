# Deployment Guide — MHI RC-EX3 ESPHome Integration

## Prerequisites

- Home Assistant with the **ESPHome add-on** installed (Add-on Store → ESPHome)
- ESP32-WROOM-32 (or compatible ESP32 dev board)
- 3.3 V buck converter (e.g. MP1584 module) — do **not** use a linear regulator; the panel draws too much current
- 4 dupont/jumper wires
- USB cable for first flash

---

## 1. Hardware Wiring

Locate the four through-hole vias on the RC-EX3 PCB (see the `images/` folder in the [upstream repo](https://github.com/mcchas/rc-ex3-esp) for photos).

| RC-EX3 via | ESP32 pin      | Notes                              |
|------------|----------------|------------------------------------|
| VCC        | Buck out 3.3 V | Power the ESP32 from the same buck |
| GND        | GND            |                                    |
| TX         | GPIO16 (RX2)   | Panel transmits → ESP32 receives   |
| RX         | GPIO17 (TX2)   | Panel receives ← ESP32 transmits   |

> **TX/RX are crossed** — connect the panel's TX pin to the ESP32's RX pin and vice versa.

The ESP32 is powered by the buck converter drawing from the panel's VCC rail. If you prefer to power the ESP32 separately (USB or external 5 V), leave VCC unwired and connect only GND, TX, and RX.

---

## 2. Create the Device in ESPHome Dashboard

1. Open the **ESPHome** add-on in HA and click **+ New Device**
2. Give it a name (e.g. `mhi-rc-ex3`) and click **Next**
3. Select **ESP32** as the device type
4. When it asks to install, choose **Skip** for now
5. Click **Edit** on the new device card
6. Replace the entire contents with the YAML from [mhi-rc-ex3.yaml](https://github.com/hump-coder/mhi-rc-ex3-esphome/blob/main/mhi-rc-ex3.yaml) in this repo
7. Click **Save**

---

## 3. Configure Credentials

In the ESPHome dashboard, open the **Secrets** editor (the key icon, top-right) and add:

```yaml
wifi_ssid: "YourWiFiSSID"
wifi_password: "YourWiFiPassword"
ap_password: "rc-ex3-setup"
api_encryption_key: "GENERATE_ONE_BELOW"
ota_password: "choose-something"
```

To generate the API key, open the ESPHome add-on terminal (or any terminal with ESPHome installed) and run:

```bash
esphome generate-api-key
```

Paste the output as the `api_encryption_key` value.

---

## 4. First Flash (USB)

The very first flash must be via USB — connect the ESP32 to the machine running HA (or use a laptop with ESPHome CLI if HA is on a remote server).

In the ESPHome dashboard, click **Install** on the device card, then choose **Plug into this computer** (if HA has USB access) or **Manual download** to get the `.bin` and flash it with the [ESPHome Web Flasher](https://web.esphome.io) from any browser.

ESPHome will compile the firmware (~2–3 minutes first time, the component code is fetched automatically from GitHub) and flash the device.

---

## 4. Verify Serial Communication

Once booted and connected to WiFi, watch the log for these lines:

```
[D][rc_ex3] status: power=0 mode=2 fan=7 temp=22.0°C
```

If you see this within ~30 seconds of boot, the wiring and protocol are working.

If you see nothing from `rc_ex3` after 60 seconds:
- Double-check TX/RX are not swapped
- Confirm the buck converter is outputting 3.3 V
- Try swapping GPIO16 and GPIO17 in the YAML (some panels have reversed TX/RX labelling)

---

## 5. Add to Home Assistant

1. In Home Assistant go to **Settings → Devices & Services → Add Integration**
2. Search for **ESPHome**
3. Enter the device's IP address (shown in the ESPHome log on boot) or hostname `mhi-rc-ex3.local`
4. Enter the API encryption key from `secrets.yaml` when prompted

Home Assistant will discover:
- **Climate entity** — `climate.living_room_ac` (mode, fan speed, target temp, current temp)
- **Sensor entities** — indoor/outdoor temp, compressor frequency, compressor hours, fan speeds (if configured in the YAML)

---

## 6. Subsequent Updates (OTA)

After the first USB flash, all future updates are wireless. In the ESPHome dashboard, click **Install → Wirelessly** on the device card. No USB connection needed.

---

## 7. Reduce Log Verbosity in Production

Once everything is confirmed working, edit the device in the ESPHome dashboard and change:

```yaml
logger:
  level: WARN
```

Then **Install → Wirelessly** to apply.

---

## 8. Fallback / Recovery

If the device loses WiFi or OTA fails, it will broadcast a fallback access point:

- **SSID**: `MHI-RC-EX3 Fallback`
- **Password**: value of `ap_password` in `secrets.yaml`

Connect to that AP, navigate to `http://192.168.4.1`, and use the captive portal to reconfigure WiFi or trigger a re-flash.

If the device is completely unresponsive, re-flash via USB.
