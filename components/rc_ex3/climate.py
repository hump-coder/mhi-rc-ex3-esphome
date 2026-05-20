import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart, sensor
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_HERTZ,
)

from . import rc_ex3_ns

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["climate", "sensor"]

RcEx3Climate = rc_ex3_ns.class_(
    "RcEx3Climate",
    climate.Climate,
    uart.UARTDevice,
    cg.PollingComponent,
)

CONF_INDOOR_TEMPERATURE  = "indoor_temperature"
CONF_OUTDOOR_TEMPERATURE = "outdoor_temperature"
CONF_RETURN_AIR_TEMPERATURE = "return_air_temperature"
CONF_COMPRESSOR_FREQUENCY = "compressor_frequency"
CONF_INDOOR_FAN_SPEED = "indoor_fan_speed"

CONFIG_SCHEMA = (
    climate.climate_schema(RcEx3Climate)
    .extend(
        {
            cv.Optional(CONF_INDOOR_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_OUTDOOR_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_RETURN_AIR_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_COMPRESSOR_FREQUENCY): sensor.sensor_schema(
                unit_of_measurement=UNIT_HERTZ,
                accuracy_decimals=0,
                icon="mdi:sine-wave",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_INDOOR_FAN_SPEED): sensor.sensor_schema(
                accuracy_decimals=0,
                icon="mdi:fan",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.polling_component_schema("30s"))
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if CONF_INDOOR_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_INDOOR_TEMPERATURE])
        cg.add(var.set_indoor_temperature_sensor(sens))

    if CONF_OUTDOOR_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_OUTDOOR_TEMPERATURE])
        cg.add(var.set_outdoor_temperature_sensor(sens))

    if CONF_RETURN_AIR_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_RETURN_AIR_TEMPERATURE])
        cg.add(var.set_return_air_temperature_sensor(sens))

    if CONF_COMPRESSOR_FREQUENCY in config:
        sens = await sensor.new_sensor(config[CONF_COMPRESSOR_FREQUENCY])
        cg.add(var.set_compressor_frequency_sensor(sens))

    if CONF_INDOOR_FAN_SPEED in config:
        sens = await sensor.new_sensor(config[CONF_INDOOR_FAN_SPEED])
        cg.add(var.set_indoor_fan_speed_sensor(sens))
