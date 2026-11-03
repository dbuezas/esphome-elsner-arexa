import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa

DEPENDENCIES = ["elsner_arexa"]

CONF_PAIRING = "pairing"
CONF_STATION_RAW = "station_raw"
CONF_LAST_COMMAND = "last_command"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa),
        # the pairing of the RC heard on the air (18 hex digits); usable as `pairing:` in the hub
        cv.Optional(CONF_PAIRING): text_sensor.text_sensor_schema(
            icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        # the station's value bytes 17..35 in hex, ".." for a byte that did not arrive clean (to work out new values)
        cv.Optional(CONF_STATION_RAW): text_sensor.text_sensor_schema(
            icon="mdi:hexadecimal", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        # the result of the last command sent: "open: confirmed", "open: no answer after 4 sends",
        # "auto: mode unchanged after 4 sends", "open: not sent, RC not heard yet", "open: replaced by stop"
        cv.Optional(CONF_LAST_COMMAND): text_sensor.text_sensor_schema(icon="mdi:check-network"),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    if CONF_PAIRING in config:
        t = await text_sensor.new_text_sensor(config[CONF_PAIRING])
        cg.add(hub.set_pairing_text_sensor(t))
    if CONF_STATION_RAW in config:
        t = await text_sensor.new_text_sensor(config[CONF_STATION_RAW])
        cg.add(hub.set_station_raw_text_sensor(t))
    if CONF_LAST_COMMAND in config:
        t = await text_sensor.new_text_sensor(config[CONF_LAST_COMMAND])
        cg.add(hub.set_last_command_text_sensor(t))
