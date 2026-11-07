import esphome.codegen as cg
from esphome.components import cc1101
import esphome.config_validation as cv
from esphome.const import CONF_FREQUENCY, CONF_ID, CONF_OUTPUT_POWER

CODEOWNERS = ["@dbuezas"]
DEPENDENCIES = ["cc1101"]
MULTI_CONF = True

CONF_CC1101_ID = "cc1101_id"
CONF_ELSNER_AREXA_ID = "elsner_arexa_id"
CONF_PAIRING = "pairing"
CONF_SETTINGS = "settings"
CONF_OPEN_ABOVE = "open_above"
CONF_OUTSIDE_LOCK = "outside_lock"
CONF_WIND_ALARM = "wind_alarm"
CONF_RAIN_ALARM = "rain_alarm"
CONF_TRAVEL_TIME = "travel_time"

elsner_arexa_ns = cg.esphome_ns.namespace("elsner_arexa")
ElsnerArexa = elsner_arexa_ns.class_("ElsnerArexa", cg.Component)


def validate_pairing(value):
    """auto, or the 20 hex digits the pairing text sensor shows (bytes 8..16 of the RC's packets and the counter's
    check byte; 18 digits without the check byte also work, but then the first command waits for a packet)"""
    value = cv.string(value).strip().lower()
    if value == "auto":
        return value
    if len(value) not in (18, 20) or any(ch not in "0123456789abcdef" for ch in value):
        raise cv.Invalid("pairing must be 'auto' or 20 hex digits, like the pairing text sensor shows")
    # every packet has the system ID twice: bytes 8, 9 and 13, 14
    if value[0:4] != value[10:14]:
        raise cv.Invalid(
            f"pairing: the system ID (digits 1..4, {value[0:4]}) must come again in digits 11..14 "
            f"({value[10:14]}): a typo?"
        )
    return value


def off_or(validator):
    """off (a function switched off on the RC), or a value"""

    def validate(value):
        if value is False or (isinstance(value, str) and value.strip().lower() == "off"):
            return None
        return validator(value)

    return validate


# The settings made on the RC (its manual: "Einstellung der Automatik"), the station acts on them. Only for the window
# text sensor: the component can't read or change them. The defaults are the factory settings. To see them on the RC:
# hold SET for 3 s while it shows the temperatures or the weather values.
SETTINGS_SCHEMA = cv.Schema(
    {
        # A. the automatic mode opens above this inside temperature (the RC's) and closes 2 °C below it
        cv.Optional(CONF_OPEN_ABOVE, default=25.0): off_or(cv.All(cv.temperature, cv.float_range(min=5))),
        # B. below this outside temperature the automatic mode closes and keeps the window closed, until 2 °C above it
        cv.Optional(CONF_OUTSIDE_LOCK, default=5.0): off_or(cv.All(cv.temperature, cv.float_range(min=-20))),
        # C. above this wind speed (m/s) the window closes, also in manual mode, and the buttons are locked; held 5 min
        cv.Optional(CONF_WIND_ALARM, default=4.0): off_or(
            cv.All(cv.float_with_unit("wind speed", "(m/s)", optional_unit=True), cv.float_range(min=0, min_included=False))
        ),
        # D. the station's rain alarm closes the window, also in manual mode, and locks the buttons
        cv.Optional(CONF_RAIN_ALARM, default=True): cv.boolean,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ElsnerArexa),
        cv.Required(CONF_CC1101_ID): cv.use_id(cc1101.CC1101Component),
        # The Arexa sends at about 868.19 MHz. Cheap CC1101 modules can be off by +-40 kHz: set this to
        # 868.19 MHz + your module's error (mine needs 868.227 MHz).
        cv.Optional(CONF_FREQUENCY, default="868.19MHz"): cv.All(
            cv.frequency, cv.float_range(min=867.9e6, max=868.5e6)
        ),
        cv.Optional(CONF_OUTPUT_POWER, default=11): cv.float_range(min=-30.0, max=11.0),
        # auto: learn it from the RC's packets. Or fix it (the value of the pairing text sensor), then the buttons
        # work as soon as any Arexa packet brought the counter.
        cv.Optional(CONF_PAIRING, default="auto"): validate_pairing,
        cv.Optional(CONF_SETTINGS, default={}): SETTINGS_SCHEMA,
        # the window's time from open to closed: a stop within it after a close leaves the window partly open
        cv.Optional(CONF_TRAVEL_TIME, default="40s"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    radio = await cg.get_variable(config[CONF_CC1101_ID])
    cg.add(var.set_radio(radio))
    cg.add(var.set_frequency(config[CONF_FREQUENCY]))
    cg.add(var.set_output_power(config[CONF_OUTPUT_POWER]))
    settings = config[CONF_SETTINGS]
    for key in (CONF_OPEN_ABOVE, CONF_OUTSIDE_LOCK, CONF_WIND_ALARM):
        if settings[key] is not None:
            cg.add(getattr(var, f"set_{key}")(settings[key]))
    cg.add(var.set_rain_alarm(settings[CONF_RAIN_ALARM]))
    cg.add(var.set_travel_time(config[CONF_TRAVEL_TIME]))
    if config[CONF_PAIRING] != "auto":
        pairing = [int(config[CONF_PAIRING][i : i + 2], 16) for i in range(0, len(config[CONF_PAIRING]), 2)]
        cg.add(var.set_fixed_pairing(pairing))
