import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, sensor, switch, text_sensor
from esphome.const import CONF_ID

CODEOWNERS = ["@clowrey"]
AUTO_LOAD = ["number", "sensor", "switch", "text_sensor"]

CONF_BATTERY_POWER = "battery_power"
CONF_BATTERY_POWER_FALLBACK = "battery_power_fallback"
CONF_POWER_DIVISOR = "power_divisor"
CONF_BATTERY_VOLTAGE = "battery_voltage"
CONF_BATTERY_SOC = "battery_soc"
CONF_LINE_VOLTAGE = "line_voltage"
CONF_NOMINAL_LINE_VOLTAGE = "nominal_line_voltage"
CONF_CP_STATE = "cp_state"
CONF_CHARGER_MODE = "charger_mode"
CONF_CURRENT_SETPOINT = "current_setpoint"
CONF_MAX_CURRENT_LIMIT = "max_current_limit"
CONF_ENABLE_SWITCH = "enable_switch"
CONF_PARAMS = "params"
CONF_OUTPUTS = "outputs"
CONF_SENSOR = "sensor"
CONF_ENABLED = "enabled"

ns = cg.esphome_ns.namespace("evse_charge_control")
EvseChargeControl = ns.class_("EvseChargeControl", cg.PollingComponent)
ParamId = ns.enum("ParamId", is_class=True)

# YAML key -> ParamId enumerator
PARAMS = {
    "normal_target": "NORMAL_TARGET",
    "fast_target": "FAST_TARGET",
    "min_soc": "MIN_SOC",
    "low_voltage_cutoff": "LV_CUTOFF",
    "deadband": "DEADBAND",
    "gain": "GAIN",
    "max_step_up": "MAX_STEP_UP",
    "max_step_down": "MAX_STEP_DOWN",
    "settle_time": "SETTLE",
    "settle_time_fallback": "SETTLE_FALLBACK",
    "evidence_samples": "EVIDENCE",
    "filter_alpha": "FILTER_ALPHA",
    "pause_after": "PAUSE_AFTER",
    "resume_after": "RESUME_AFTER",
    "resume_margin": "RESUME_MARGIN",
    "stale_time": "STALE",
    "blind_timeout": "BLIND_TIMEOUT",
}

OUTPUT_SENSORS = {"raw_power", "filtered_power"}
OUTPUT_TEXT = {"state_text", "event", "source", "debug"}


def enabled_flag(value):
    # Substitutions arrive as strings; accept "0"/"1" as well as the usual booleans.
    if isinstance(value, str) and value.strip() in ("0", "1"):
        return value.strip() == "1"
    if isinstance(value, int) and value in (0, 1):
        return value == 1
    return cv.boolean(value)


INPUT_SCHEMA = cv.Any(
    cv.use_id(sensor.Sensor),
    cv.Schema(
        {
            cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_ENABLED, default=True): enabled_flag,
        }
    ),
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EvseChargeControl),
        cv.Required(CONF_BATTERY_POWER): cv.ensure_list(INPUT_SCHEMA),
        cv.Optional(CONF_BATTERY_POWER_FALLBACK): INPUT_SCHEMA,
        cv.Optional(CONF_POWER_DIVISOR, default=1.0): cv.positive_float,
        cv.Optional(CONF_BATTERY_VOLTAGE): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_BATTERY_SOC): cv.ensure_list(INPUT_SCHEMA),
        cv.Optional(CONF_LINE_VOLTAGE): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_NOMINAL_LINE_VOLTAGE, default=230.0): cv.positive_float,
        cv.Required(CONF_CP_STATE): cv.use_id(number.Number),
        cv.Required(CONF_CHARGER_MODE): cv.use_id(number.Number),
        cv.Required(CONF_CURRENT_SETPOINT): cv.use_id(number.Number),
        cv.Required(CONF_MAX_CURRENT_LIMIT): cv.use_id(number.Number),
        cv.Required(CONF_ENABLE_SWITCH): cv.use_id(switch.Switch),
        cv.Optional(CONF_PARAMS, default={}): cv.Schema(
            {cv.Optional(k): cv.use_id(number.Number) for k in PARAMS}
        ),
        cv.Optional(CONF_OUTPUTS, default={}): cv.Schema(
            {
                **{cv.Optional(k): cv.use_id(sensor.Sensor) for k in OUTPUT_SENSORS},
                **{cv.Optional(k): cv.use_id(text_sensor.TextSensor) for k in OUTPUT_TEXT},
            }
        ),
    }
).extend(cv.polling_component_schema("1s"))


def _split_input(item):
    if isinstance(item, dict):
        return item[CONF_SENSOR], item[CONF_ENABLED]
    return item, True


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for item in config[CONF_BATTERY_POWER]:
        sens_id, enabled = _split_input(item)
        cg.add(var.add_power_input(await cg.get_variable(sens_id), enabled))
    if CONF_BATTERY_POWER_FALLBACK in config:
        sens_id, enabled = _split_input(config[CONF_BATTERY_POWER_FALLBACK])
        cg.add(var.set_fallback_power_input(await cg.get_variable(sens_id), enabled))
    cg.add(var.set_power_divisor(config[CONF_POWER_DIVISOR]))
    if CONF_BATTERY_VOLTAGE in config:
        cg.add(var.set_battery_voltage_sensor(await cg.get_variable(config[CONF_BATTERY_VOLTAGE])))
    for item in config.get(CONF_BATTERY_SOC, []):
        sens_id, enabled = _split_input(item)
        cg.add(var.add_soc_sensor(await cg.get_variable(sens_id), enabled))
    if CONF_LINE_VOLTAGE in config:
        cg.add(var.set_line_voltage_sensor(await cg.get_variable(config[CONF_LINE_VOLTAGE])))
    cg.add(var.set_nominal_line_voltage(config[CONF_NOMINAL_LINE_VOLTAGE]))

    cg.add(var.set_cp_state_number(await cg.get_variable(config[CONF_CP_STATE])))
    cg.add(var.set_mode_number(await cg.get_variable(config[CONF_CHARGER_MODE])))
    cg.add(var.set_setpoint_number(await cg.get_variable(config[CONF_CURRENT_SETPOINT])))
    cg.add(var.set_max_current_number(await cg.get_variable(config[CONF_MAX_CURRENT_LIMIT])))
    cg.add(var.set_enable_switch(await cg.get_variable(config[CONF_ENABLE_SWITCH])))

    for key, enumerator in PARAMS.items():
        if key in config[CONF_PARAMS]:
            num = await cg.get_variable(config[CONF_PARAMS][key])
            cg.add(var.set_param_number(getattr(ParamId, enumerator), num))

    outputs = config[CONF_OUTPUTS]
    if "raw_power" in outputs:
        cg.add(var.set_raw_power_sensor(await cg.get_variable(outputs["raw_power"])))
    if "filtered_power" in outputs:
        cg.add(var.set_filtered_power_sensor(await cg.get_variable(outputs["filtered_power"])))
    if "state_text" in outputs:
        cg.add(var.set_state_text_sensor(await cg.get_variable(outputs["state_text"])))
    if "event" in outputs:
        cg.add(var.set_event_text_sensor(await cg.get_variable(outputs["event"])))
    if "source" in outputs:
        cg.add(var.set_source_text_sensor(await cg.get_variable(outputs["source"])))
    if "debug" in outputs:
        cg.add(var.set_debug_text_sensor(await cg.get_variable(outputs["debug"])))
