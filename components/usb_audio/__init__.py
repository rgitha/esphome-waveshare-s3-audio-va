"""USB audio (test): the board as a USB Audio Class 1.0 device, both directions.

PC -> board: 48 kHz 16-bit stereo, mixed to mono and played into a speaker (a mixer
input), following the PC's volume and mute. Board -> PC: a 16 kHz microphone (the AFE
output), converted to 48 kHz mono, following the PC's microphone level and mute. UAC 1.0 needs no driver on Windows 7 and later, Linux
or macOS. Built on espressif/tinyusb with our own device layer (usb_uac1/: descriptors,
class requests, the USB task). The USB port then carries audio, not the USB serial console.
"""

from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import microphone, psram, speaker
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["ring_buffer"]

CONF_START_DELAY = "start_delay"
CONF_MIN_FREE_INTERNAL = "min_free_internal"
CONF_PRODUCT_NAME = "product_name"
CONF_TASK_CORE = "task_core"
CONF_TASK_PRIORITY = "task_priority"
CONF_TASK_STACK_IN_PSRAM = "task_stack_in_psram"
CONF_VOLUME_MIN = "volume_min"
CONF_MIC_VOLUME_MIN = "mic_volume_min"
CONF_MIC_VOLUME_MAX = "mic_volume_max"
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
        cv.Optional(CONF_MIN_FREE_INTERNAL, default=12000): cv.int_range(min=0, max=200000),
        cv.Optional(CONF_PRODUCT_NAME, default="Voice Assistant"): cv.All(
            cv.string_strict, cv.Length(max=31)
        ),
        # USB task: TinyUSB events and the audio work every 1-2 ms
        cv.Optional(CONF_TASK_CORE, default=0): cv.int_range(min=-1, max=1),
        cv.Optional(CONF_TASK_PRIORITY, default=18): cv.int_range(min=1, max=24),
        cv.Optional(CONF_TASK_STACK_IN_PSRAM, default=True): cv.boolean,
        # PC volume slider range: this .. 0 dB
        cv.Optional(CONF_VOLUME_MIN, default=-40): cv.int_range(min=-60, max=-6),
        # PC microphone level slider range: mic_volume_min .. mic_volume_max dB (0 dB at start)
        cv.Optional(CONF_MIC_VOLUME_MIN, default=-20): cv.int_range(min=-60, max=-1),
        cv.Optional(CONF_MIC_VOLUME_MAX, default=12): cv.int_range(min=0, max=12),
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
    cg.add(var.set_product_name(config[CONF_PRODUCT_NAME]))
    cg.add(
        var.set_task(
            config[CONF_TASK_CORE],
            config[CONF_TASK_PRIORITY],
            config[CONF_TASK_STACK_IN_PSRAM],
        )
    )
    cg.add(var.set_volume_min_db(config[CONF_VOLUME_MIN]))
    cg.add(
        var.set_mic_volume_range_db(config[CONF_MIC_VOLUME_MIN], config[CONF_MIC_VOLUME_MAX])
    )
    for key, trig in (
        (CONF_ON_PLAYBACK_START, "get_playback_start_trigger"),
        (CONF_ON_PLAYBACK_END, "get_playback_end_trigger"),
    ):
        if key in config:
            await automation.build_automation(getattr(var, trig)(), [], config[key])

    if config[CONF_TASK_STACK_IN_PSRAM]:
        psram.request_external_task_stack()

    # TinyUSB (UAC 1.0 support since 0.20) and our device layer, which builds TinyUSB
    # with its tusb_config.h
    add_idf_component(name="espressif/tinyusb", ref="0.21.0~6")
    add_idf_component(name="usb_uac1", path=str(Path(__file__).parent / "usb_uac1"))
    # the USB port is the audio device: no console on USB Serial/JTAG
    add_idf_sdkconfig_option("CONFIG_ESP_CONSOLE_SECONDARY_NONE", True)
