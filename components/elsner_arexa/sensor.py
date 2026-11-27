from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_ILLUMINANCE,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_WIND_SPEED,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_LUX,
    UNIT_METER_PER_SECOND,
)

from . import types_schema, types_to_code

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
_offset = sensor.sensor_schema(
    unit_of_measurement="kHz",
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:sine-wave",
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)
_temperature = sensor.sensor_schema(
    unit_of_measurement=UNIT_CELSIUS,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
)
TYPES = {
    "inside_temperature": _temperature,
    "outside_temperature": _temperature,
    "wind": sensor.sensor_schema(
        unit_of_measurement=UNIT_METER_PER_SECOND,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_WIND_SPEED,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "brightness": sensor.sensor_schema(
        unit_of_measurement=UNIT_LUX,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_ILLUMINANCE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # the station's byte 29 (not decoded, the same in every packet so far); the rain binary sensor is the rain alarm
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
    # the CC1101's estimate of how far each sender is from the set frequency, to tune `frequency:`
    "rc_freq_offset": _offset,
    "station_freq_offset": _offset,
}

CONFIG_SCHEMA = types_schema(TYPES)


async def to_code(config):
    await types_to_code(config, TYPES, sensor.new_sensor, "sensor")
