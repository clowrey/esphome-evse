import esphome.config_validation as cv

CODEOWNERS = ["@clowrey"]

# Header-only pilot controller. ESPHome copies evse_cp_controller.h into the
# build and includes it from esphome.h, so nothing else is needed here. The
# package constructs the controller from YAML and drives update() on an interval.
CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
