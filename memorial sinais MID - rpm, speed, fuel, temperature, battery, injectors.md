# Technical Memorial: Signal Characterization — Astra MID Reader (Phase 1)

**Device:** Arduino Nano (ATmega328P, 16MHz) / **Vehicle:** Brazilian 1999 Chevrolet Astra (ECM Bosch Motronic 1.5.5) with MID.

---

### PIN 2 — PIN_TACHO

* **Signal description:** Engine crankshaft rotation frequency (tachometer).
* **Signal origin:** Negative square wave emitted by the ECU, conditioned and inverted by an optocoupler PC817 before reaching the Arduino. The resulting signal at the Arduino pin is a positive square wave, active HIGH at rest (`INPUT_PULLUP`), pulsing LOW on each engine event.
* **Electrical characteristics:** Signal shared with the OEM instrument cluster stepper motor driver. Series resistor 10kΩ on optocoupler LED input. Optocoupler collector pulled up internally by Arduino `INPUT_PULLUP` (~50kΩ to 5V). Pull-up resistor on optocoupler LED side measured at ~2.2kΩ recommended for reliable LED saturation at 5mA.
* **Observed behavior:** At idle (~950–1000 RPM), period measured at ~31.500µs (~31.7Hz). At 2600 RPM, period drops to ~11.500µs (~86Hz). Signal is stable and consistent across sessions. Pulse count per window of 500ms at idle: 16–19 pulses, consistent with ~32Hz and two pulses per crankshaft revolution (motor 4-cylinder, 2 events per cycle). Between 15 and 150 pulses can occur within each 500ms interval depending on engine speed.
* **Interrupt:** External interrupt INT0, FALLING edge, `attachInterrupt()`.
* **RPM calculation:** $$\text{RPM} = \frac{60 \times 10^6}{\text{period}(\mu\text{s}) \times N_{\text{teeth}}}$$
  where $N_{\text{teeth}}$ is the number of reluctor teeth per revolution — confirmed empirically at 2 events/rev for this engine.
* **Notes for future development:** This signal is reliable and fully characterized. For a future OBD computer, this pin can directly drive RPM display, shift-light logic, and engine load calculations. Timer Input Capture (ICP1, pin 8) should be preferred over external interrupt in a redesign, as it timestamps edges in hardware with zero jitter, freeing CPU cycles and improving accuracy at high RPM.

---

### PIN 3 — PIN_SPEED

* **Signal description:** Vehicle road speed (speedometer).
* **Signal origin:** Presumably identical architecture to PIN_TACHO — negative square wave from ECU, conditioned via optocoupler, shared with OEM instrument cluster.
* **Electrical characteristics:** Same isolation and protection scheme as PIN_TACHO. `INPUT_PULLUP` active.
* **Observed behavior:** Signal confirmed functional during vehicle movement. The frequency of pulses can be user to determine vehycle speed.
* **Interrupt:** External interrupt INT1, FALLING edge, `attachInterrupt()`.
* **Speed calculation:** $$V_{\text{km/h}} = \frac{3600 \times 10^6}{\text{period}(\mu\text{s}) \times K}$$
  where $K$ is the pulses-per-kilometer constant. For this project $K$ = 15600.

---

### PIN 5 — PIN_TEMP_PWM

* **Signal description:** Engine coolant temperature indicator, as seen by the OEM instrument cluster stepper motor driver.
* **Signal origin:** PWM signal generated internally by the OEM instrument cluster to drive the stepper motor of the temperature gauge. The ECU sends temperature data to the cluster via a separate bus (likely serial or analog); the cluster translates this into a PWM duty cycle to position the stepper motor pointer. The Arduino taps this PWM line directly at the cluster connector.
* **Electrical characteristics:** No optocoupler isolation. Signal voltage is 5V-compatible (cluster logic level). `INPUT_PULLUP` active. Signal absent (pin stays HIGH via pull-up) when cluster is unpowered or when coolant temperature is below the gauge's lower threshold (~70°C), because the stepper motor driver receives no movement command in that range.
* **Observed behavior:**

| Temperature (°C) | TEMP_PWM Duty (%) | Observation |
| :--- | :--- | :--- |
| <70 (cold engine) | 90.2% (fixed) | Pointer at rest, no stepper command |
| 70 | 89.5% | Pointer begins to move |
| 80 | 72.0% | Normal operating range |
| 85 | 64.0% | Radiator fan #1 activated by ECU |
| ~90 (extrapolated) | ~55% | Estimated, not yet logged |

