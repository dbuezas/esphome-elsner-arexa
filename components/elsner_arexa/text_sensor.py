import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa

DEPENDENCIES = ["elsner_arexa"]

CONF_SYSTEM = "system"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa),
        # the system bytes of the RC heard on the air (18 hex digits); usable as `system:` in the hub
        cv.Optional(CONF_SYSTEM): text_sensor.text_sensor_schema(
            icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    if CONF_SYSTEM in config:
        t = await text_sensor.new_text_sensor(config[CONF_SYSTEM])
        cg.add(hub.set_system_text_sensor(t))
