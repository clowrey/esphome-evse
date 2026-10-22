# esphome-evse
EVSE running entirely on ESP32 MCU ESPHome YAML

BETA firmware. Use at your own risk.
- No RCD.
- The S3 package includes PV auto charge (Normal / Fast), a low-voltage lockout, and the local LCD.
- Home Assistant supplies the battery power, voltage, and state-of-charge sensors those modes use.

A device config sets the name, Wi-Fi, API and OTA credentials, and the substitutions below. Everything else is pulled from this repo:

- `components/evse_cp` is the control-pilot state machine (external component).
- `components/evse_charge_control` is the Normal/Fast charge-current controller. Its core (`charge_controller.h/.cpp`) has no ESPHome dependencies and is replayed against recorded data by the tests in `test/charge_control`.
- `packages/s3-evse.yaml` is the entry point; it includes `packages/s3/board.yaml` (board, Wi-Fi, API, time), `cp-pilot.yaml` (pilot sensing, PWM, relay, offered current), `metering.yaml` (CT current, voltage, power, daily energy), `charge-control.yaml` (battery inputs, controller, modes, status) and `lcd.yaml` (display).

## How charging is controlled

Charger Mode is Stop, Manual, Normal or Fast. Manual charges at Current Setpoint. Normal and Fast hold the house-battery power at a target by stepping Current Setpoint between 6 A and Max Current Limit:

- The battery power is the sum of the configured Home Assistant power entities (positive = charging the house battery). Steps are judged on readings taken after the previous step: the controller waits a settle time (8 s, or 2.5 update intervals of a slow sensor, or 40 s on the fallback input) and three ticks of agreement, then steps by about 0.8 A per amp of error (at most 4 A up, 8 A down). The hold band is 0.25 kW or 1.5 A worth of power, whichever is wider, so one step always lands inside it.
- Normal Target (default −1 kW) holds about 1 kW of battery discharge: with PV surplus the car ramps all the way up to Max Current Limit; at night it sits at 6 A. Normal never pauses.
- Fast Target (default −3 kW) holds about 3 kW of discharge. If the battery is still over target with the car at 6 A for a minute, Fast pauses (pilot off) and resumes once the house alone leaves room to restart at 6 A.
- A current set from Home Assistant or the LCD is taken as the new starting point ("user set 40A" event) and the controller continues from there.
- Battery Target Min SOC stops charging (resume 1 % higher) and reduces the current limit in the 10 % above it. Battery Low Voltage Cutoff (0 = off) latches a lockout after 30 s below it; Charger Enabled clears it once the voltage is back above the cutoff.
- If a power input stops updating (30 s) the fallback input takes over; with no usable input the current is held, never raised, and drops to 6 A after 10 minutes. `Battery Power Source` shows which input is in use.

The tuning lives in the `Charge Control *` numbers (deadband, gain, max step up/down, settle times, evidence samples, filter alpha, pause/resume, stale time, blind timeout). `Event Flag` records one line per controller action; `HA Inputs Debug` is a one-line dump of everything the controller sees.

`esphome-evse.yaml` is a starting device config. Point `packages:` at this repo:

```yaml
packages:
  s3_evse:
    url: https://github.com/clowrey/esphome-evse.git
    ref: main
    files:
      - packages/s3-evse.yaml
    refresh: 0s
```

`secrets.yaml` needs `wifi_ssid`, `wifi_password`, `ota_password`, and `api_encryption_key`. Set `wifi.use_address` when OTA should target an IP the device already has.

The package uses these defaults when a device file leaves a substitution out. Values marked first-boot are used once; later changes are saved on the device.

