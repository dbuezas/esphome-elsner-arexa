from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import types_schema, types_to_code

DEPENDENCIES = ["elsner_arexa"]

TYPES = {
    # the pairing of the RC heard on the air (20 hex digits); usable as `pairing:` in the hub
    "pairing": text_sensor.text_sensor_schema(icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # the station's value bytes 17..35 in hex (to work out new values)
    "station_raw": text_sensor.text_sensor_schema(icon="mdi:hexadecimal", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # the result of the last command sent (see the README, Sending and its result)
    "last_command": text_sensor.text_sensor_schema(icon="mdi:check-network"),
    # the window: open or closed (nothing until it is known), from the commands (gateway and RC), the alarms and the automatic
    # mode with the RC's settings (the hub's `settings:`); README: Window state
    "window": text_sensor.text_sensor_schema(icon="mdi:window-open-variant"),
    # every Arexa frame as received, and my own sends, to keep them all in the history: "ok -62 cead93..."
    # (CRC matches; then the signal in dBm and the 38 bytes), "repaired ..." (one wrong bit), "bad ..." (wrong
    # CRC), "tx cead93..." (sent)
    "last_frame": text_sensor.text_sensor_schema(icon="mdi:radio-tower", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
}

CONFIG_SCHEMA = types_schema(TYPES)


async def to_code(config):
    await types_to_code(config, TYPES, text_sensor.new_text_sensor, "text_sensor")
