import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_ILLUMINANCE,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_LUX,
)

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa

DEPENDENCIES = ["elsner_arexa"]

_raw = sensor.sensor_schema(
    accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
)
_rssi = sensor.sensor_schema(
    unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)
TYPES = {
    "inside_temperature": sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "outside_temperature": sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # raw value, 0 = calm (the scale to m/s is not known yet)
    "wind": sensor.sensor_schema(accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, icon="mdi:weather-windy"),
    "brightness": sensor.sensor_schema(
        unit_of_measurement=UNIT_LUX,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_ILLUMINANCE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # the rain sensor's raw reading (lower = wetter); the rain binary sensor is the station's rain alarm
    "rain_sensor": sensor.sensor_schema(
        accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, icon="mdi:water", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    # the raw bytes behind the formulas, to calibrate against the RC display
    "raw_inside": _raw,
    "raw_outside": _raw,
    "raw_brightness": _raw,
    # the signal strength of the last packet from the RC and from the station, to place the radio well
    "rc_rssi": _rssi,
    "station_rssi": _rssi,
}

CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa), **{cv.Optional(k): v for k, v in TYPES.items()}}
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    for key in TYPES:
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(hub, f"set_{key}_sensor")(s))
