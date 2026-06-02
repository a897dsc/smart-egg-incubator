#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Servo.h>
#include "DHT.h"

// ================== LCD (16x2) ==================
#define LCD_COLS 16
#define LCD_ROWS 2
LiquidCrystal_I2C lcd(0x27, LCD_COLS, LCD_ROWS);

const unsigned long LCD_REFRESH_MS = 250;
unsigned long lastLcdMs = 0;

// ================== Hardware ==================
#define SERVO_PIN      4

// Ventilation Fan relay (Active-LOW)
#define VENT_RELAY_PIN 40
#define FAN_ON         LOW
#define FAN_OFF        HIGH

// Heating Lamps relay (Active-LOW)
#define HEAT_RELAY_PIN 12
#define HEAT_ON        LOW
#define HEAT_OFF       HIGH

// Humidity relays (Active-LOW)
#define HUM_RELAY_PIN      5
#define HUM_FAN_RELAY_PIN  6
#define HUM_ON             LOW
#define HUM_OFF            HIGH

// DHT
#define DHT_PIN   3
#define DHT_TYPE  DHT11

// Servo angles
#define SERVO_CLOSED 90
#define SERVO_OPEN   130

// ================== Stepper (TB6600) ==================
#define DIR_PIN      11
#define STEP_PIN     10
#define ENABLE_PIN    8

#define STEPS_PER_REV 200
#define MICROSTEPS    8
#define TARGET_ANGLE  97
#define SLOW_DELAY_US 3500

#define FLIP_DURATION_MINUTES 2
#define FLIP_INTERVAL_HOURS   0
#define FLIP_INTERVAL_MINUTES 3

const unsigned long FLIP_PAUSE_MS = 100;

// ================== Flip ARM + Boot Delay ==================
const unsigned long FLIP_ARM_DELAY_MS = 3UL * 60UL * 1000UL; // 3 minutes
bool flipArmed = false;
unsigned long bootTimeMs = 0;
bool flipSafeStopRequested = false;

// ================== Water Auto-Refill ==================
const int waterLevelPin = 43;  // INPUT_PULLUP
const int valveRelayPin = 42;  // relay Active-LOW

#define VALVE_OPEN   LOW
#define VALVE_CLOSED HIGH

// Sensor: HIGH => water low, LOW => water OK
#define WATER_LOW_READING HIGH

const unsigned long WATER_STABLE_MS = 2500;
bool waterLowRaw = false;
bool waterLowStable = false;
bool valveOpen = false;
unsigned long waterLastChangeMs = 0;

// ================== Door Alarm (IR + Buzzer) ==================
#define IR_PIN      49
#define BUZZER_PIN  47

// Confirmed by you:
#define DOOR_CLOSED_READING LOW
#define DOOR_OPEN_READING   HIGH

#define BUZZER_ON   LOW
#define BUZZER_OFF  HIGH

// Door debounce (stability filter)
const unsigned long DOOR_STABLE_MS = 250;
bool doorRaw = false;
bool doorStable = false;
unsigned long doorLastChangeMs = 0;

bool buzzerOn = false;
bool lastDoorStable = false;

// ================== Keypad (Rows 26..29, Cols 22..25) ==================
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'D','C','B','A'},
  {'#','9','6','3'},
  {'0','8','5','2'},
  {'*','7','4','1'}
};

byte rowPins[ROWS] = {26, 27, 28, 29};
byte colPins[COLS] = {22, 23, 24, 25};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ================== Objects ==================
Servo ventServo;
DHT dht(DHT_PIN, DHT_TYPE);

// ================== Ventilation State Machine ==================
enum VentState {
  VENT_IDLE,
  VENT_OPEN,
  VENT_FAN_RUNNING,
  VENT_FAN_STOPPED,
  VENT_CLOSE
};

VentState ventState = VENT_IDLE;

enum VentReason {
  REASON_NONE,
  REASON_TIME,
  REASON_HUMIDITY,
  REASON_TEMP
};

VentReason lastVentReason = REASON_NONE;

// ================== Time ==================
unsigned long now;
unsigned long ventStateStartMs = 0;
unsigned long lastVentTime = 0;

// DHT scheduling
unsigned long lastDhtReadTime = 0;
const unsigned long DHT_READ_INTERVAL = 2000;

// Telemetry scheduling
unsigned long lastPrintTime = 0;
const unsigned long PRINT_INTERVAL_IDLE = 1000;
const unsigned long PRINT_INTERVAL_FLIP = 3000;

// ================== Incubation ==================
int day = 1;
int eggCount = 30;

// ================== Targets ==================
float targetTempAuto = 37.6;
float targetTemp = 37.6;
int targetRH = 56;

bool tempManual = false;
bool rhManual = false;

// Heating hysteresis
const float HEAT_HYST_ON  = 0.2;
const float HEAT_HYST_OFF = 0.0;

// Minimum relay toggle
const unsigned long HEAT_MIN_TOGGLE_MS = 10000;
const unsigned long HUM_MIN_TOGGLE_MS  = 5000;
unsigned long lastHeatToggleMs = 0;
unsigned long lastHumToggleMs  = 0;

