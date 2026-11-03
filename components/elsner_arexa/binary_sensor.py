import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_MOISTURE, DEVICE_CLASS_PROBLEM

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa

DEPENDENCIES = ["elsner_arexa"]

TYPES = {
    # the station's rain alarm (it holds ~25 minutes after the sensor is dry)
    "rain": binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_MOISTURE),
    "wind_alarm": binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM, icon="mdi:weather-windy"),
    # on in automatic mode (the station tells the RC in its minute answer; the RC shows it from then on)
    "auto_mode": binary_sensor.binary_sensor_schema(icon="mdi:refresh-auto"),
}

CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa), **{cv.Optional(k): v for k, v in TYPES.items()}}
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    for key in TYPES:
        if key in config:
            s = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(hub, f"set_{key}_binary_sensor")(s))
