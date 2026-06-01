# esphome-actron-b812

ESPHome external component to replace the **Actron B812(RT)** wall controller on WSHP (water-source heat pump) air conditioners with a LE85R3 relay board.

> *Wall controllers covered: **B812**, **B812RT**, **B512CW**. Likely also **LE612CW**, **LE75CW**.*  
> *Relay boards covered: **LE85**, **LE85R3**, **LE85R3-1**.* 

The B812 communicates with the LE85 over a 2-wire bus using a custom pulse-distance coding protocol that has not (to my knowledge) been documented publicly before. This repo contains both the ESPHome component and [full protocol documentation](docs/protocol.md).

> **Origins**: The LE85 and these wall controllers were originally designed by **OEM Electronics** and sold under the **Leasam Controls** brand before being rebadged by Actron Controls. The original [Leasam Controls catalogue](docs/Leasam-catalogue.pdf) documents the whole interoperable family and confirmed several protocol details (see [protocol.md](docs/protocol.md)).

## Features

- **Modes**: off, cool, heat, heat/cool (auto), fan-only
- **Fan speeds**: low, medium, high, auto
- **2-zone damper control**: zone interlocks enforced (at least one zone always active)
- **Soft thermostat** with configurable hysteresis and auto-mode deadband
- **Compressor protection**: minimum off-time before restart (prevents short-cycling)
- **Reversing valve sequencing**: waits for valve to settle before restarting compressor after heat→cool direction change
- **Diagnostic sensors**: compressor state, thermostat direction, protection timers, reversing valve state, and more

## Hardware

The Actron Controls B812 is a wall-mounted controller which connects to a LE85R3 relay board found in some (seemingly obscure 😆) WSHP units. It connects to the WSHP unit via a 2-wire bus and continuously transmits the desired state at ~222 ms intervals in 8-bit data frames over the power wire, like an IR remote but over power 😆📺.

> **Compatibility note**:
>
> **Wall controllers** — The same connector pinout is used by several other Actron Controls wall controllers: **B812**, **B812RT**(?), **B512CW**, **LE612CW** and **LE75CW**. The B812 and the B512CW are described as interchangeable on the LE85R3-1 by retailer documentation, so this component is very likely to work as a B512CW replacement too. The original Leasam catalogue documents `B512CW`, `LE612CW` and `LE75CW` as one LE85-compatible wall-pad family on an identical control connector (with `LE612CW` wired identically to `B512CW`), so they are very likely protocol-compatible — though this hasn't been confirmed bit-for-bit. Note `LE75CW` has **no zone control**, so it would not drive the zone bits.
>
> **Relay boards** — Verified against the **LE85R3** specifically. The wider **LE85** / **LE85R3** family is likely compatible since these controllers are sold as direct replacements for each other, but I have only personally tested with the LE85R3.
>
> If you successfully use this component (or fail to!) with any of these variants, please open an issue or PR.


| WSHP with LE85R3 relay module                 | B812RT Wall Controller                 |
| --------------------------------------------- | -------------------------------------- |
| WSHP with Actron Controls LE85R3 relay module | Actron Controls B812RT Wall Controller |


## Wiring

### LE85R3 connector

The B812RT connects to the **LE85R3 relay board** via up to 5 wires. For this component, only **PWR** and **COM** are needed.


| Wire    | Required                                        | Description                                                                                                                                                                                                                                                                    |
| ------- | ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **PWR** | yes                                             | Nominally 7 V supply — also the signal wire                                                                                                                                                                                                                                    |
| **COM** | yes                                             | Common / ground return                                                                                                                                                                                                                                                         |
| **AUX** | yes (original B812RT only — not this component) | Status back-channel from the LE85 — interlock/fault state (remote on/off + condenser-pump interlock, and likely fault codes too). The original controller won't call for conditioning without it. This component ignores it. See [docs/protocol.md](docs/protocol.md#aux-line) |
| **SN1** | no                                              | Optional remote temperature sensor input (10K3T thermistor)                                                                                                                                                                                                                    |
| **SN2** | no                                              | Optional second remote temperature sensor input                                                                                                                                                                                                                                |


B812 connector pins