// Humidity hysteresis
const int RH_ON_DELTA  = 2;
const int RH_OFF_DELTA = 0;

// ================== Durations (VENT) ==================
unsigned long servoMoveTime   = 600;
unsigned long fanRunTime      = 180000;
unsigned long pauseAfterFan   = 1000;

unsigned long ventInterval;

// ================== Cached readings ==================
float tempC = NAN;
float rhPct = NAN;

// ================== Output states ==================
bool heatingOn = false;
bool humidityOn = false;
bool humidityBlockedByVent = false;

// =====================================================
// =============== STEPPER NON-BLOCKING =================
// =====================================================
enum FlipState {
  FLIP_IDLE,
  FLIP_SESSION_START,
  FLIP_MOVE_CW,
  FLIP_PAUSE_1,
  FLIP_MOVE_CCW,
  FLIP_PAUSE_2,
  FLIP_SESSION_END
};

FlipState flipState = FLIP_IDLE;
bool autoFlipEnabled = true;
bool isFlippingSession = false;

unsigned long lastFlipTime = 0;

unsigned long armTimeMs = 0;
bool firstFlipPendingAfterArm = false;
const unsigned long FIRST_FLIP_AFTER_ARM_DELAY_MS = 60000UL; // 1 minute


unsigned long flipStartTimeMs = 0;
unsigned long flipStateStartMs = 0;

unsigned long flipIntervalMs;
unsigned long flipDurationMs;

long stepsPerMove = 0;
long stepsDoneInMove = 0;
unsigned long cycleCount = 0;

// pulse generator using micros()
unsigned long lastStepToggleUs = 0;
bool stepPinHigh = false;

void enableStepper() { digitalWrite(ENABLE_PIN, HIGH); }
void disableStepper(){ digitalWrite(ENABLE_PIN, LOW);  }

void startMove(bool forward) {
  digitalWrite(DIR_PIN, forward ? LOW : HIGH);
  stepsDoneInMove = 0;
  stepPinHigh = false;
  digitalWrite(STEP_PIN, LOW);
  lastStepToggleUs = micros();
}

bool runMoveNonBlocking() {
  unsigned long nowUs = micros();
  if (nowUs - lastStepToggleUs >= (unsigned long)SLOW_DELAY_US) {
    lastStepToggleUs = nowUs;
    stepPinHigh = !stepPinHigh;
    digitalWrite(STEP_PIN, stepPinHigh ? HIGH : LOW);

    if (!stepPinHigh) {
      stepsDoneInMove++;
      if (stepsDoneInMove >= stepsPerMove) {
        digitalWrite(STEP_PIN, LOW);
        stepPinHigh = false;
        return true;
      }
    }
  }
  return false;
}

long computeStepsForAngle(float angleDeg) {
  float stepsF = (angleDeg / 360.0f) * (float)STEPS_PER_REV * (float)MICROSTEPS;
  return (long)(stepsF);
}

char flipStateChar(FlipState s) {
  // single-letter/char indicator for LCD
  switch (s) {
    case FLIP_SESSION_START: return 'S';
    case FLIP_MOVE_CW:       return 'C'; // CW
    case FLIP_PAUSE_1:       return '1';
    case FLIP_MOVE_CCW:      return 'c'; // CCW
    case FLIP_PAUSE_2:       return '2';
    case FLIP_SESSION_END:   return 'E';
    case FLIP_IDLE:
    default:                 return 'I';
  }
}

// ================== Water System ==================
void setValve(bool open) {
  valveOpen = open;
  digitalWrite(valveRelayPin, open ? VALVE_OPEN : VALVE_CLOSED);
}

void updateWaterSystem() {
  int levelState = digitalRead(waterLevelPin);
  bool rawLow = (levelState == WATER_LOW_READING);

  if (rawLow != waterLowRaw) {
    waterLowRaw = rawLow;
    waterLastChangeMs = now;
  }

  if ((now - waterLastChangeMs) >= WATER_STABLE_MS) {
    waterLowStable = waterLowRaw;
  }

  if (waterLowStable) {
    if (!valveOpen) {
      setValve(true);
      Serial.println("[WATER] Stable LOW -> Valve OPEN");
    }
  } else {
    if (valveOpen) {
      setValve(false);
      Serial.println("[WATER] Stable OK -> Valve CLOSED");
    }
  }
}

// ================== Door Alarm (Debounced) ==================
void updateDoorAlarmDebounced() {
  int irState = digitalRead(IR_PIN);
  bool rawOpen = (irState == DOOR_OPEN_READING);

  if (rawOpen != doorRaw) {
    doorRaw = rawOpen;
    doorLastChangeMs = now;
  }

  if ((now - doorLastChangeMs) >= DOOR_STABLE_MS) {
    doorStable = doorRaw;
  }

  // Only act on stable state
  bool shouldBuzz = doorStable;

  if (shouldBuzz != buzzerOn) {
    buzzerOn = shouldBuzz;
    digitalWrite(BUZZER_PIN, buzzerOn ? BUZZER_ON : BUZZER_OFF);
  }

  if (doorStable != lastDoorStable) {
    Serial.print("[DOOR] Door ");
    Serial.print(doorStable ? "OPEN" : "CLOSED");
    Serial.println(doorStable ? " -> ALARM ON" : " -> ALARM OFF");
    lastDoorStable = doorStable;
  }
}

