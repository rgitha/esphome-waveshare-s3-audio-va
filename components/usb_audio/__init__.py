"""USB audio (test): the board as a USB Audio Class device, both directions.

PC -> board: 48 kHz 16-bit stereo, mixed to mono and played into a speaker (a mixer
input). Board -> PC: a 16 kHz microphone (the AFE output), converted to 48 kHz mono.
Built on Espressif's usb_device_uac (TinyUSB). The USB port then carries audio, not the
USB serial console.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import microphone, speaker
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["ring_buffer"]

CONF_START_DELAY = "start_delay"
CONF_MIN_FREE_INTERNAL = "min_free_internal"
CONF_PRODUCT_NAME = "product_name"
CONF_TASK_CORE = "task_core"
CONF_ON_PLAYBACK_START = "on_playback_start"
CONF_ON_PLAYBACK_END = "on_playback_end"

usb_audio_ns = cg.esphome_ns.namespace("usb_audio")
UsbAudio = usb_audio_ns.class_("UsbAudio", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UsbAudio),
        cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
            min_bits_per_sample=16, max_bits_per_sample=16, min_channels=1, max_channels=1
        ),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        # start USB this long after boot (fail safe: a problem cannot lock out OTA)
        cv.Optional(CONF_START_DELAY, default="10s"): cv.positive_time_period_milliseconds,
        # do not start USB if less internal RAM than this is free (bytes)
        cv.Optional(CONF_MIN_FREE_INTERNAL, default=16000): cv.int_range(min=0, max=200000),
        cv.Optional(CONF_PRODUCT_NAME, default="Voice Assistant"): cv.string_strict,
        cv.Optional(CONF_TASK_CORE, default=0): cv.int_range(min=-1, max=1),
        cv.Optional(CONF_ON_PLAYBACK_START): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_PLAYBACK_END): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)

FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MICROPHONE): microphone.final_validate_microphone_source_schema(
            "usb_audio", sample_rate=16000
        ),
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    mic = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic))
    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))
    cg.add(var.set_start_delay(config[CONF_START_DELAY]))
    cg.add(var.set_min_free_internal(config[CONF_MIN_FREE_INTERNAL]))
    for key, trig in ((CONF_ON_PLAYBACK_START, "get_playback_start_trigger"),
                      (CONF_ON_PLAYBACK_END, "get_playback_end_trigger")):
        if key in config:
            await automation.build_automation(getattr(var, trig)(), [], config[key])

    add_idf_component(name="espressif/usb_device_uac", ref="1.3.2")
    # UAC format: 48 kHz 16-bit; PC -> board stereo, board -> PC mono
    add_idf_sdkconfig_option("CONFIG_UAC_SAMPLE_RATE", 48000)
    add_idf_sdkconfig_option("CONFIG_UAC_SPEAKER_CHANNEL_NUM", 2)
    add_idf_sdkconfig_option("CONFIG_UAC_MIC_CHANNEL_NUM", 1)
    add_idf_sdkconfig_option("CONFIG_UAC_BIT_RESOLUTION_16", True)
    add_idf_sdkconfig_option("CONFIG_UAC_TUSB_PRODUCT", config[CONF_PRODUCT_NAME])
    # tasks away from core 1 (AFE feed)
    core = config[CONF_TASK_CORE]
    add_idf_sdkconfig_option("CONFIG_UAC_TINYUSB_TASK_CORE", core)
    add_idf_sdkconfig_option("CONFIG_UAC_SPK_TASK_CORE", core)
    add_idf_sdkconfig_option("CONFIG_UAC_MIC_TASK_CORE", core)
    # the USB port is the audio device: no console on USB Serial/JTAG
    add_idf_sdkconfig_option("CONFIG_ESP_CONSOLE_SECONDARY_NONE", True)
