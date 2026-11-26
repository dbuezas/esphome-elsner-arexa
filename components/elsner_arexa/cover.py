import esphome.codegen as cg
from esphome.components import cover
import esphome.config_validation as cv

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa, elsner_arexa_ns

DEPENDENCIES = ["elsner_arexa"]

ArexaCover = elsner_arexa_ns.class_("ArexaCover", cover.Cover)

CONF_OPEN_DURATION = "open_duration"
CONF_CLOSE_DURATION = "close_duration"
CONF_START_DELAY = "start_delay"

# The window as a cover, its position worked out from the timing (README: Window position). The defaults are my
# window's (2026-11-26): it opens in 26 s and closes in 19 s once the motor runs, which starts 1 s after a command.
CONFIG_SCHEMA = cover.cover_schema(ArexaCover, device_class="window", icon="mdi:window-open-variant").extend(
    {
        cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa),
        cv.Optional(CONF_OPEN_DURATION, default="26s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_CLOSE_DURATION, default="19s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_START_DELAY, default="1s"): cv.positive_time_period_milliseconds,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    c = await cover.new_cover(config)
    await cg.register_parented(c, hub)
    cg.add(hub.set_cover(c))
    cg.add(hub.set_open_time(config[CONF_OPEN_DURATION]))
    cg.add(hub.set_close_time(config[CONF_CLOSE_DURATION]))
    cg.add(hub.set_start_delay(config[CONF_START_DELAY]))
