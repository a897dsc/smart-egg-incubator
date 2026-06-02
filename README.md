# Automated Egg Incubator — Arduino Mega

> Graduation Project | Embedded Systems | Arduino Mega 2560

A fully automated egg incubator controller built on the Arduino Mega 2560.  
The system manages temperature, humidity, ventilation, egg flipping, water refill, and door security — all running concurrently without blocking delays.

---

## Features

- **Temperature control** — Heating lamp relay with hysteresis band to prevent relay chatter
- **Humidity control** — Humidifier + fan relay with hysteresis, auto-disabled during ventilation
- **Automatic ventilation** — Servo-driven vent flap + fan, triggered by time, temperature, or humidity excess
- **Automatic egg flipping** — Non-blocking stepper motor (TB6600 driver) rotates eggs on a configurable schedule
- **Water auto-refill** — Debounced float/IR water level sensor opens a valve relay when water is low
- **Door alarm** — IR sensor detects open incubator door and triggers a buzzer alert
- **Day-based auto targets** — Temperature and RH targets adjust automatically by incubation day
- **16×2 LCD display** — Live readout of temperature, humidity, valve state, vent state, flip countdown, and door status
- **4×4 keypad UI** — On-device settings for temperature, humidity target, and current day
- **Serial monitor control** — ARM/DISARM flip, manual start, safe-stop, and telemetry via single-char commands
- **Fully non-blocking** — All subsystems run via `millis()`/`micros()` state machines; zero `delay()` calls

---

## Hardware List

| Component | Model / Spec | Qty |
|---|---|---|
| Microcontroller | Arduino Mega 2560 | 1 |
| Temperature & Humidity Sensor | DHT11 | 1 |
| LCD Display | 16×2 with I2C backpack (0x27) | 1 |
| Keypad | 4×4 membrane matrix | 1 |
| Servo Motor | Standard PWM servo | 1 |
| Stepper Motor Driver | TB6600 | 1 |
| Stepper Motor | NEMA 17 (200 steps/rev) | 1 |
| Relay Module | 5V active-LOW, 5-channel | 1 |
| Heating Lamp | Incandescent / IR lamp | 1 |
| Ventilation Fan | DC fan | 1 |
| Humidifier + Fan | Ultrasonic humidifier module | 1 |
| Water Valve | Solenoid valve (relay-controlled) | 1 |
| Water Level Sensor | Float switch or IR level sensor | 1 |
| IR Sensor (door) | Digital IR proximity sensor | 1 |
| Buzzer | Active buzzer (active-LOW) | 1 |

---

## Wiring Summary

| Pin | Connected To |
|---|---|
| 3 | DHT11 data |
| 4 | Servo signal |
| 5 | Humidity relay (humidifier) |
| 6 | Humidity fan relay |
| 8 | TB6600 ENABLE |
| 10 | TB6600 STEP |
| 11 | TB6600 DIR |
| 12 | Heating lamp relay |
| 40 | Ventilation fan relay |
| 42 | Water valve relay |
| 43 | Water level sensor (INPUT_PULLUP) |
| 47 | Buzzer |
| 49 | IR door sensor |
| 22–25 | Keypad columns |
| 26–29 | Keypad rows |
| SDA/SCL (20/21) | LCD I2C backpack |

> All relay modules are **active-LOW** (LOW = ON, HIGH = OFF).

---

## Circuit Diagram

![Circuit Diagram](images%20for%20project/1.jpg)

> Additional hardware photos:

| | | |
|---|---|---|
| ![](images%20for%20project/2.jpg) | ![](images%20for%20project/3.jpg) | ![](images%20for%20project/4.jpg) |
| ![](images%20for%20project/5.jpg) | ![](images%20for%20project/6.jpg) | |

---

## Libraries Required

Install all libraries via **Arduino IDE → Tools → Manage Libraries**:

| Library | Version tested |
|---|---|
| `LiquidCrystal_I2C` (Frank de Brabander) | 1.1.2 |
| `Keypad` (Mark Stanley, Alexander Brevig) | 3.1.1 |
| `Servo` (built-in Arduino) | — |
| `DHT sensor library` (Adafruit) | 1.4.x |
| `Wire` (built-in Arduino) | — |

---

## How to Flash

1. **Install** [Arduino IDE](https://www.arduino.cc/en/software) (version 1.8.x or 2.x)
2. **Install** all required libraries listed above
3. **Rename** `3- Last Version of code.txt` → `egg_incubator.ino`  
   *(or open the `.txt` file and paste its contents into a new Arduino sketch)*
4. **Select board:** `Tools → Board → Arduino Mega or Mega 2560`
5. **Select port:** `Tools → Port → COMx` (your Mega's COM port)
6. **Upload:** Click the Upload button (→)
7. **Open Serial Monitor** at **9600 baud** to view telemetry and send commands

---

## Serial Commands

Once flashed, open Serial Monitor (9600 baud) and use these single-key commands:

| Key | Action |
|---|---|
| `H` | Show help |
| `R` | ARM the flip system |
| `U` | DISARM the flip system |
| `S` | Start a flip session immediately |
| `X` | Safe-stop (finish current move, then stop) |
| `A` | Toggle auto-schedule ON/OFF |
| `T` | Print flip timing info |

---

## Keypad UI

| Key | Action |
|---|---|
| `A` | Enter settings mode |
| `1` | Select temperature target |
| `2` | Select humidity (RH) target |
| `3` | Select current incubation day |
| `*` | Insert decimal point (temperature only) |
| `#` | Confirm / save value |
| `D` | Delete last character |
| `C` | Exit settings without saving |
| `B` | Reset all targets to automatic defaults |

---

## Incubation Parameters (Auto Mode)

| Day Range | Target Temp | Target RH | Vent Interval |
|---|---|---|---|
| Days 1–7 | 37.6°C | 56% | Every 30 min |
| Days 8–18 | 37.6°C | 60% | Every 20 min |
| Days 19–21 | 37.6°C | 65% | Every 15 min |

---

## Project Structure

```
egg-incubator/
├── 3- Last Version of code.txt   # Main Arduino sketch
├── Presentation Egg Incubator.pdf
├── Report About Project.pdf
├── Egg Incubator demo .mp4
└── images for project/
    ├── 1.jpg
    ├── 2.jpg
    ├── 3.jpg
    ├── 4.jpg
    ├── 5.jpg
    └── 6.jpg
```

---

## License

This project is submitted as a graduation project. All rights reserved by the author.