// ================== Default targets by day ==================
int defaultRHForDay(int d) {
  if (d <= 7) return 56;
  else if (d <= 18) return 60;
  else return 65;
}

void applyAutoTargetsIfNeeded() {
  if (!tempManual) targetTemp = targetTempAuto;
  if (!rhManual)   targetRH = defaultRHForDay(day);
}

void updateVentInterval() {
  if (day <= 7)       ventInterval = 1800000UL; // 30 min
  else if (day <= 18) ventInterval = 1200000UL; // 20 min
  else                ventInterval = 900000UL;  // 15 min
}

// ================== Vent Trigger Logic ==================
bool shouldVentilate(float t, float h, VentReason &reasonOut) {
  bool timeTrigger = (now - lastVentTime) >= ventInterval;
  bool humidityTrigger = h > targetRH;

  const float VENT_TEMP_MARGIN = 0.1f; // per your request ✅
  bool tempTrigger = t >= (targetTemp + VENT_TEMP_MARGIN);

  if (tempTrigger)     { reasonOut = REASON_TEMP; return true; }
  if (humidityTrigger) { reasonOut = REASON_HUMIDITY; return true; }
  if (timeTrigger)     { reasonOut = REASON_TIME; return true; }

  reasonOut = REASON_NONE;
  return false;
}

const char* reasonName(VentReason r) {
  switch (r) {
    case REASON_TIME: return "TIME";
    case REASON_HUMIDITY: return "HUM";
    case REASON_TEMP: return "TEMP";
    default: return "NONE";
  }
}

// ================== DHT read (non-blocking cached) ==================
void readDhtNonBlocking() {
  if (now - lastDhtReadTime < DHT_READ_INTERVAL) return;
  lastDhtReadTime = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) tempC = t;
  if (!isnan(h)) rhPct = h;
}

// ================== Heating ==================
void setHeating(bool on) {
  heatingOn = on;
  digitalWrite(HEAT_RELAY_PIN, on ? HEAT_ON : HEAT_OFF);
  lastHeatToggleMs = now;
}

void updateHeating(float t) {
  if (now - lastHeatToggleMs < HEAT_MIN_TOGGLE_MS) return;

  float onTh  = targetTemp - HEAT_HYST_ON;
  float offTh = targetTemp + HEAT_HYST_OFF;

  if (!heatingOn && t <= onTh) setHeating(true);
  else if (heatingOn && t >= offTh) setHeating(false);
}

// ================== Humidity ==================
void setHumiditySystem(bool on) {
  humidityOn = on;
  digitalWrite(HUM_RELAY_PIN, on ? HUM_ON : HUM_OFF);
  digitalWrite(HUM_FAN_RELAY_PIN, on ? HUM_ON : HUM_OFF);
  lastHumToggleMs = now;
}

void updateHumidity(float rh) {
  if (ventState != VENT_IDLE) {
    humidityBlockedByVent = true;
    if (humidityOn) setHumiditySystem(false);
    return;
  }

  humidityBlockedByVent = false;
  if (now - lastHumToggleMs < HUM_MIN_TOGGLE_MS) return;

  float onThreshold  = (float)(targetRH - RH_ON_DELTA);
  float offThreshold = (float)(targetRH - RH_OFF_DELTA);

  if (!humidityOn && rh <= onThreshold) setHumiditySystem(true);
  else if (humidityOn && rh >= offThreshold) setHumiditySystem(false);
}

// ================== Flip Commands + Update ==================
void printFlipTiming() {
  //flipIntervalMs = (unsigned long)FLIP_INTERVAL_HOURS * 60UL * 60UL * 1000UL;
  flipIntervalMs =
  (unsigned long)FLIP_INTERVAL_HOURS * 60UL * 60UL * 1000UL + (unsigned long)FLIP_INTERVAL_MINUTES * 60UL * 1000UL;

  flipDurationMs = (unsigned long)FLIP_DURATION_MINUTES * 60UL * 1000UL;

  unsigned long sinceBoot = now - bootTimeMs;
  long bootRemain = (long)(FLIP_ARM_DELAY_MS - sinceBoot);

  Serial.println();
  Serial.println("=== FLIP TIMING ===");
  Serial.print("Armed: "); Serial.println(flipArmed ? "YES" : "NO");
  Serial.print("Auto: "); Serial.println(autoFlipEnabled ? "ON" : "OFF");

  if (bootRemain > 0) {
    Serial.print("Boot delay remaining: ");
    Serial.print(bootRemain / 1000L);
    Serial.println(" sec");
  } else {
    Serial.println("Boot delay: PASSED ✅");
  }

  if (!isFlippingSession) {
    unsigned long passed = now - lastFlipTime;
    unsigned long rem = (passed < flipIntervalMs) ? (flipIntervalMs - passed) : 0;
    Serial.print("Next scheduled flip in: ");
    Serial.print(rem / 1000UL);
    Serial.println(" sec");
  } else {
    unsigned long passed = now - flipStartTimeMs;
    unsigned long rem = (passed < flipDurationMs) ? (flipDurationMs - passed) : 0;
    Serial.print("Session remaining: ");
    Serial.print(rem / 1000UL);
    Serial.println(" sec");
    Serial.print("SafeStopRequested: ");
    Serial.println(flipSafeStopRequested ? "YES" : "NO");
  }
  Serial.println("===================");
  Serial.println();
}