See [docs/protocol.md](docs/protocol.md#pinout) for full pin details.

### Signal circuit (MOSFET)

The ESP cannot be powered from the PWR rail — the rail collapses under the current spikes of Wi-Fi TX (see [docs/protocol.md](docs/protocol.md#power-budget) for the load test). Instead:

- **Power the ESP from an external 5 V supply**
- Use an **N-channel MOSFET** driven by a GPIO to short PWR to COM for each data pulse

```
PWR ───────────────────────── drain
                               │
GPIO ── 100 Ω ── gate  (MOSFET)
                  │
                10 kΩ
                  │
COM ──────────────┴──────── source
```

- The **10 kΩ pull-down** from gate to COM keeps the MOSFET off if the GPIO is floating (e.g. during boot)
- The **100 Ω series resistor** on the gate limits inrush into the gate capacitance — the circuit works without it but it is good practice

### ESPHome transmitter

Declare a `remote_transmitter` with no carrier (DC output):

```yaml
remote_transmitter:
  id: ac_tx
  pin: GPIO5        # adjust to your wiring
  carrier_duty_percent: 100%
```

### Signal details

The wall controller transmits by shorting PWR to COM for each 150 µs active-low pulse of an 8-bit pulse-distance-encoded frame, repeated every ~222 ms.

Single data frame

## Installation

Add to your ESPHome YAML:

```yaml
external_components:
  - source: github://ddoodm/esphome-actron-b812@main
    components: [actron_b812]
```

## Minimal example

```yaml
remote_transmitter:
  id: ac_tx
  pin: GPIO5
  carrier_duty_percent: 100%

climate:
  - platform: actron_b812
    name: "Aircon"
    transmitter_id: ac_tx
```

## Full configuration reference

```yaml
climate:
  - platform: actron_b812
    name: "Aircon"

    # Required
    transmitter_id: ac_tx           # ID of your remote_transmitter

    # Thermostat (optional)
    temperature_sensor_id: my_temp  # Sensor to use for current temperature
    hysteresis: 0.5                 # °C - deadband before engaging in cool/heat mode (default 0.5)
    auto_deadband: 1.5              # °C - gap between heat and cool thresholds in heat/cool mode (default 1.5)
    auto_deadband_timeout: 20min    # How long idle before deadband protection expires (default 20min, 0 to disable)

    # Compressor protection (optional)
    compressor_cooldown: 5min       # Minimum off-time before compressor can restart (default 5min)
    valve_settle_time: 30s          # Time to wait after reversing valve switches before restarting (default 30s)

    # Time source for diagnostic timestamps (optional)
    time_id: ha_time

    # Zone damper switches (optional - both zones on if omitted)
    zone_1:
      name: "Aircon Zone 1"
    zone_2:
      name: "Aircon Zone 2"

    # Diagnostic sensors (all optional)
    compressor_running:
      name: "Aircon Compressor Running"
    state:
      name: "Aircon State"
    thermostat_direction:
      name: "Aircon Thermostat Direction"
    reversing_valve:
      name: "Aircon Reversing Valve"
    call_active:
      name: "Aircon Call Active"
    protection_expires_at:
      name: "Aircon Protection Expires At"
    deadband_active:
      name: "Aircon Deadband Active"
    deadband_expires_at:
      name: "Aircon Deadband Expires At"
```

### Diagnostic sensor values


| Sensor                  | Type          | Values                                                                                                     |
| ----------------------- | ------------- | ---------------------------------------------------------------------------------------------------------- |
| `compressor_running`    | binary sensor | `true` / `false`                                                                                           |
| `state`                 | text sensor   | `off`, `cooling`, `heating`, `fan_only`, `heat_idle`, `comp_cooldown`, `heat_valve_hold`, `valve_settling` |
| `thermostat_direction`  | text sensor   | `idle`, `cool`, `heat`                                                                                     |
| `reversing_valve`       | binary sensor | `true` = heat position                                                                                     |
| `call_active`           | binary sensor | `true` when calling for conditioning                                                                       |
| `protection_expires_at` | text sensor   | ISO 8601 timestamp, or empty when no timer active                                                          |
| `deadband_active`       | binary sensor | `true` when cross-mode deadband is suppressing engagement                                                  |
| `deadband_expires_at`   | text sensor   | ISO 8601 timestamp, or empty when no deadband active                                                       |


## How the thermostat works

The component includes a soft thermostat - connect any ESPHome `sensor` as `temperature_sensor_id` and the component will drive the compressor on/off automatically.

- **Cool / Heat modes**: compressor engages when temperature drifts `hysteresis` past the setpoint; turns off when setpoint is reached
- **Heat/Cool auto mode**: a deadband of `auto_deadband` °C separates the heat and cool engagement thresholds, preventing the system from bouncing between modes after an overshoot. After `auto_deadband_timeout` of idle, the protection expires so the unit can respond to slow thermal drift (e.g. afternoon sun)

A `temperature_sensor_id` is required for the compressor to engage in cool, heat, or heat/cool modes. Without one, `thermostat_direction` remains idle and the unit will only run the fan (if a non-auto fan speed is set). Fan-only mode works regardless of whether a sensor is configured. TODO: support manual control 😃

## Protocol

See [docs/protocol.md](docs/protocol.md) for the full reverse-engineered protocol documentation, including physical layer, bit encoding, frame format, and observed behaviour.

## Testing

The compressor protection and thermostat logic is covered by a native C++ test suite using [Catch2](https://github.com/catchorg/Catch2). Tests run on the host (no ESP required) using a fake `millis()` clock for deterministic time control.

```sh
cd tests
./run_tests.sh
```

Requires CMake ≥ 3.14 and a C++17 compiler. Catch2 is fetched automatically via CMake FetchContent.

The test suite covers:

- Compressor cooldown on restart
- Heat->cool and cool->heat direction changes with full sequencing
- Reversing valve held during cooldown, settle timer enforced after
- Thermostat hysteresis and HEAT_COOL deadband behaviour
- Rapid mode change and sensor noise stress tests
- Regression tests for production bugs

## Tested on

- ESP32-C3
- ESP32-P4
- ESP32-S3

## License

MIT