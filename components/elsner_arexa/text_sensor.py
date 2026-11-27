import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa

DEPENDENCIES = ["elsner_arexa"]

TYPES = {
    # the pairing of the RC heard on the air (20 hex digits); usable as `pairing:` in the hub
    "pairing": text_sensor.text_sensor_schema(icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # the station's value bytes 17..35 in hex (to work out new values)
    "station_raw": text_sensor.text_sensor_schema(icon="mdi:hexadecimal", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # the result of the last command sent (see the README, Sending and its result)
    "last_command": text_sensor.text_sensor_schema(icon="mdi:check-network"),
    # the window: open, closed or unknown, from the commands (gateway and RC), the alarms and the automatic
    # mode with the RC's settings (the hub's `settings:`); README: Window state
    "window": text_sensor.text_sensor_schema(icon="mdi:window-open-variant"),
    # every Arexa frame as received, and my own sends, to keep them all in the history: "ok -62 cead93..."
    # (CRC matches; then the signal in dBm and the 38 bytes), "repaired ..." (one wrong bit), "bad ..." (wrong
    # CRC), "tx cead93..." (sent)
    "last_frame": text_sensor.text_sensor_schema(icon="mdi:radio-tower", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
}

CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa), **{cv.Optional(k): v for k, v in TYPES.items()}}
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    for key in TYPES:
        if key in config:
            t = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(hub, f"set_{key}_text_sensor")(t))