void startFlipSession(bool manualStart) {
  isFlippingSession = true;
  flipStartTimeMs = now;
  cycleCount = 0;
  flipSafeStopRequested = false;

  flipState = FLIP_SESSION_START;
  flipStateStartMs = now;
  //lastFlipTime = now;

  Serial.println();
  Serial.println("╔═══════════════════════════════════════╗");
  Serial.println("║   🥚 بدء جلسة التقليب               ║");
  Serial.println("╚═══════════════════════════════════════╝");
  if (manualStart) Serial.println("[FLIP] Manual start (S).");
}

void endFlipSession(const char* why) {
  isFlippingSession = false;
  disableStepper();
  lastFlipTime = now;  // start rest timer from END of session

  Serial.println();
  Serial.println("╔═══════════════════════════════════════╗");
  Serial.println("║   ✓ انتهت جلسة التقليب               ║");
  Serial.print("║   السبب: "); Serial.print(why); Serial.println("         ║");
  Serial.print("║   عدد الدورات: "); Serial.print(cycleCount); Serial.println("          ║");
  Serial.println("╚═══════════════════════════════════════╝");
  Serial.println();
}

void handleSerialCommands() {
  while (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == '\n' || cmd == '\r') continue;
    while (Serial.available()) Serial.read();

    if (cmd == 'H' || cmd == 'h') {
      Serial.println();
      Serial.println("=== COMMANDS ===");
      Serial.println("H : help");
      Serial.println("R : ARM flip system (after boot delay)");
      Serial.println("U : DISARM flip system (no auto flip)");
      Serial.println("S : start flip session NOW (needs ARM)");
      Serial.println("X : SAFE STOP (finish returning, then stop)");
      Serial.println("A : toggle auto schedule (ON/OFF)");
      Serial.println("T : timing info");
      Serial.println("================");
      Serial.println();
    }
    else if (cmd == 'R' || cmd == 'r') {
      flipArmed = true;
      flipSafeStopRequested = false;

      // prevent immediate flip spike
      armTimeMs = now;
      firstFlipPendingAfterArm = true;
      lastFlipTime = now;  // reset schedule reference to now

      Serial.println("[FLIP] ARMED ✅ (first flip after 60s, then normal schedule)");
    }

    else if (cmd == 'U' || cmd == 'u') {
      flipArmed = false;
      flipSafeStopRequested = false;
      if (isFlippingSession) {
        flipSafeStopRequested = true;
        Serial.println("[FLIP] DISARM while flipping -> SAFE STOP will complete then disable.");
      } else {
        Serial.println("[FLIP] DISARMED ⛔");
      }
    }
    else if (cmd == 'A' || cmd == 'a') {
      autoFlipEnabled = !autoFlipEnabled;
      Serial.print("[FLIP] Auto schedule: ");
      Serial.println(autoFlipEnabled ? "ON" : "OFF");
    }
    else if (cmd == 'S' || cmd == 's') {
      bool bootDelayPassed = (now - bootTimeMs) >= FLIP_ARM_DELAY_MS;
      if (!flipArmed) { Serial.println("[FLIP] رفض: غير ARMED. اكتب R."); return; }
      if (!bootDelayPassed) { Serial.println("[FLIP] رفض: Boot Delay فعال. اكتب T."); return; }
      if (!isFlippingSession) startFlipSession(true);
      else Serial.println("[FLIP] جلسة شغالة بالفعل.");
    }
    else if (cmd == 'X' || cmd == 'x') {
      if (isFlippingSession) {
        flipSafeStopRequested = true;
        Serial.println("[FLIP] SAFE STOP requested ✅");
      } else {
        Serial.println("[FLIP] لا توجد جلسة تقليب.");
      }
    }
    else if (cmd == 'T' || cmd == 't') {
      printFlipTiming();
    }
    else {
      Serial.println("[CMD] Unknown. Press H.");
    }
  }
}

