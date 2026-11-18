"""bss_probe: test firmware component that records the AFE output and its raw_data
channels into PSRAM and plays each one back on the speaker (see tests/bss_probe)."""

import esphome.codegen as cg
from esphome.components import microphone, speaker
from esphome.components.esp_afe import CONF_ESP_AFE_ID, EspAfe
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER

DEPENDENCIES = ["esp32", "esp_afe", "psram"]
CODEOWNERS = ["@rgitha"]

CONF_RECORD_SECONDS = "record_seconds"
CONF_PLAYBACK_VOLUME = "playback_volume"
CONF_PLAYBACK_PEAK_DB = "playback_peak_db"
CONF_MAX_PLAYBACK_GAIN_DB = "max_playback_gain_db"
CONF_TEST_SOUND_LEVEL_DB = "test_sound_level_db"
CONF_ECHO_PLAYBACK_GAIN_DB = "echo_playback_gain_db"

bss_probe_ns = cg.esphome_ns.namespace("bss_probe")
BssProbe = bss_probe_ns.class_("BssProbe", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BssProbe),
        cv.Required(CONF_ESP_AFE_ID): cv.use_id(EspAfe),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_RECORD_SECONDS, default=10.0): cv.float_range(min=1.0, max=20.0),
        cv.Optional(CONF_PLAYBACK_VOLUME, default=0.8): cv.percentage,
        # Each stream is scaled so its peak plays at this level, at most by
        # max_playback_gain_db (the recordings are quiet; levels are in the log).
        cv.Optional(CONF_PLAYBACK_PEAK_DB, default=-6.0): cv.float_range(min=-30.0, max=0.0),
        cv.Optional(CONF_MAX_PLAYBACK_GAIN_DB, default=40.0): cv.float_range(min=0.0, max=60.0),
        cv.Optional(CONF_TEST_SOUND_LEVEL_DB, default=-12.0): cv.float_range(min=-40.0, max=0.0),
        # Recordings with the test sound play back at this one fixed gain, so the
        # echo cancellation on / off recordings can be compared by ear.
        cv.Optional(CONF_ECHO_PLAYBACK_GAIN_DB, default=20.0): cv.float_range(min=0.0, max=60.0),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_afe(await cg.get_variable(config[CONF_ESP_AFE_ID])))
    cg.add(var.set_microphone(await cg.get_variable(config[CONF_MICROPHONE])))
    cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))
    cg.add(var.set_record_seconds(config[CONF_RECORD_SECONDS]))
    cg.add(var.set_playback_volume(config[CONF_PLAYBACK_VOLUME]))
    cg.add(var.set_playback_peak_db(config[CONF_PLAYBACK_PEAK_DB]))
    cg.add(var.set_max_playback_gain_db(config[CONF_MAX_PLAYBACK_GAIN_DB]))
    cg.add(var.set_test_sound_level_db(config[CONF_TEST_SOUND_LEVEL_DB]))
    cg.add(var.set_echo_playback_gain_db(config[CONF_ECHO_PLAYBACK_GAIN_DB]))
