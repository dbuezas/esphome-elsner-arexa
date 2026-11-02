import esphome.codegen as cg
from esphome.components import cc1101
import esphome.config_validation as cv
from esphome.const import CONF_FREQUENCY, CONF_ID, CONF_OUTPUT_POWER

CODEOWNERS = ["@dbuezas"]
DEPENDENCIES = ["cc1101"]
MULTI_CONF = True

CONF_CC1101_ID = "cc1101_id"
CONF_ELSNER_AREXA_ID = "elsner_arexa_id"
CONF_SYSTEM = "system"

elsner_arexa_ns = cg.esphome_ns.namespace("elsner_arexa")
ElsnerArexa = elsner_arexa_ns.class_("ElsnerArexa", cg.Component)


def validate_system(value):
    """auto, or the 18 hex digits the system text sensor shows (bytes 8..16 of the RC's packets)"""
    value = cv.string(value).strip().lower()
    if value == "auto":
        return value
    if len(value) != 18 or any(ch not in "0123456789abcdef" for ch in value):
        raise cv.Invalid("system must be 'auto' or 18 hex digits, like the system text sensor shows")
    return value


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
        # auto: learn it from the RC's packets. Or fix it (the value of the system text sensor), then the buttons
        # work as soon as any Arexa packet brought the counter.
        cv.Optional(CONF_SYSTEM, default="auto"): validate_system,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    radio = await cg.get_variable(config[CONF_CC1101_ID])
    cg.add(var.set_radio(radio))
    cg.add(var.set_frequency(config[CONF_FREQUENCY]))
    cg.add(var.set_output_power(config[CONF_OUTPUT_POWER]))
    if config[CONF_SYSTEM] != "auto":
        system = [int(config[CONF_SYSTEM][i : i + 2], 16) for i in range(0, 18, 2)]
        cg.add(var.set_fixed_system(system))