void updateFlipSystem() {
  //flipIntervalMs = (unsigned long)FLIP_INTERVAL_HOURS * 60UL * 60UL * 1000UL;
  flipIntervalMs =
  (unsigned long)FLIP_INTERVAL_HOURS * 60UL * 60UL * 1000UL +(unsigned long)FLIP_INTERVAL_MINUTES * 60UL * 1000UL;

  flipDurationMs = (unsigned long)FLIP_DURATION_MINUTES * 60UL * 1000UL;

  bool bootDelayPassed = (now - bootTimeMs) >= FLIP_ARM_DELAY_MS;

    if (!isFlippingSession) {
    if (flipArmed && bootDelayPassed && autoFlipEnabled) {

      // One-time delay after ARM (R)
      if (firstFlipPendingAfterArm) {
        if (now - armTimeMs >= FIRST_FLIP_AFTER_ARM_DELAY_MS) {
          firstFlipPendingAfterArm = false;
          startFlipSession(false);
        }
      } else {
        if (now - lastFlipTime >= flipIntervalMs) startFlipSession(false);
      }
    }
    return;
  }


  bool sessionTimeEnded = (now - flipStartTimeMs >= flipDurationMs);

  switch (flipState) {
    case FLIP_SESSION_START:
      enableStepper();
      if (now - flipStateStartMs >= 10) {
        stepsPerMove = computeStepsForAngle((float)TARGET_ANGLE);
        cycleCount++;
        if (cycleCount == 1) Serial.println("[FLIP] session running...");
        startMove(true);
        flipState = FLIP_MOVE_CW;
        flipStateStartMs = now;
      }
      break;

    case FLIP_MOVE_CW:
      if (runMoveNonBlocking()) {
        flipState = FLIP_PAUSE_1;
        flipStateStartMs = now;
      }
      break;

    case FLIP_PAUSE_1:
      if (now - flipStateStartMs >= FLIP_PAUSE_MS) {
        startMove(false);
        flipState = FLIP_MOVE_CCW;
        flipStateStartMs = now;
      }
      break;

    case FLIP_MOVE_CCW:
      if (runMoveNonBlocking()) {
        flipState = FLIP_PAUSE_2;
        flipStateStartMs = now;
      }
      break;

    case FLIP_PAUSE_2:
      if (now - flipStateStartMs >= FLIP_PAUSE_MS) {
        if (flipSafeStopRequested || sessionTimeEnded) {
          flipState = FLIP_SESSION_END;
          flipStateStartMs = now;
          break;
        }
        flipState = FLIP_SESSION_START;
        flipStateStartMs = now;
      }
      break;

    case FLIP_SESSION_END:
      endFlipSession(flipSafeStopRequested ? "SAFE STOP (X)" : "NORMAL (TIME)");
      flipSafeStopRequested = false;
      flipState = FLIP_IDLE;
      break;

    case FLIP_IDLE:
    default:
      isFlippingSession = false;
      disableStepper();
      break;
  }
}

// ================== UI (Keypad + LCD) ==================
enum UiMode { UI_MAIN, UI_SETTINGS };
UiMode uiMode = UI_MAIN;

int uiItem = 0;
char uiBuf[10];
byte uiLen = 0;
bool uiHasDecimal = false;

void uiClearBuffer() {
  uiLen = 0;
  uiBuf[0] = '\0';
  uiHasDecimal = false;
}

void uiDeleteChar() {
  if (uiLen == 0) return;
  if (uiBuf[uiLen - 1] == '.') uiHasDecimal = false;
  uiLen--;
  uiBuf[uiLen] = '\0';
}

bool uiAppendChar(char c) {
  if (uiLen >= sizeof(uiBuf) - 1) return false;
  uiBuf[uiLen++] = c;
  uiBuf[uiLen] = '\0';
  return true;
}

void uiEnterSettings() {
  uiMode = UI_SETTINGS;
  uiItem = 0;
  uiClearBuffer();
  Serial.println("[UI] Settings: 1(temp) 2(RH) 3(day), #=OK, C=Exit, D=Del, B=Reset");
}

void uiExitSettingsNoSave() {
  uiMode = UI_MAIN;
  uiItem = 0;
  uiClearBuffer();
  Serial.println("[UI] Exit Settings (no save)");
}

void uiResetDefaults() {
  tempManual = false;
  rhManual = false;
  targetTemp = targetTempAuto;
  targetRH = defaultRHForDay(day);
  Serial.println("[UI] Reset Defaults -> Temp AUTO, RH AUTO(day)");
}

float uiParseFloat() { return atof(uiBuf); }
int uiParseInt() { return atoi(uiBuf); }

void uiSaveCurrentItem() {
  if (uiItem == 1) {
    if (uiLen == 0) { Serial.println("[UI] Temp empty -> ignored"); return; }
    float v = uiParseFloat();
    // ✅ requested range 15..45
    if (v < 15.0f || v > 45.0f) { Serial.println("[UI] Temp out of range (15..45)"); return; }
    targetTemp = v;
    tempManual = true;
    Serial.print("[UI] Saved TargetTemp = ");
    Serial.println(targetTemp, 1);
  }
  else if (uiItem == 2) {
    if (uiLen == 0) { Serial.println("[UI] RH empty -> ignored"); return; }
    int v = uiParseInt();
    // ✅ requested range 30..80
    if (v < 30 || v > 80) { Serial.println("[UI] RH out of range (30..80)"); return; }
    targetRH = v;
    rhManual = true;
    Serial.print("[UI] Saved TargetRH = ");
    Serial.println(targetRH);
  }
  else if (uiItem == 3) {
    if (uiLen == 0) { Serial.println("[UI] Day empty -> ignored"); return; }
    int v = uiParseInt();
    if (v < 1 || v > 21) { Serial.println("[UI] Day out of range (1..21)"); return; }
    day = v;
    Serial.print("[UI] Saved Day = ");
    Serial.println(day);

    // update interval immediately
    updateVentInterval();

    // auto RH update only if not manual
    if (!rhManual) {
      targetRH = defaultRHForDay(day);
      Serial.print("[UI] RH auto-updated by day => ");
      Serial.println(targetRH);
    }
  }
}

