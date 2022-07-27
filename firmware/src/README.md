# Eddy WD1 firmware
## Overview
This firmware allows you to easly control a water diverter valve using ESP8266 or ESP32 boards.

### Configure
`mos wifi <ssid> <pwd>`

`mos config-set mqtt.server=<mqtt_server_address>`
`mos config-set mqtt.user=<username>`
`mos config-set mqtt.pass=<password>`

`mos config-set dash.token=<token>`
`mos config-set dash.enable=true`

```json
{
    "valves": {
        "pure_water": 0
    },
    "flow_sensors": {
        "pure_water": {
            "flowRate": 0,
            "partialFlow": 0,
            "totalFlow": 0
        },
        "tap_water": {
            "flowRate": 0,
            "partialFlow": 0,
            "totalFlow": 0
        }
    },
    "alerts": {
        "pure_water": null,
        "tap_water": null
    },
    "force-btn": {
        "event": 1,
        "pressCount": 0,
        "pressDuration": 0
    }
}
```

## Configuration
The library adds the `eddy` section to the device configuration:
```javascript

```
## To Do
*No improvements scheduled.*