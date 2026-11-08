# Hydroponic Controller

An ESP32 controller for an NFT hydroponic setup. It watches pH, TDS, water level, flow, temperature and four moisture probes, tops up the reservoir, doses nutrients when TDS gets low, runs the aeration pump, switches an AC/dehumidifier relay, and raises an alarm when pH or TDS go out of range. Everything shows on the 16x2 LCD, and if Wi-Fi is available there is a small JSON status endpoint.

Full write-up and wiring guide: [docs/Report5_Hydroponic_Controller.docx](docs/Report5_Hydroponic_Controller.docx)

## Pin map

| Part | Pin |
|---|---|
| DHT11 | 19 |
| LDR | 35 |
| HC-SR04 (water level) | trig 25, echo 26 |
| CD4051 common out | 36 |
| CD4051 select A / B / C | 4 / 2 / 5 |
| Water flow sensor | 23 |
| Main pump (MOSFET, PWM) | 33 |
| Oxygen pump (L293D IN1) | 27 |
| Nutrient pump (L293D IN3) | 12 |
| Water source valve (L293D IN2) | 14 |
| Nutrient valve (L293D IN4) | 13 |
| L293D enable | 22 |
| AC / dehumidifier relay | 3 |
| Buzzer | 15 |
| Green status LED | 18 |
| LCD (data / clock / enable) | 17 / 16 / 21 |

Mux channels: 0 pH, 1 TDS, 2 moisture 1, 3 free, 4 moisture 2, 5 moisture 3, 6 moisture 4, 7 potentiometer.

## What it does each cycle

1. Temp above 28 C turns the AC relay on, below 24 C turns it off.
2. LDR under 500 sets the "lights off" flag.
3. Water level is more than the threshold away from the sensor: open the source valve and run the main pump. Back within range: close and stop.
4. pH, TDS and the four moisture probes are read through the mux.
5. pH outside 5.5-6.5 or TDS above 1400 ppm: buzzer on, LED off. Otherwise buzzer off, LED on.
6. TDS under 800 ppm: open the nutrient valve and run the nutrient pump for 5 seconds, then wait two minutes before dosing again so the mix can settle.
7. LCD flips between two pages every 3 seconds.
8. Oxygen pump stays on. Flow rate comes from the pulse counter.

The potentiometer on mux channel 7 sets the water level threshold between 5 and 30 cm. Turn that off with `POT_SETS_LEVEL`.

## Setup

1. Install: DHT sensor library (plus Adafruit Unified Sensor) and LiquidCrystal_SR.
2. Copy `secrets.example.h` to `secrets.h` and put in your Wi-Fi. The controller keeps working without Wi-Fi.
3. Upload `hydroponic_controller.ino`. Open the serial monitor at 115200 to see a status line every 5 seconds.
4. If Wi-Fi connects, `http://<ip>/status` returns everything as JSON.

## Calibrate before trusting it

| What | How |
|---|---|
| pH | Read the raw value in pH 4 and pH 7 buffer, put them in `PH_RAW_AT_4` and `PH_RAW_AT_7`. Until then it uses a rough placeholder formula. |
| TDS | Check against a known EC standard. Set `ADC_DIVIDER` to 0.4 if you fitted the 33k/22k divider on the signal line. |
| Moisture | Note raw readings in dry air and in water, set `MOIST_DRY_RAW` and `MOIST_WET_RAW`. |
| Water level | Measure sensor-to-surface at full and empty and tune the threshold. |
| Flow | Flow in L/min is pulse frequency divided by `FLOW_HZ_PER_LPM` (7.5 for a YF-S201). Check with a measuring jug. |

## Heads up

- GPIO 3 (relay) is the serial RX pin, and GPIO 12 and 15 are boot strapping pins. If the board refuses to boot or flash with things wired up, disconnect those loads first.
- Test each pump, valve and sensor on its own before running everything together.
- Keep a 12 V supply with at least 5 A of headroom.