void updateUI() {
  char k = keypad.getKey();
  if (!k) return;

  if (k == 'B') {
    uiResetDefaults();
    uiMode = UI_MAIN;
    uiItem = 0;
    uiClearBuffer();
    return;
  }

  if (uiMode == UI_MAIN) {
    if (k == 'A') uiEnterSettings();
    return;
  }

  // SETTINGS
  if (k == 'C') { uiExitSettingsNoSave(); return; }

  if (uiItem == 0) {
    if (k == '1' || k == '2' || k == '3') {
      uiItem = (k - '0');
      uiClearBuffer();
      Serial.print("[UI] Selected ");
      Serial.println(uiItem == 1 ? "TEMP" : (uiItem == 2 ? "RH" : "DAY"));
    }
    return;
  }

  if (k == 'D') { uiDeleteChar(); return; }

  if (k == '#') {
    uiSaveCurrentItem();
    uiItem = 0;
    uiClearBuffer();
    // stay in settings item select; user can press C to exit
    Serial.println("[UI] Saved. Pick 1/2/3 or C to exit.");
    return;
  }

  if (k >= '0' && k <= '9') { uiAppendChar(k); return; }

  if (k == '*') {
    if (uiItem == 1 && !uiHasDecimal) {
      uiHasDecimal = true;
      uiAppendChar('.');
    }
    return;
  }
}

// ================== LCD Helpers ==================
void lcdPrint16(const String &s) {
  String out = s;
  if (out.length() > LCD_COLS) out = out.substring(0, LCD_COLS);
  while (out.length() < LCD_COLS) out += ' ';
  lcd.print(out);
}

String twoDigits(int v) {
  if (v < 10) return "0" + String(v);
  return String(v);
}

String temp4() {
  // Always 4 chars for range 15.0..45.0: "37.6"
  if (isnan(tempC)) return "--.-";
  return String(tempC, 1);
}

String rh2() {
  if (isnan(rhPct)) return "--";
  int r = (int)rhPct;
  if (r < 0) r = 0;
  if (r > 99) r = 99;
  return twoDigits(r);
}

char vlvChar() { return valveOpen ? 'O' : 'C'; }

char vntChar() {
  // per your request:
  // O = servo open (VENT_OPEN + FAN_RUNNING + FAN_STOPPED)
  // C = servo closed (VENT_IDLE + VENT_CLOSE)
  switch (ventState) {
    case VENT_OPEN:
    case VENT_FAN_RUNNING:
    case VENT_FAN_STOPPED:
      return 'O';
    case VENT_IDLE:
    case VENT_CLOSE:
    default:
      return 'C';
  }
}

// line2 timing logic
void buildLine2(String &out) {
  char doorChar = doorStable ? 'O' : 'C';

  bool bootDelayPassed = (now - bootTimeMs) >= FLIP_ARM_DELAY_MS;
  //flipIntervalMs = (unsigned long)FLIP_INTERVAL_HOURS * 60UL * 60UL * 1000UL;
  flipIntervalMs =
  (unsigned long)FLIP_INTERVAL_HOURS * 60UL * 60UL * 1000UL + (unsigned long)FLIP_INTERVAL_MINUTES * 60UL * 1000UL;

  flipDurationMs = (unsigned long)FLIP_DURATION_MINUTES * 60UL * 1000UL;

  if (!bootDelayPassed) {
    // Boot countdown seconds
    unsigned long remMs = FLIP_ARM_DELAY_MS - (now - bootTimeMs);
    unsigned long remS = remMs / 1000UL;
    if (remS > 999) remS = 999;
    // "F:OFF B180s D:O"
    out = "F:OFF B";
    if (remS < 100) out += "0";
    if (remS < 10)  out += "0";
    out += String(remS);
    out += "s D:";
    out += doorChar;
    return;
  }

  // After ARM: show 60s countdown before first flip
  if (firstFlipPendingAfterArm) {
    unsigned long remMs = (now - armTimeMs >= FIRST_FLIP_AFTER_ARM_DELAY_MS)
                            ? 0
                            : (FIRST_FLIP_AFTER_ARM_DELAY_MS - (now - armTimeMs));
    unsigned long remS = remMs / 1000UL;
    if (remS > 999) remS = 999;

    out = "F:OFF R";
    if (remS < 100) out += "0";
    if (remS < 10)  out += "0";
    out += String(remS);
    out += "s D:";
    out += doorChar;
    return;
  }


  if (isFlippingSession) {
    // Session remaining minutes (ceil)
    unsigned long passed = now - flipStartTimeMs;
    unsigned long remMs = (passed < flipDurationMs) ? (flipDurationMs - passed) : 0;
    unsigned long remMin = (remMs + 59999UL) / 60000UL; // ceil minutes
    if (remMin > 999) remMin = 999;

    char st = flipStateChar(flipState);
    // "F:ON  C14m D:O"
    out = "F:ON  ";
    out += st;
    if (remMin < 100) out += "0";
    if (remMin < 10)  out += "0";
    out += String(remMin);
    out += "m D:";
    out += doorChar;
    return;
  }

  // Waiting for next session: show minutes remaining to next flip
  unsigned long passed = now - lastFlipTime;
  unsigned long remMs = (passed < flipIntervalMs) ? (flipIntervalMs - passed) : 0;
  unsigned long remMin = (remMs + 59999UL) / 60000UL; // ceil minutes
  if (remMin > 999) remMin = 999;

  // "F:OFF F120mD:O" (compact)
  out = "F:OFF F";
  if (remMin < 100) out += "0";
  if (remMin < 10)  out += "0";
  out += String(remMin);
  out += "m D:";
  out += doorChar;
}

