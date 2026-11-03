import esphome.codegen as cg
from esphome.components import event
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_BUTTON

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa

DEPENDENCIES = ["elsner_arexa"]

CONF_REMOTE = "remote"
# in the order of the Command enum (elsner_arexa.h); a close press is the event type "down"
EVENT_TYPES = ["open", "stop", "down", "auto", "manual"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa),
        # a press on the real RC (its repeats, with the same counter, count as one press)
        cv.Optional(CONF_REMOTE): event.event_schema(device_class=DEVICE_CLASS_BUTTON, icon="mdi:remote"),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    if CONF_REMOTE in config:
        e = await event.new_event(config[CONF_REMOTE], event_types=EVENT_TYPES)
        cg.add(hub.set_remote_event(e))
