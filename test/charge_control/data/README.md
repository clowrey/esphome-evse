# Replay data

`replay.exe` reads Home Assistant recorder rows, one per line: `epoch metadata_id state`
(state may be `unavailable` or `unknown`). The data files are not committed.

The controller was developed against the Umauma overnight Fast session of
2026-10-21 18:50 to 2026-10-22 10:30 HST (`umauma-2026-10-21.txt`). To export it again
from that Home Assistant (SSH alias `homeassistant`, SQLite recorder):

```
ssh homeassistant "sqlite3 -readonly -separator ' ' /config/home-assistant_v2.db \"select last_updated_ts, metadata_id, state from states where metadata_id in (11229,9125,3291,4315,1056,2913,2911,6694,8730,12136,12137,12121,12171,3301) and last_updated_ts between 1792640000 and 1792701000 order by last_updated_ts\"" > umauma-2026-10-21.txt
```

Metadata ids on that instance (`select metadata_id, entity_id from states_meta`):

| id | entity | unit |
|---|---|---|
| 11229 | sensor.ck0_power | kW, + charging |
| 9125 | sensor.ck1_power | kW, + charging |
| 3291 | sensor.evse_0_power | kW |
| 4315 | number.evse_0_current_setpoint | A |
| 6694 | sensor.inverter_battery_power_total | kW (slow path) |
| 8730 | sensor.evse_0_ha_battery_power_filtered | kW |
| 12136 / 12137 | sensor.evse_0_ha_battery_soc_read / _2_ | % |
| 1056 | sensor.sunsynk_load_power | W |
| 2913 / 2911 | sensor.solar_power_mate3 / hydro_power_mate3 | kW |

Window epochs (HST): 18:50 = 1792644600, 22:45 = 1792658700, 06:00 = 1792684800, 10:30 = 1792701000.
The old firmware's step cooldown was changed from 12 s to 40 s at 22:44:22 = 1792658662.