void renderMainLCD() {
  // Line1 fixed 16 chars: "T:37.6H:58V:ON:C"
  String line1 = "T:" + temp4() + "H:" + rh2() + "V:" + String(vlvChar()) + "N:" + String(vntChar());

  String line2;
  buildLine2(line2);

  lcd.setCursor(0, 0); lcdPrint16(line1);
  lcd.setCursor(0, 1); lcdPrint16(line2);
}

void renderSettingsLCD() {
  // keep it short for 16x2
  String line1 = "SET 1T 2H 3D";
  String line2;

  if (uiItem == 0) line2 = "Pick1/2/3 CEx";
  else if (uiItem == 1) line2 = "T:" + String(uiBuf) + " #OK DDel";
  else if (uiItem == 2) line2 = "H:" + String(uiBuf) + " #OK DDel";
  else line2 = "D:" + String(uiBuf) + " #OK DDel";

  lcd.setCursor(0, 0); lcdPrint16(line1);
  lcd.setCursor(0, 1); lcdPrint16(line2);
}

void updateLCD() {
  if (now - lastLcdMs < LCD_REFRESH_MS) return;
  lastLcdMs = now;

  if (uiMode == UI_MAIN) renderMainLCD();
  else renderSettingsLCD();
}

// ================== Telemetry ==================
void printStatus(float t, float h) {
  unsigned long remainingVent =
    (now - lastVentTime >= ventInterval) ? 0 : (ventInterval - (now - lastVentTime)) / 1000UL;

  Serial.println("====================================");
  Serial.print("Day: "); Serial.println(day);

  Serial.print("Temp: ");
  if (isnan(t)) Serial.print("N/A"); else Serial.print(t, 1);
  Serial.print(" | Target: "); Serial.print(targetTemp, 1);
  Serial.print(tempManual ? " (MANUAL)" : " (AUTO)");
  Serial.println();

  Serial.print("Humidity: ");
  if (isnan(h)) Serial.print("N/A"); else Serial.print(h, 0);
  Serial.print(" | Target: "); Serial.print(targetRH);
  Serial.print(rhManual ? " (MANUAL)" : " (AUTO)");
  Serial.println();

  Serial.print("DoorStable: "); Serial.print(doorStable ? "OPEN" : "CLOSED");
  Serial.print(" | Buzzer: "); Serial.println(buzzerOn ? "ON" : "OFF");

  Serial.print("Valve: "); Serial.print(valveOpen ? "OPEN" : "CLOSED");
  Serial.print(" | WaterRaw: "); Serial.print(waterLowRaw ? "LOW" : "OK");
  Serial.print(" | WaterStable: "); Serial.println(waterLowStable ? "LOW" : "OK");

  Serial.print("VentState: "); Serial.print((int)ventState);
  Serial.print(" | Next vent in: "); Serial.print(remainingVent); Serial.println(" sec");

  Serial.print("Flip: "); Serial.print(isFlippingSession ? "ON" : "OFF");
  Serial.print(" | FlipStateChar: "); Serial.println(flipStateChar(flipState));

  Serial.println("UI: A=settings, 1/2/3 select, *=decimal(temp), #=save, C=exit, D=del, B=reset");
  Serial.println("====================================");
}

