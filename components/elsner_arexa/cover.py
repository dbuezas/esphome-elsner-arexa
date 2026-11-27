import esphome.codegen as cg
from esphome.components import cover
import esphome.config_validation as cv

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa, elsner_arexa_ns

DEPENDENCIES = ["elsner_arexa"]

ArexaCover = elsner_arexa_ns.class_("ArexaCover", cover.Cover)

CONF_OPEN_DURATION = "open_duration"
CONF_CLOSE_DURATION = "close_duration"
CONF_START_DELAY = "start_delay"

# The window as a cover, its position worked out from the timing (README: Window position). The timing's defaults are
# in elsner_arexa.h (the window text sensor uses them without a cover too).
CONFIG_SCHEMA = cover.cover_schema(ArexaCover, device_class="window", icon="mdi:window-open-variant").extend(
    {
        cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa),
        cv.Optional(CONF_OPEN_DURATION): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_CLOSE_DURATION): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_START_DELAY): cv.positive_time_period_milliseconds,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    c = await cover.new_cover(config)
    await cg.register_parented(c, hub)
    cg.add(hub.set_cover(c))
    for key, setter in (
        (CONF_OPEN_DURATION, hub.set_open_time),
        (CONF_CLOSE_DURATION, hub.set_close_time),
        (CONF_START_DELAY, hub.set_start_delay),
    ):
        if key in config:
            cg.add(setter(config[key]))
