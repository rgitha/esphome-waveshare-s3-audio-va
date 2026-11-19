"""DOA probe (test): direction of arrival from the two raw microphones (before the
AFE) with SRP-PHAT (srp_phat.h; two mics, 0-180 degrees), logged with the
levels of the AFE's three raw_data streams (BSS raw 0, raw 1, unseparated raw 2).

Needs the esp_afe mic tap (rgitha/esphome-audio-stack, branch doa).
"""

import esphome.codegen as cg
from esphome.components import micro_wake_word, sensor
from esphome.components.esp_afe import CONF_ESP_AFE_ID, EspAfe
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["esp_afe"]
AUTO_LOAD = ["sensor", "ring_buffer"]

CONF_MIC_DISTANCE = "mic_distance"
CONF_RESOLUTION = "resolution"
CONF_FRAME_SAMPLES = "frame_samples"
CONF_LOG_INTERVAL = "log_interval"
CONF_LEVEL_THRESHOLD = "level_threshold"
CONF_SWAP_MICS = "swap_mics"
CONF_ANGLE = "angle"
CONF_TASK_CORE = "task_core"
CONF_START_DELAY = "start_delay"
CONF_CHANGE_THRESHOLD = "change_threshold"
CONF_REQUIRE_VAD = "require_vad"
CONF_VAD_SOURCES = "vad_sources"
CONF_DELAY_OFFSET = "delay_offset"
CONF_MIN_FREE_INTERNAL = "min_free_internal"

doa_probe_ns = cg.esphome_ns.namespace("doa_probe")
DoaProbe = doa_probe_ns.class_("DoaProbe", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DoaProbe),
        cv.GenerateID(CONF_ESP_AFE_ID): cv.use_id(EspAfe),
        # distance between the two microphones, metres
        # (effective) distance between the two microphones, metres: calibrated, so that
        # sound from in line with the mics reads 0 / 180 deg
        cv.Required(CONF_MIC_DISTANCE): cv.float_range(min=0.005, max=0.5),
        # fixed extra delay of the left mic relative to the right (microseconds),
        # calibration for a channel timing or phase mismatch (broadside reads 90 deg)
        cv.Optional(CONF_DELAY_OFFSET, default=0.0): cv.float_range(min=-200.0, max=200.0),
        cv.Optional(CONF_RESOLUTION, default=10.0): cv.float_range(min=1.0, max=45.0),
        cv.Optional(CONF_FRAME_SAMPLES, default=1024): cv.one_of(256, 512, 1024, 2048, int=True),
        cv.Optional(CONF_LOG_INTERVAL, default="2s"): cv.positive_time_period_milliseconds,
        # frames quieter than this (dBFS, mean of both mics) are left out of the angle
        cv.Optional(CONF_LEVEL_THRESHOLD, default=-60.0): cv.float_range(min=-120.0, max=0.0),
        # report (log line, sensor) only while the AFE VAD hears voice, and only when the
        # direction moved by at least this much (deg) since the last report
        cv.Optional(CONF_CHANGE_THRESHOLD, default=10.0): cv.float_range(min=0.0, max=180.0),
        # use only frames during which a micro_wake_word VAD (vad_sources) reported voice
        # (LEDs, log, sensor); false: every frame above level_threshold
        cv.Optional(CONF_REQUIRE_VAD, default=True): cv.boolean,
        # micro_wake_word instances whose VAD model decides "voice" (any of them)
        cv.Optional(CONF_VAD_SOURCES, default=[]): cv.ensure_list(cv.use_id(micro_wake_word.MicroWakeWord)),
        # 0 deg = right mic's side (transport mic 0, slot 0); swap if the angle runs backwards
        cv.Optional(CONF_SWAP_MICS, default=False): cv.boolean,
        cv.Optional(CONF_TASK_CORE, default=0): cv.int_range(min=0, max=1),
        # start this long after boot (after ESPHome has marked the boot good), so a
        # crash in the probe cannot lock the device into safe mode and OTA stays possible
        cv.Optional(CONF_START_DELAY, default="90s"): cv.positive_time_period_milliseconds,
        # do not start if less internal RAM than this is free (bytes)
        cv.Optional(CONF_MIN_FREE_INTERNAL, default=16000): cv.int_range(min=0, max=200000),
        cv.Optional(CONF_ANGLE): sensor.sensor_schema(
            unit_of_measurement="°",
            accuracy_decimals=0,
            icon="mdi:compass-outline",
            entity_category="diagnostic",
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    afe = await cg.get_variable(config[CONF_ESP_AFE_ID])
    cg.add(var.set_afe(afe))
    cg.add(var.set_mic_distance(config[CONF_MIC_DISTANCE]))
    cg.add(var.set_delay_offset_us(config[CONF_DELAY_OFFSET]))
    cg.add(var.set_resolution(config[CONF_RESOLUTION]))
    cg.add(var.set_frame_samples(config[CONF_FRAME_SAMPLES]))
    cg.add(var.set_log_interval(config[CONF_LOG_INTERVAL]))
    cg.add(var.set_level_threshold(config[CONF_LEVEL_THRESHOLD]))
    cg.add(var.set_change_threshold(config[CONF_CHANGE_THRESHOLD]))
    cg.add(var.set_require_vad(config[CONF_REQUIRE_VAD]))
    for src in config[CONF_VAD_SOURCES]:
        cg.add(var.add_vad_source(await cg.get_variable(src)))
    cg.add(var.set_swap_mics(config[CONF_SWAP_MICS]))
    cg.add(var.set_task_core(config[CONF_TASK_CORE]))
    cg.add(var.set_start_delay(config[CONF_START_DELAY]))
    cg.add(var.set_min_free_internal(config[CONF_MIN_FREE_INTERNAL]))
    if CONF_ANGLE in config:
        s = await sensor.new_sensor(config[CONF_ANGLE])
        cg.add(var.set_angle_sensor(s))