// ================== Setup ==================
void setup() {
  Serial.begin(9600);
  dht.begin();

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();

  // Servo
  ventServo.attach(SERVO_PIN);

  pinMode(VENT_RELAY_PIN, OUTPUT);
  pinMode(HEAT_RELAY_PIN, OUTPUT);
  pinMode(HUM_RELAY_PIN, OUTPUT);
  pinMode(HUM_FAN_RELAY_PIN, OUTPUT);

  // Stepper
  pinMode(DIR_PIN, OUTPUT);
  pinMode(STEP_PIN, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);

  // Water
  pinMode(waterLevelPin, INPUT_PULLUP);
  pinMode(valveRelayPin, OUTPUT);

  // Door
  pinMode(IR_PIN, INPUT);       // keep as your working setup
  pinMode(BUZZER_PIN, OUTPUT);

  // SAFE BOOT outputs
  ventState = VENT_IDLE;
  lastVentReason = REASON_NONE;

  digitalWrite(VENT_RELAY_PIN, FAN_OFF);
  ventServo.write(SERVO_CLOSED);

  digitalWrite(HEAT_RELAY_PIN, HEAT_OFF);
  digitalWrite(HUM_RELAY_PIN, HUM_OFF);
  digitalWrite(HUM_FAN_RELAY_PIN, HUM_OFF);

  digitalWrite(STEP_PIN, LOW);
  disableStepper();

  setValve(false);

  digitalWrite(BUZZER_PIN, BUZZER_OFF);

  now = millis();
  bootTimeMs = now;

  lastVentTime = now;
  ventStateStartMs = now;

  lastDhtReadTime = 0;
  lastPrintTime = 0;
  lastLcdMs = 0;

  // Flip
  lastFlipTime = now;
  flipArmed = false;
  flipSafeStopRequested = false;

  armTimeMs = 0;
firstFlipPendingAfterArm = false;


  // relay protection
  lastHeatToggleMs = now;
  lastHumToggleMs  = now;

  // water filter init
  waterLowRaw = (digitalRead(waterLevelPin) == WATER_LOW_READING);
  waterLowStable = waterLowRaw;
  waterLastChangeMs = now;

  // door debounce init
  doorRaw = (digitalRead(IR_PIN) == DOOR_OPEN_READING);
  doorStable = doorRaw;
  lastDoorStable = doorStable;
  doorLastChangeMs = now;
  buzzerOn = doorStable;
  digitalWrite(BUZZER_PIN, buzzerOn ? BUZZER_ON : BUZZER_OFF);

  // apply initial auto targets
  applyAutoTargetsIfNeeded();
  updateVentInterval();

  Serial.println("BOOT: Incubator (VENT + HEAT + HUM + FLIP + WATER + DOOR_ALARM + LCD + KEYPAD) running...");
  Serial.println("FLIP: DISARMED by default. Press R to ARM. BootDelay=3min.");
  Serial.println("LCD: 16x2 fixed. Line1=T/H/V/N  Line2=Flip timing + Door");
  Serial.println("---------------------------------------");
}

// ================== Loop ==================
void loop() {
  now = millis();

  // serial flip commands
  handleSerialCommands();

  // door alarm (debounced)
  updateDoorAlarmDebounced();

  // UI
  updateUI();

  // auto targets if not manual
  applyAutoTargetsIfNeeded();
  updateVentInterval();

  // sensors
  readDhtNonBlocking();

  // heating/humidity
  if (!isnan(tempC)) updateHeating(tempC);
  if (!isnan(rhPct)) updateHumidity(rhPct);

  // ventilation SM
  switch (ventState) {
    case VENT_IDLE:
      if (!isnan(tempC) && !isnan(rhPct)) {
        VentReason reason;
        if (shouldVentilate(tempC, rhPct, reason)) {
          lastVentReason = reason;
          ventState = VENT_OPEN;
          ventStateStartMs = now;
          Serial.print("[VENT] Triggered by: ");
          Serial.println(reasonName(reason));
        }
      }
      break;

    case VENT_OPEN:
      ventServo.write(SERVO_OPEN);
      if (now - ventStateStartMs >= servoMoveTime) {
        ventState = VENT_FAN_RUNNING;
        ventStateStartMs = now;
        digitalWrite(VENT_RELAY_PIN, FAN_ON);
      }
      break;

    case VENT_FAN_RUNNING:
      if (now - ventStateStartMs >= fanRunTime) {
        digitalWrite(VENT_RELAY_PIN, FAN_OFF);
        ventState = VENT_FAN_STOPPED;
        ventStateStartMs = now;
      }
      break;

    case VENT_FAN_STOPPED:
      if (now - ventStateStartMs >= pauseAfterFan) {
        ventState = VENT_CLOSE;
        ventStateStartMs = now;
      }
      break;

    case VENT_CLOSE:
      ventServo.write(SERVO_CLOSED);
      if (now - ventStateStartMs >= servoMoveTime) {
        ventState = VENT_IDLE;
        lastVentTime = now;
        lastVentReason = REASON_NONE;
      }
      break;
  }

  // flip
  updateFlipSystem();

  // water
  updateWaterSystem();

  // LCD
  updateLCD();

  // telemetry
  unsigned long printInterval = isFlippingSession ? PRINT_INTERVAL_FLIP : PRINT_INTERVAL_IDLE;
  if (now - lastPrintTime >= printInterval) {
    lastPrintTime = now;
    printStatus(tempC, rhPct);
  }
}
