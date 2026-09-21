from pathlib import Path

import esphome.config_validation as cv

CODEOWNERS = ["@clowrey"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    # Header-only pilot controller. The package constructs it from YAML and
    # drives update() on an interval, same as the previous direct include.
    from esphome.core.config import include_file

    header = Path(__file__).resolve().parent / "evse_cp_controller.h"
    include_file(header, Path(header.name))
