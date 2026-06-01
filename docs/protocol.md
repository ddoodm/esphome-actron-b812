# Actron B812 / Leasam LE85 protocol

This document describes the wire protocol used between Actron Controls wall controllers (**B812RT**, likely also **B812** / **B512CW** / **LE612CW** / **LE75CW**) and the Leasam **LE85** family of relay boards (**LE85**, **LE85R3**, **LE85R3-1**). Reverse engineered with an oscilloscope and a logic analyser, against a B812RT and an LE85R3. Pin labels, defaults and error codes are cross-referenced against the official **B812RT installation manual** ([scan](b812rt_manual_install.png)) and the original **Leasam Controls product catalogue** ([PDF](Leasam-catalogue.pdf)).

> **Provenance**: The LE85 relay module and these wall controllers were originally designed by **OEM Electronics** and sold under the **Leasam Controls** brand (the catalogue states the LE85 "is the original microprocessor controller specifically developed by OEM Electronics for water cooled air conditioning systems") before being rebadged by **Actron Controls**. The Leasam catalogue documents the `B512CW` / `LE612CW` / `LE75CW` / `LE75CA` wall pads and the `LE85` / `LE85R3` relay modules as one interoperable family, which is the basis for several of the cross-references below.

> **Caveat**: Everything about the protocol bits and timings is based on observation of a single installation. Some behaviours may vary across LE85 variants, controller revisions or installation configurations (e.g. with a condenser pump interlock fitted, or with electric heat instead of reverse cycle - see [LE85 DIP switches](#le85-dip-switches-configured-at-install)). Corrections, additions and contradicting observations are very welcome - please open an issue.

## Hardware context

The LE85 relay board lives inside the indoor unit and switches the actual mains-voltage loads: compressor contactor, supply fan, reversing valve solenoid, zone damper actuators and (optionally) electric heat. The wall controller is a low-voltage device that simply tells the relay board what to switch on.

According to the manufacturer wiring diagram for the LE85, the same connector pinout is shared by several controllers: **B512CW**, **LE612CW** and **LE75CW**. This protocol document was captured with an Actron **B812RT**. Retailer documentation describes the B812RT and B512CW as interchangeable on the LE85R3-1, so they almost certainly share this protocol exactly — but I have not personally verified any controller other than my own. The LE612CW and LE75CW share the connector pinout but their protocol compatibility is unverified.

LE85 wiring diagram

The wall controller has surprisingly direct control over the mechanical hardware: bits in the frame map almost one-to-one to relays on the LE85 board (compressor, reversing valve, fan speeds, zones). This means the wall controller is responsible for honouring compressor cooldown timing and reversing valve sequencing - there is no apparent safety logic on the LE85 side.

## Data link layer

### Frame structure

Frames are **8 bits**, transmitted **MSB first**, repeated continuously at ~222.4 ms intervals. Every frame includes a leading and trailing pulse:

```
┌pulse┐  bit 7  ┌pulse┐  bit 6  ┌pulse┐  ...  ┌pulse┐  bit 0  ┌pulse┐
│150µs│  space  │150µs│  space  │150µs│       │150µs│  space  │150µs│
└─────┘ 2 or 6  └─────┘ 2 or 6  └─────┘       └─────┘ 2 or 6  └─────┘
        ms              ms                            ms
```

- **Pulse**: fixed **150 µs** active-low pulse on PWR
- **Space (gap between pulses)** encodes the bit value:
  - **2 ms space** = `0`
  - **6 ms space** = `1`
- **Trailing pulse**: a final 150 µs pulse marks the end of the last bit's space
- **Inter-frame gap**: ~222.4 ms idle (high) before the next frame begins

This is **pulse-distance coding** — the same scheme used by NEC IR remotes. The pulse width itself never varies; the *distance* between pulses carries the bit value.

Single data frame on the scope

Stream of frames showing 222 ms cadence

### Bit map

Bits are listed below from MSB (transmitted first) to LSB (transmitted last):


| Bit | Mask   | Name    | Function                                                   |
| --- | ------ | ------- | ---------------------------------------------------------- |
| 7   | `0x80` | `FS3`   | Fan speed: high                                            |
| 6   | `0x40` | `ZONE1` | Zone 1 damper enable                                       |
| 5   | `0x20` | `ZONE2` | Zone 2 damper enable                                       |
| 4   | `0x10` | `CALL`  | Conditioning requested (see [The CALL bit](#the-call-bit)) |
| 3   | `0x08` | `COMP`  | Compressor on                                              |
| 2   | `0x04` | `FS1`   | Fan speed: low                                             |
| 1   | `0x02` | `HEAT`  | Reversing valve: 0 = cool, 1 = heat                        |
| 0   | `0x01` | `FS2`   | Fan speed: medium                                          |


The fan-speed bits are **one-hot** — at most one of `FS1`, `FS2`, `FS3` is ever set. The original controller never transmits combinations of fan bits.

The zone bits both default to `1`; an installation with no zoning still sees both bits set. The original controller never transmits both zone bits as `0` — at least one is always set — so the LE85's behaviour with both clear has not been observed and is unknown. The Leasam catalogue confirms this is deliberate controller behaviour: *"One zone will always be on. This is to prevent the air conditioning system operating with all zones closed."* The LE85 has exactly **two zone relays** (the 4-/8-zone variants in the catalogue are the larger `BM2` controllers, not the LE85), which matches the two zone bits here.

### Example frames


| Frame (hex) | Binary      | Decoded                                              |
| ----------- | ----------- | ---------------------------------------------------- |
| `0x60`      | `0110 0000` | OFF (both zones, all else clear)                     |
| `0x7C`      | `0111 1100` | Cooling, fan low, both zones, calling, compressor on |
| `0x6E`      | `0110 1110` | Heating, fan low, both zones, calling, compressor on |
| `0xE1`      | `1110 0001` | Fan-only high speed, both zones                      |


## Physical layer

### Pinout

The B812RT has **5 wires** at the controller end:


| Wire    | Direction          | Description                                                                                           |
| ------- | ------------------ | ----------------------------------------------------------------------------------------------------- |
| **PWR** | board → controller | Nominally ~7 V supply rail. Signal is also transmitted on this line by the controller.                |
| **COM** | —                  | Common / 0 V return                                                                                   |
| **SN1** | —                  | Optional remote temperature sensor input (10K3T thermistor, 10 kΩ @ 25 °C). Used for Zone 1 if zoned. |
| **SN2** | —                  | Optional remote temperature sensor input. Used for Zone 2 if zoned.                                   |
| **AUX** | board → controller | Interlock status from the LE85 (see [AUX line](#aux-line) below).                                     |


The `SN1` and `SN2` wires are only required when using external temperature sensors. With DIP switch 7 set ON the controller uses its on-board sensor and these wires can be left disconnected. Many installations therefore only use **3 wires** (PWR, COM, AUX) - including mine.

B812 connector pins

### Voltage levels and signalling

The PWR line is the supply *and* the data line. The wall controller transmits by **shorting PWR briefly to COM** for each data pulse. The line therefore idles **high (~7 V)** and pulses are **active-low**.

Measured voltages (probe across PWR–COM, original B812 controller attached, no load):


| State                    | Voltage |
| ------------------------ | ------- |
| Idle (PWR high)          | ~7.09 V |
| Pulse (active-low short) | ~−0.6 V |


> The −0.6 V undershoot during the active-low pulse is presumably the forward voltage of a clamping diode on the LE85 side.

Single pulse on the scope

### Power budget

The original B812 draws very little current — it has an LCD with a backlight and a few buttons:


| Mode                      | Current |
| ------------------------- | ------- |
| Idle (average)            | ~1.7 mA |
| Peak (during pulse short) | ~35 mA  |


The ~35 mA peaks line up with the active-low pulses — the controller is briefly shorting its own supply rail during each transmission.

To characterise the maximum load the PWR rail can supply, I measured PWR voltage with a scope (CH1 across PWR–COM) and current with a 1.1 Ω shunt in series with the controller's COM (CH2). I then loaded PWR to COM with a series of decreasing-resistance load resistors:


| Load resistor | Steady-state current | PWR (top) | Status         |
| ------------- | -------------------- | --------- | -------------- |
| 324 Ω         | 24 mA                | 7.10 V    | OK             |
| 264 Ω         | 26 mA                | 7.10 V    | OK             |
| 176 Ω         | 40 mA                | 7.08 V    | OK             |
| 118 Ω         | 60 mA                | 6.93 V    | OK             |
| 88 Ω          | 80 mA                | 6.90 V    | OK             |
| 66 Ω          | 100 mA               | 6.96 V    | OK             |
| 27 Ω          | —                    | 0 V       | rail collapsed |


The rail holds up gracefully to at least 100 mA with only modest voltage sag. Somewhere between 100 mA (66 Ω, OK) and ~260 mA (27 Ω, collapse) there is a hard cutoff — the rail goes from "fine" to fully shut down with no observed middle ground. An ESP32 is comfortably above this limit during Wi-Fi TX bursts, so powering the ESP from PWR is risky even though the average current would be within budget. I power my replacement controller from an external 5 V supply instead (see the main [README](../README.md#wiring) for circuit).

Load test setup

## Observed timing behaviour

The wall controller is responsible for sequencing the compressor and reversing valve safely. The LE85 appears to obey whatever it is told instantly — there is no apparent debouncing or protection on the relay board. These are the timings observed from the original B812 controller:

### Compressor minimum off-time

After the compressor is turned off (`COMP` cleared), the original controller will not re-set `COMP` for **~5 minutes**. This is the standard "anti-short-cycle" protection found on most heat pumps — it gives the system pressure time to equalise so the compressor doesn't restart against high head pressure.

The Leasam catalogue confirms this is a **field-adjustable "anti-cycle" off-time** configured on the wall controller (range **0.0–6.0 min**, the manual elsewhere refers to a "6 minute compressor start delay"). So the ~5 min observed here is this unit's configured value, not a fixed protocol constant — and it confirms the *controller*, not the LE85, owns this timer.

### Reversing valve settle

When switching from heat to cool, the original controller appears to wait roughly **30 seconds** after clearing the `HEAT` bit before re-energising the compressor. The reverse direction (cool → heat) does not appear to require this extra settle time, only the standard compressor cooldown.

The Leasam catalogue confirms this is a named, field-adjustable/cancellable controller timer — the **"Reversing Valve Hold In"** delay (one of three controller timers it lists alongside *anti-cycle* and *fan run-on*). The catalogue presents it as a single symmetric value, so it is most likely **armed on any valve change** but only *observable* in the heat → cool direction: in the cool → heat direction the ~5 min anti-cycle is already longer than the hold, so it is masked and never appears as a separate delay.

Physically this makes sense (this rationale is inferred, not stated in the catalogue): the reversing valve is actuated by refrigerant pressure differential, and switching it while the compressor is still pressurised is the most stressful thing you can do to it.

### Boot sequence

For roughly **2 seconds after power-on**, the original controller transmits all-zero frames (`0x00`) — no zones, no fan, no compressor, no anything. This component does not currently emulate this behaviour; it begins transmitting its desired state immediately.

### CALL / COMP de-coupling

The `CALL` bit is *not* always synchronised with `COMP`. During the compressor cooldown window, `COMP` is `0` (compressor off) but `CALL` may be `1` (the thermostat still wants conditioning, but the cooldown timer is preventing the compressor from starting)

## The `CALL` bit

`CALL` correlates with "the thermostat is currently calling for conditioning":

- `CALL = 1` when the room is past the engage threshold and the controller wants the compressor on
- `CALL = 0` when the thermostat is satisfied (within hysteresis of setpoint)

The Leasam catalogue explains what it is **for**: it drives the LE85's condenser-water-**pump interlock**. The LE85 has a physical `CALL` output terminal, and the pump-interlock sequence is documented as:

> 1. On a request for compressor to operate a signal is sent from relay module `CALL` terminal to Pump Interlock Relay Module
> 2. Pump ON confirmation signal is received via the relay module `VERIFY` terminal. Compressor start enabled.

So on a WSHP the chain is: wall-controller `CALL` bit → LE85 asserts its `CALL` output → external condenser-water pump starts → flow confirmed on the LE85's `VERIFY` input → compressor permitted. `CALL` is the **request to spin up the condenser water loop** before the compressor runs. (If pump flow is never confirmed, the LE85 shows fault `EP` and refuses to start the compressor.)

This also explains the `**CALL` / `COMP` de-coupling** noted above: the controller keeps `CALL` asserted (pump requested/running) through the compressor anti-cycle window even while `COMP` is held off.

Why it *looked* vestigial during reverse engineering: in the installation under test the pump interlock is bypassed (the LE85's `AUX "B"` is linked to ground / interlock disabled), so asserting `CALL` produces no externally observable effect — the compressor is gated only by `COMP`. On an install with a real condenser pump tower, `CALL` is functionally essential.

This component sets `CALL` whenever the thermostat is engaged, regardless of the compressor state, to match what the original controller transmits.

### AUX line

The B812RT installation manual ([scan](b812rt_manual_install.png)) clarifies the AUX function. The LE85 has **two separate AUX inputs**, both volt-free dry-contact inputs to GND:

- `**AUX IN (A)`** — **Remote ON/OFF**. *"Connect isolated Switch (by others) between AUX IN (A) and GND. Close to Start, Open to Stop."* This is intended for building-management interlocks (e.g. a hotel/apartment master cut-off). When open, the system refuses to call for conditioning. The wall controller's ON/OFF buttons still work locally.
- `**AUX IN (B)`** — **Condenser pump interlock**. *"Connect link between AUX IN (B) to GND if condenser pump interlock is not required or set up Off in TIMERS MENU."* In a WSHP installation with an external pump tower, this confirms the pump is running before the compressor is allowed to start. If unused, it must be hard-linked to GND.

> **Terminology note (Leasam vs Actron)**: The Leasam catalogue names these differently. The remote ON/OFF is the `**KEY`** terminal ("key card" / home-automation interface — *"Connect volt free contact between terminal KEY and SEN COM. Open contact stops A/C, closed contact A/C restarted from Wall Control"*), and the pump-interlock override is `**AUX "B"*`* (*"Link terminals AUX 'B' and SEN COM on relay module ... to override pump interlock"*). So Actron's `AUX IN (A)` is Leasam's `KEY`, and `AUX IN (B)` is Leasam's `AUX B`.

The wall controller's `AUX` wire reads back interlock/status state from the LE85 and allows the controller to call for conditioning. It is probably **richer than a single "interlock satisfied" bit**: the Leasam catalogue lists several *distinct* fault codes the wall control displays when connected to an LE85 — `EP` (pump verify not received), `EC` / `E-C` (HP/LP pressure trip), `EH` / `E-H` (heater high-temp), plus sensor open/short codes ("Additional fault displays when B512CW is connected to LE85 Relay Module"). A single binary line could not convey all of those, so the back-channel almost certainly carries multiplexed status — though the exact wire and encoding are not yet confirmed.

In my installation:

- `AUX` sits at ~0 V with significant noise
- Voltage on `AUX` shows ~1.1 V pulses synchronous with the PWR-line data pulses — almost certainly **inductive coupling** from the PWR line being shorted, not real signalling
- With the original controller installed and `AUX` **disconnected**, the controller will run the fan but **will not call for conditioning** (compressor never starts) — exactly what the manual describes
- With `AUX` connected, the controller behaves normally

This component does not implement `AUX` in either direction — it always commands as if the interlock is satisfied. **If your installation has an active remote on/off or pump interlock, those need to be wired and configured on the LE85 itself**; the LE85 will physically prevent the compressor from running when those interlocks are not satisfied, regardless of what this controller transmits.

### Safety inputs (cool / heat)

The LE85 has two dedicated safety-trip inputs which are **independent of the wall controller protocol**:

- `**COOL SAFETY*`* — typically wired to high-pressure (HP) and low-pressure (LP) refrigerant switches
- `**HEAT SAFETY**` — typically wired to a high-temperature (HT) cut-off switch

Per the manual, when no fault is present these sit at **+14 Vdc relative to GND**. When a switch trips (e.g. high refrigerant pressure), the LE85 displays an error and **physically refuses to start the compressor regardless of what the wall controller commands**.

This is significant for ESPHome replacements: the wall controller is *not* in the safety loop. There is no way for a misbehaving wall controller (or a buggy ESPHome component) to bypass these safety switches. The LE85 is the safety enforcer; the wall controller is purely a request channel.

## LE85 reference (from the B812RT manual)

Useful constants and error codes from the official B812RT installation manual ([scan](b812rt_manual_install.png)):

### Factory defaults


| Setting                             | Default  | Adjustable range |
| ----------------------------------- | -------- | ---------------- |
| Setpoint range                      | 16–28 °C | 12–32 °C         |
| Switching differential (hysteresis) | 0.5 °C   | field-adjustable |
| Deadband (heat off → cool on)       | 1.5 °C   | field-adjustable |
| Fan run-on (electric heat only)     | 60 s     | field-adjustable |
| Run timer (serviced apartments)     | None     | 3 / 6 / 9 hr     |


### LE85 hardware spec

- Mains supply: 240 VAC 50 Hz
- Compressor contactor: 3 HP @ 250 VAC
- Heater relay: 30 A resistive @ 250 VAC
- Zone & fan relays: 16 A resistive @ 250 VAC (×5 — 2 zones, 3 fan speeds)
- On-board sensor: 10K3T thermistor, 10 kΩ @ 25 °C

### B812 DIP switches


| Switch | OFF                                                     | ON                                   |
| ------ | ------------------------------------------------------- | ------------------------------------ |
| 1      | Cool only + electric heat (`COOL/ELEC`)                 | Reverse-cycle heat pump (`HEATPUMP`) |
| 2      | Continuous indoor fan (`CONT FAN`)                      | Auto / continuous fan selectable     |
| 3      | Single-speed indoor fan (high output)                   | Three-speed indoor fan               |
| 4      | Use only Zone 1 or Zone 2, not both                     | Use Zone 1, Zone 2 or both           |
| 5 / 6  | Run-timer duration selector (None / 3 hr / 6 hr / 9 hr) |                                      |
| 7      | Use remote sensor on SN1                                | Use on-board sensor                  |
| 8      | 6-hr run timer                                          | 24-hr run timer                      |


## Reverse-engineering methodology

For anyone wanting to verify or extend this work:

### Bit identification

Initial bit mapping was done by **systematic single-variable changes**: capture a few seconds of frames with a known controller setting (e.g. fan low, cool mode), change exactly one setting, capture again, and diff the bytes.

The bits with persistent state (`FS1/2/3`, `HEAT`, `ZONE1/2`) fell out almost immediately. The fan one-hot encoding was obvious from the start.

The `COMP` and `HEAT` bits took longer to confirm because they don't always strictly follow the user's request — the controller adds its own sequencing, e.g. holding `HEAT` set during cooldown to protect the reversing valve. I had to characterise the timing patterns (with frame-by-frame timestamps) before the underlying meaning of these bits became clear.

A working spreadsheet from this process is preserved here: [Actron B812 aircon signal decode](https://docs.google.com/spreadsheets/d/16ep-1Ss4A2m0h3jxcf53EI_2NZw93tBKChI3bsItK38/edit?usp=sharing).

### Capture chain

1. **Oscilloscope** for initial visualisation of the signal - figuring out the encoding, voltage levels, and the basic frame structure
2. **Logic analyser** to verify timing
3. **Oscilloscope (cursor mode)** voltage / current characterisation

## Open questions

The Leasam catalogue (see [Provenance](#actron-b812--le85-protocol)) answered or narrowed several questions that were previously open:

### Answered / narrowed by the Leasam catalogue

- **What does the LE85 do with the `CALL` bit?** — It drives the **condenser-water-pump interlock**: `CALL` → LE85 `CALL` output → pump start → `VERIFY` flow confirmation → compressor permitted. See [The `CALL` bit](#the-call-bit). It only appeared inert because the test install has the pump interlock bypassed.
- **Does the `AUX` line carry only "interlock satisfied", or more?** — Almost certainly **more**: the catalogue lists multiple distinct fault codes (`EP`/`EC`/`EH`/sensor faults) the wall control displays when connected to an LE85, which a binary line couldn't convey. Exact encoding still unconfirmed. See [AUX line](#aux-line).
- **Do `LE612CW` / `LE75CW` use the same encoding?** — Strong supporting evidence: the catalogue documents `B512CW` / `LE612CW` / `LE75CW` as one LE85-compatible wall-pad family sharing an identical control-wiring connector, with `LE612CW` wired identically to `B512CW`. `LE75CW` is compatible but has **no zone control** (so would not drive the zone bits). Not yet confirmed at the individual-bit level.
- **With DIP switch 1 in `COOL/ELEC` mode, what does `HEAT` drive?** — Supporting evidence that it drives electric heat: on the LE85 the relevant relay output is labelled **"EDH or Rev. Valve"** (Electric Duct Heater *or* Reversing Valve), i.e. one shared output repurposed by system type. So the `HEAT` bit most likely drives that relay either way — electric heat in COOL/ELEC, reversing valve in heat-pump mode. Worth a hardware confirmation.

### Still open

- What does the LE85 do if it sees `COMP=1` and `HEAT` toggling rapidly? (This component prevents that, but how forgiving is the hardware?)
- Are there any frame values or sequences that put the LE85 into a special mode (diagnostics, fault, install)? — *Probably not*: the catalogue shows all commissioning/service is done via wall-control button combinations and DIP switches, local to the controller, with no indication of a special bus mode.
- The exact wire and encoding of the `AUX` status back-channel (which fault states it can represent, and how).

If you have an LE85 system and can answer any of these, please open an issue or PR.