import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    binary_sensor,
    climate,
    remote_transmitter,
    sensor,
    switch,
    text_sensor,
)
from esphome.components import time as time_
from esphome.const import CONF_ID

AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "switch"]

actron_b812_ns = cg.esphome_ns.namespace("actron_b812")
ActronB812Climate = actron_b812_ns.class_(
    "ActronB812Climate", climate.Climate, cg.PollingComponent
)
ActronB812ZoneSwitch = actron_b812_ns.class_("ActronB812ZoneSwitch", switch.Switch)

CONF_TRANSMITTER_ID = "transmitter_id"
CONF_COMPRESSOR_COOLDOWN = "compressor_cooldown"
CONF_VALVE_SETTLE_TIME = "valve_settle_time"
CONF_TEMPERATURE_SENSOR = "temperature_sensor_id"
CONF_HYSTERESIS = "hysteresis"
CONF_AUTO_DEADBAND = "auto_deadband"
CONF_AUTO_DEADBAND_TIMEOUT = "auto_deadband_timeout"
CONF_TIME_ID = "time_id"
CONF_COMPRESSOR_RUNNING = "compressor_running"
CONF_STATE_SENSOR = "state"
CONF_PROTECTION_EXPIRES_AT = "protection_expires_at"
CONF_THERMOSTAT_DIRECTION = "thermostat_direction"
CONF_DEADBAND_ACTIVE = "deadband_active"
CONF_DEADBAND_EXPIRES_AT = "deadband_expires_at"
CONF_REVERSING_VALVE = "reversing_valve"
CONF_CALL_ACTIVE = "call_active"
CONF_ZONE_1 = "zone_1"
CONF_ZONE_2 = "zone_2"

CONFIG_SCHEMA = climate.climate_schema(ActronB812Climate).extend({
    cv.Required(CONF_TRANSMITTER_ID): cv.use_id(
        remote_transmitter.RemoteTransmitterComponent
    ),
    cv.Optional(CONF_COMPRESSOR_COOLDOWN, default="3min"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_VALVE_SETTLE_TIME, default="30s"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
    cv.Optional(CONF_HYSTERESIS, default=0.5): cv.positive_float,
    cv.Optional(CONF_AUTO_DEADBAND, default=1.0): cv.positive_float,
    cv.Optional(CONF_AUTO_DEADBAND_TIMEOUT, default="20min"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
    cv.Optional(CONF_COMPRESSOR_RUNNING): binary_sensor.binary_sensor_schema(),
    cv.Optional(CONF_STATE_SENSOR): text_sensor.text_sensor_schema(),
    cv.Optional(CONF_PROTECTION_EXPIRES_AT): text_sensor.text_sensor_schema(
        device_class="timestamp",
    ),
    cv.Optional(CONF_THERMOSTAT_DIRECTION): text_sensor.text_sensor_schema(),
    cv.Optional(CONF_DEADBAND_ACTIVE): binary_sensor.binary_sensor_schema(),
    cv.Optional(CONF_DEADBAND_EXPIRES_AT): text_sensor.text_sensor_schema(
        device_class="timestamp",
    ),
    cv.Optional(CONF_REVERSING_VALVE): binary_sensor.binary_sensor_schema(),
    cv.Optional(CONF_CALL_ACTIVE): binary_sensor.binary_sensor_schema(),
    cv.Optional(CONF_ZONE_1): switch.switch_schema(
        ActronB812ZoneSwitch,
        default_restore_mode="RESTORE_DEFAULT_ON",
    ),
    cv.Optional(CONF_ZONE_2): switch.switch_schema(
        ActronB812ZoneSwitch,
        default_restore_mode="RESTORE_DEFAULT_ON",
    ),
}).extend(cv.polling_component_schema("222ms"))

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    tx = await cg.get_variable(config[CONF_TRANSMITTER_ID])
    cg.add(var.set_transmitter(tx))
    cg.add(var.set_compressor_cooldown(config[CONF_COMPRESSOR_COOLDOWN]))
    cg.add(var.set_valve_settle_time(config[CONF_VALVE_SETTLE_TIME]))

    if CONF_TEMPERATURE_SENSOR in config:
        s = await cg.get_variable(config[CONF_TEMPERATURE_SENSOR])
        cg.add(var.set_temperature_sensor(s))
    cg.add(var.set_hysteresis(config[CONF_HYSTERESIS]))
    cg.add(var.set_auto_deadband(config[CONF_AUTO_DEADBAND]))
    cg.add(var.set_auto_deadband_timeout(config[CONF_AUTO_DEADBAND_TIMEOUT]))

    if CONF_TIME_ID in config:
        t = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(t))

    if CONF_COMPRESSOR_RUNNING in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_COMPRESSOR_RUNNING])
        cg.add(var.set_compressor_running_sensor(bs))

    if CONF_STATE_SENSOR in config:
        ts = await text_sensor.new_text_sensor(config[CONF_STATE_SENSOR])
        cg.add(var.set_state_sensor(ts))

    if CONF_PROTECTION_EXPIRES_AT in config:
        ts = await text_sensor.new_text_sensor(config[CONF_PROTECTION_EXPIRES_AT])
        cg.add(var.set_protection_expires_at_sensor(ts))

    if CONF_THERMOSTAT_DIRECTION in config:
        ts = await text_sensor.new_text_sensor(config[CONF_THERMOSTAT_DIRECTION])
        cg.add(var.set_thermostat_direction_sensor(ts))

    if CONF_DEADBAND_ACTIVE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_DEADBAND_ACTIVE])
        cg.add(var.set_deadband_active_sensor(bs))

    if CONF_DEADBAND_EXPIRES_AT in config:
        ts = await text_sensor.new_text_sensor(config[CONF_DEADBAND_EXPIRES_AT])
        cg.add(var.set_deadband_expires_at_sensor(ts))

    if CONF_REVERSING_VALVE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_REVERSING_VALVE])
        cg.add(var.set_reversing_valve_sensor(bs))

    if CONF_CALL_ACTIVE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_CALL_ACTIVE])
        cg.add(var.set_call_active_sensor(bs))

    if CONF_ZONE_1 in config:
        sw = await switch.new_switch(config[CONF_ZONE_1], var, 1)
        cg.add(var.set_zone_1_switch(sw))

    if CONF_ZONE_2 in config:
        sw = await switch.new_switch(config[CONF_ZONE_2], var, 2)
        cg.add(var.set_zone_2_switch(sw))
