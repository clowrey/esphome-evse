import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, output, sensor, switch
from esphome.const import CONF_ID

CODEOWNERS = ["@clowrey"]
AUTO_LOAD = ["number", "output", "sensor", "switch"]

CONF_CP_PWM_OUTPUT = "cp_pwm_output"
CONF_CP_PWM_SENSOR = "cp_pwm_sensor"
CONF_CP_STATE_NUMBER = "cp_state_number"
CONF_CURRENT_SETPOINT = "current_setpoint"
CONF_ENABLE_SWITCH = "enable_switch"
CONF_DEBUG_ERROR_SWITCH = "debug_error_switch"
CONF_RELAY_SWITCH = "relay_switch"

evse_cp_ns = cg.esphome_ns.namespace("evse_cp")
EvseCpController = evse_cp_ns.class_("EvseCpController", cg.PollingComponent)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EvseCpController),
        cv.Required(CONF_CP_PWM_OUTPUT): cv.use_id(output.FloatOutput),
        cv.Required(CONF_CP_PWM_SENSOR): cv.use_id(sensor.Sensor),
        cv.Required(CONF_CP_STATE_NUMBER): cv.use_id(number.Number),
        cv.Required(CONF_CURRENT_SETPOINT): cv.use_id(number.Number),
        cv.Required(CONF_ENABLE_SWITCH): cv.use_id(switch.Switch),
        cv.Required(CONF_DEBUG_ERROR_SWITCH): cv.use_id(switch.Switch),
        cv.Required(CONF_RELAY_SWITCH): cv.use_id(switch.Switch),
    }
).extend(cv.polling_component_schema("100ms"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_cp_pwm_output(await cg.get_variable(config[CONF_CP_PWM_OUTPUT])))
    cg.add(var.set_cp_pwm_sensor(await cg.get_variable(config[CONF_CP_PWM_SENSOR])))
    cg.add(var.set_cp_state_number(await cg.get_variable(config[CONF_CP_STATE_NUMBER])))
    cg.add(var.set_current_setpoint_number(await cg.get_variable(config[CONF_CURRENT_SETPOINT])))
    cg.add(var.set_enable_switch(await cg.get_variable(config[CONF_ENABLE_SWITCH])))
    cg.add(var.set_debug_error_switch(await cg.get_variable(config[CONF_DEBUG_ERROR_SWITCH])))
    cg.add(var.set_relay_switch(await cg.get_variable(config[CONF_RELAY_SWITCH])))
