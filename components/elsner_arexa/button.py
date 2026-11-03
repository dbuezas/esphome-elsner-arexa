import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv

from . import CONF_ELSNER_AREXA_ID, ElsnerArexa, elsner_arexa_ns

DEPENDENCIES = ["elsner_arexa"]

ArexaButton = elsner_arexa_ns.class_("ArexaButton", button.Button)

# the RC buttons: key, (value of the Command enum in elsner_arexa.h, icon). close is another name for down.
TYPES = {
    "open": (0, "mdi:window-open-variant"),
    "stop": (1, "mdi:stop"),
    "down": (2, "mdi:window-closed-variant"),
    "close": (2, "mdi:window-closed-variant"),
    "auto": (3, "mdi:refresh-auto"),
    "manual": (4, "mdi:hand-back-right"),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ELSNER_AREXA_ID): cv.use_id(ElsnerArexa),
        **{cv.Optional(k): button.button_schema(ArexaButton, icon=icon) for k, (_, icon) in TYPES.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_ELSNER_AREXA_ID])
    for key, (cmd, _) in TYPES.items():
        if key in config:
            b = await button.new_button(config[key])
            await cg.register_parented(b, hub)
            cg.add(b.set_command(cmd))