- `name` — hostname, for example `evse-0`. Entity ids use it with the hyphen turned into an underscore (`sensor.evse_0_power`). Leave this as `evse-0`, `evse-1`, and so on if you will have more than one EVSE. You can rename the device in Home Assistant afterward; that does not change the entity ids. Changing `name` in the YAML makes Home Assistant see a new device.
- `device_description` — ESPHome device comment.
- `firmware_ref` — git ref (branch or tag) of this repo that the device builds from. Use it in the device file's `packages: ref: ${firmware_ref}`. `components_source` is the matching external-components source (default `github://clowrey/esphome-evse@${firmware_ref}`); point it at `../esphome-evse/components` to build from a local checkout.
- `ha_battery_power_id` — house battery power entity, signed: positive is charging the house battery, negative is discharging. Normal and Fast modes step the charge current from this sensor.
- `ha_battery_power_2_enabled` / `ha_battery_power_2_id` — `1` adds a second power entity that is summed with the first (for example one sensor per battery).
- `ha_battery_power_fallback_enabled` / `ha_battery_power_fallback_id` — `1` names a slower power entity to use while one of the inputs above is unavailable or stale.
- `ha_battery_power_divisor` — divide those sensors by this to get kilowatts. Use `1000` when they are watts, and `1` when they are already kilowatts.
- `line_voltage_nominal` — supply voltage used to size the control steps when the EVSE's own voltage reading is unavailable (default 230).
- `ha_battery_voltage_id` — house battery voltage entity. Charging locks out after 30 seconds at or below the cutoff (47 V unless changed on the device).
- `ha_battery_soc_id` — house battery state of charge, percent. Current is reduced in the 10% above the minimum SOC. At or below that minimum, Normal and Fast stop charging until SOC is 1% above it. With two batteries, the lower SOC is used.
- `ha_battery_2_enabled` — `0` ignores a second battery. `1` includes `ha_battery_2_soc_id`.
- `ha_battery_2_soc_id` — second house battery state of charge. Used only when `ha_battery_2_enabled` is `1`.
- `current_cal_0` .. `current_cal_4` — CT clamp calibration, `raw reading -> amps`.
- `max_current_initial` — first-boot Max Current Limit, amps. This is the ceiling. Current Setpoint is what the car is offered, and Normal and Fast step that setpoint between 6 A and this limit. 6 A is the floor because 5 A showed up as 0 A on a Leaf. After first boot, change Max Current Limit on the device.
- `normal_setpoint_a` — LCD Normal button only. It selects Normal and sets Current Setpoint to this many amps, which is the floor the ramp starts from. Auto then raises it when the house battery is charging and lowers it when discharge passes the Normal target. The Home Assistant Normal button changes mode only.
- `fast_setpoint_a` — LCD Fast button only. It selects Fast and jumps Current Setpoint to this many amps. Auto then moves it to hold house-battery discharge near the Fast target, still between 6 A and Max Current Limit. If it is already at 6 A and the battery keeps discharging, Fast pauses. The Home Assistant Fast button changes mode only.
- `electricity_cost_initial` — first-boot electricity price, USD per kWh, for the daily cost on the LCD.
- `car_battery_kwh_initial` — first-boot car battery size, kWh. Daily energy is also shown as a percent of this.

Host tests: `test/charge_control/build.sh` (Linux or WSL) or `build.ps1` (MinGW) builds `scenarios` (synthetic checks) and `replay` (recorded-data replay, see `test/charge_control/data/README.md`).

Upgrading from the single-file package: the `PV Auto *` numbers, the `Auto Mode` switch and the `HA Battery * Read` sensors are gone (delete them in Home Assistant), the `Charge Control *` numbers and `Battery Power Source` are new, and `HA Battery Power` now reports the summed kilowatts.

`dashboard-evse.yaml` is a Home Assistant dashboard for `name: evse-0`. It uses those entity ids (`sensor.evse_0_power`, `number.evse_0_charger_mode`, and the other `evse_0_` entities). It needs the flex-horseshoe-card and Mushroom cards from HACS. If you change `name`, change the `evse_0_` prefix to match.

LCD

<img src="images/lcd.jpg" alt="EVSE LCD" style="width:45%; height:auto;">

Home Assistant dashboard

<img src="images/ha-dashboard.png" alt="Home Assistant EVSE dashboard" style="width:30%; height:auto;">

[Post in the ESPHome Discord "Show Off" Channel](https://discord.com/channels/429907082951524364/1308643575449518160) 


[Inspired by this great ESP32 (not ESPHome) project!](https://github.com/dzurikmiroslav/esp32-evse) 

[Miroslav Dzúrik explains the CP protocol and hardware to generate it in depth on his Wiki here.](https://github.com/dzurikmiroslav/esp32-evse/wiki/CP-calibration)

<img src="https://github.com/user-attachments/assets/9bf606d9-bdd5-4a05-84b6-b90d1b3f0aeb" alt="Alt Text" style="width:40%; height:auto;">

Charging real car at 40A - current setpoint can be changed in real time and vehicle will adjust its draw.

<img src="https://github.com/user-attachments/assets/bde0680d-757e-4c0f-a392-faaf9ced7648" alt="Alt Text" style="width:30%; height:auto;">
<img src="https://github.com/user-attachments/assets/6664beeb-25bf-4822-97a6-7bf8c94cb628" alt="Alt Text" style="width:30%; height:auto;">

PCB Designed by me - produced by JLCPCB

<img src="https://github.com/user-attachments/assets/edd2551b-6f4c-4058-9826-bdea2e6f5002" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/63f8fcb8-f410-4154-9450-279c3e58c243" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/bf221ddc-6771-4154-95e3-2095ccb661a0" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/48726e65-868e-46b8-bb4f-d90d5c7a817b" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/52ee4612-118a-437d-b911-eaf96cbfea1c" alt="Alt Text" style="width:40%; height:auto;">



<img src="https://github.com/user-attachments/assets/aeedd815-5398-4cb1-81bf-dedf85389a31" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/885721e3-cc22-4db5-8445-3736fb2d41a7" alt="Alt Text" style="width:40%; height:auto;">

