import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa, elsner_arexa_ns

DEPENDENCIES = ["elsner_arexa"]

ArexaButton = elsner_arexa_ns.class_("ArexaButton", button.Button)

# the RC buttons, in the order of the Command enum (elsner_arexa.h)
TYPES = {
    "open": "mdi:window-open-variant",
    "stop": "mdi:stop",
    "close": "mdi:window-closed-variant",
    "auto": "mdi:refresh-auto",
    "manual": "mdi:hand-back-right",
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa),
        **{cv.Optional(k): button.button_schema(ArexaButton, icon=icon) for k, icon in TYPES.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    for cmd, key in enumerate(TYPES):
        if key in config:
            b = await button.new_button(config[key])
            await cg.register_parented(b, hub)
            cg.add(b.set_command(cmd))
