from esphome.components import binary_sensor
from esphome.const import DEVICE_CLASS_MOISTURE, DEVICE_CLASS_PROBLEM

from . import types_schema, types_to_code

DEPENDENCIES = ["elsner_arexa"]

TYPES = {
    # the station's rain alarm (it holds ~25 minutes after the sensor is dry)
    "rain": binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_MOISTURE),
    "wind_alarm": binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM, icon="mdi:weather-windy"),
    # on in automatic mode (the station tells the RC in its minute answer; the RC shows it from then on)
    "auto_mode": binary_sensor.binary_sensor_schema(icon="mdi:refresh-auto"),
}

CONFIG_SCHEMA = types_schema(TYPES)


async def to_code(config):
    await types_to_code(config, TYPES, binary_sensor.new_binary_sensor, "binary_sensor")