* **Signal period:** stable at ~9.920–9.940µs (~100Hz) across all conditions. This frequency is the fixed oscillator clock of the stepper motor driver IC inside the cluster. The duty cycle encodes the pointer position command.
* **Logic polarity:** Signal is logically inverted relative to temperature. Higher temperature → lower duty cycle. With `INPUT_PULLUP`, the duty measured is the HIGH time; the ECU-commanded LOW time increases with temperature. Effective temperature-encoding duty: $\text{duty\_real} = 100\% - \text{duty\_measured}$.
* **Read method:** `pulseIn()` executed once every 10 seconds (`TEMP_INTERVAL`), to avoid blocking the main loop and corrupting `INJ_PULSE` capture. This cadence is more than sufficient given the thermal time constant of the cooling system (observed ~4 minutes from 70°C to 85°C).
* **Notes for future development:** A multi-point calibration table mapping $\text{duty\_real} to °C should be built from controlled logged sessions with a reference thermometer. Preliminary curve suggests linear or mildly nonlinear behavior between 70°C and ~110°C. The 100Hz fixed period is a reliable sanity-check signal — if period deviates significantly from ~10ms, it indicates cluster power loss or hardware fault. In a redesign, this pin should be read via PCINT2 with asynchronous edge capture to completely eliminate `pulseIn()` blocking.

---

### PIN 8 — PIN_INJECTOR

* **Signal description:** Fuel injector signal analysis.
* **Signal origin:** Negative pulse signal from ECU driving the fuel injectors. Signal is non-periodic — each pulse is an independent event corresponding to one injection event. The injector pulse duration is fixed, meaning that only the pulse frequency varies to control fuel delivery; higher frequency indicates higher fuel consumption. Signal conditioned via optocoupler PC817.
* **Electrical characteristics:** Same optocoupler isolation as TACHO. `INPUT_PULLUP` active. With `INPUT_PULLUP`, pin rests HIGH; injector opening event pulls pin LOW.
* **Observed behavior:** Between 15 and 150 injector pulses can occur within each 500ms measurement window. Current baseline calculation assumes a consumption rate of 22,700 pulses per liter of fuel injected, although this scaling factor can be modified or calibrated later.
* **Notes for future development:** This hardware channel requires the use of **Timer1 Input Capture (ICP1)** to properly monitor the incoming pulse events without missing rapid triggers.

---

### PIN A2 — PIN_BATT

* **Signal description:** Vehicle electrical system voltage (battery / alternator).
* **Signal origin:** Direct resistive voltage divider tap from the vehicle 12V bus. No isolation.
* **Electrical characteristics:** Voltage divider ratio calibrated empirically. Observed range: 0.00V (ignition off, Arduino unpowered) to 14.09V (alternator charging at ~2600 RPM). Resting battery voltage: ~12.20–12.23V (engine off, ignition on). Charging voltage: 13.60–14.09V (engine running, consistent with a healthy alternator regulator setpoint of ~14.2V ±0.3V).
* **Observed behavior:** Voltage drop to ~10.14V observed at engine crank (starter motor inrush current). Instantaneous drop to ~13.60–13.68V correlated with radiator fan activation, consistent with ~15–20A fan motor load. Voltage collapse to ~1.09V immediately at ignition key-off, reaching 0.00V within ~1.5 seconds as filter capacitors discharge.
* **Notes for future development:** ADC reference on the Arduino Nano is tied to VCC (5V), which itself may fluctuate with USB/vehicle supply variations. For improved accuracy, use the internal 1.1V bandgap reference with a better-ratio divider, or use an external precision voltage reference (e.g., LM4040). A low-pass RC filter (e.g., 10kΩ + 100µF) on the divider output will suppress alternator ripple and starter transients before the ADC pin.

---

### PIN A0 — PIN_FUEL

* **Signal description:** Fuel tank level.
* **Signal origin:** Potentiometer-type resistive sensor inside the fuel tank. Voltage read via resistive divider at the instrument cluster signal line.
* **Electrical characteristics:** No isolation. Voltage range observed: 5.82V–6.88V across all sessions, with the vehicle presumably at a fixed fuel level (parked). Divider ratio not yet calibrated against a known fuel level.
* **Observed behavior:** Signal is stable with low noise at rest. Minor fluctuations of ±0.06V observed, likely due to ADC noise and small variations in the vehicle supply. No sloshing artifact observed (vehicle stationary throughout all logged sessions).
* **Notes for future development:** Fuel sender resistance curves are nonlinear and vehicle-specific. A minimum 5-point calibration table (empty, 1/4, 1/2, 3/4, full) is required, measured with a reference fuel gauge and known tank volumes. Implement a software moving-average filter (8–16 samples) to suppress sloshing noise during vehicle movement. The observed voltage range of ~5.82–6.88V suggests the divider ratio needs adjustment to use the full ADC range (0–5V), improving resolution from the current ~12-bit effective range to the full 10-bit ADC range.

---

### General Notes for Future On-Board Computer Development

The signal set captured is sufficient to compute in real time: engine RPM, vehicle speed, coolant temperature (after calibration), fuel level (after calibration), battery state, injector frequency, and estimated fuel consumption (based on the fixed pulse-per-liter calculation).