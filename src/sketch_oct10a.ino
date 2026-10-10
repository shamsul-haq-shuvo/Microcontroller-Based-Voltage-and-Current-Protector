
#include <LiquidCrystal.h>
#include <Keypad.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ---------------- PIN CONFIGURATION ----------------

const byte VOLTAGE_PIN = A0;
const byte CURRENT_PIN = A1;
const byte RELAY_PIN = 9;

const byte MANUAL_OFF_BUTTON = 10;
const byte MANUAL_ON_BUTTON = 11;

// LCD: RS, E, D4, D5, D6, D7
LiquidCrystal lcd(13, 12, A5, A4, A3, A2);

// ---------------- KEYPAD CONFIGURATION ----------------

const byte ROWS = 4;
const byte COLS = 3;

char keymap[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

// Rows A, B, C, D; columns 1, 2, 3
byte rowPins[ROWS] = {2, 3, 4, 5};
byte colPins[COLS] = {6, 7, 8};

Keypad keypad = Keypad(
  makeKeymap(keymap),
  rowPins,
  colPins,
  ROWS,
  COLS
);

// ---------------- SENSOR CONFIGURATION ----------------

const float ADC_REFERENCE = 5.0;
const float ADC_MAX = 1023.0;

// Calibration: 190 V AC -> 2.90 V at A0
//              250 V AC -> 3.90 V at A0

const float CAL_VOLTAGE_LOW = 190.0;
const float CAL_SENSOR_LOW = 2.90;
const float CAL_VOLTAGE_HIGH = 250.0;
const float CAL_SENSOR_HIGH = 3.90;

const float ACS712_SENSITIVITY = 0.185;

const unsigned long RMS_WINDOW_MS = 20;
const unsigned long DEBOUNCE_MS = 35;
const unsigned long STARTUP_CALIBRATION_MS = 3000;

// ---------------- PROTECTION THRESHOLDS ----------------

float undervoltageLimit = 190.0;
float overvoltageLimit = 250.0;
float currentLimit = 1.65;

// ---------------- MEASURED VALUES ----------------

float currentZeroOffset = 2.502;
float sensorVoltage = 0.0;
float measuredVoltage = 0.0;
float measuredCurrent = 0.0;

// ---------------- CONTROL STATES ----------------

// D9 HIGH = load ON
// D9 LOW  = load OFF

bool loadConnected = false;
bool faultLatched = false;
bool manualOff = false;
bool manualOnRequested = false;

bool activeFaultNow = false;
bool previousActiveFault = false;
bool protectionReady = false;

// ---------------- BUTTON STATES ----------------

// Active LOW: released = HIGH, pressed = LOW

bool lastD10Reading = HIGH;
bool stableD10State = HIGH;
unsigned long lastD10Change = 0;

bool lastD11Reading = HIGH;
bool stableD11State = HIGH;
unsigned long lastD11Change = 0;

// ---------------- LCD STATE ----------------

unsigned long lastDisplayRefresh = 0;
const unsigned long DISPLAY_REFRESH_MS = 200;
const unsigned long PAGE_DURATION_MS = 2500;

bool displayMessageActive = false;
unsigned long displayMessageUntil = 0;

char displayMessageLine1[17] = "";
char displayMessageLine2[17] = "";

// ---------------- SERIAL INPUT ----------------

char commandBuffer[40];
byte commandIndex = 0;

// --------------------------------------------------
// RELAY CONTROL
// --------------------------------------------------

void setLoad(bool connected) {
  digitalWrite(RELAY_PIN, connected ? HIGH : LOW);
  loadConnected = connected;
}

// --------------------------------------------------
// VOLTAGE SENSOR
// --------------------------------------------------

float readVoltageSensor() {
  long total = 0;

  for (int i = 0; i < 10; i++) {
    total += analogRead(VOLTAGE_PIN);
  }

  float adcValue = total / 10.0;

  return adcValue * ADC_REFERENCE / ADC_MAX;
}

float convertSensorToACVoltage(float voltage) {
  return CAL_VOLTAGE_LOW +
         (voltage - CAL_SENSOR_LOW) *
         (CAL_VOLTAGE_HIGH - CAL_VOLTAGE_LOW) /
         (CAL_SENSOR_HIGH - CAL_SENSOR_LOW);
}

// --------------------------------------------------
// CURRENT SENSOR
// --------------------------------------------------

float readCurrentRMS() {
  unsigned long startTime = millis();
  double sumSquares = 0.0;
  unsigned long sampleCount = 0;

  while (millis() - startTime < RMS_WINDOW_MS) {
    float adcValue = analogRead(CURRENT_PIN);
    float outputVoltage = adcValue * ADC_REFERENCE / ADC_MAX;

    float current =
        (outputVoltage - currentZeroOffset) /
        ACS712_SENSITIVITY;

    sumSquares += (double)current * current;
    sampleCount++;
  }

  if (sampleCount == 0) {
    return 0.0;
  }

  return sqrt(sumSquares / sampleCount);
}

// --------------------------------------------------
// LCD HELPERS
// --------------------------------------------------

void printLcdLine(byte row, const char *text) {
  lcd.setCursor(0, row);

  byte count = 0;

  while (*text && count < 16) {
    lcd.print(*text++);
    count++;
  }

  while (count < 16) {
    lcd.print(' ');
    count++;
  }
}

void showMessage(const char *line1, const char *line2) {
  strncpy(displayMessageLine1, line1, 16);
  displayMessageLine1[16] = '\0';

  strncpy(displayMessageLine2, line2, 16);
  displayMessageLine2[16] = '\0';

  displayMessageActive = true;
  displayMessageUntil = millis() + 1800;

  printLcdLine(0, displayMessageLine1);
  printLcdLine(1, displayMessageLine2);
}

void showVoltageThreshold(
    const char *title,
    const char *label,
    float value) {

  char valueText[8];
  char line2[17];

  dtostrf(value, 3, 0, valueText);
  snprintf(line2, sizeof(line2), "%s:%sV", label, valueText);

  showMessage(title, line2);
}

void showCurrentThreshold(const char *title) {
  char valueText[8];
  char line2[17];

  dtostrf(currentLimit, 4, 2, valueText);
  snprintf(line2, sizeof(line2), "I-LIMIT:%sA", valueText);

  showMessage(title, line2);
}

// --------------------------------------------------
// LCD STATUS DISPLAY
// --------------------------------------------------

void updateDisplay() {
  unsigned long now = millis();

  if (displayMessageActive) {
    if ((long)(now - displayMessageUntil) < 0) {
      return;
    }

    displayMessageActive = false;
  }

  if (now - lastDisplayRefresh < DISPLAY_REFRESH_MS) {
    return;
  }

  lastDisplayRefresh = now;

  byte page = (now / PAGE_DURATION_MS) % 2;

  char voltageText[8];
  char currentText[8];
  char uvText[8];
  char ovText[8];
  char limitText[8];
  char line1[17];
  char line2[17];

  if (!protectionReady) {
    strcpy(line1, "STARTUP DELAY");
    strcpy(line2, "Calibrating...");
  }
  else if (page == 0) {
    dtostrf(measuredVoltage, 5, 1, voltageText);
    dtostrf(measuredCurrent, 4, 2, currentText);

    snprintf(line1, sizeof(line1),
             "V:%s I:%sA", voltageText, currentText);

    if (activeFaultNow) {
      strcpy(line2, "LOAD:OFF FAULT");
    }
    else if (faultLatched) {
      strcpy(line2, "LOAD:OFF LOCKOUT");
    }
    else if (manualOff) {
      strcpy(line2, "LOAD:OFF MANUAL");
    }
    else {
      strcpy(line2, "LOAD:ON SAFE");
    }
  }
  else {
    dtostrf(undervoltageLimit, 3, 0, uvText);
    dtostrf(overvoltageLimit, 3, 0, ovText);
    dtostrf(currentLimit, 4, 2, limitText);

    snprintf(line1, sizeof(line1),
             "UV:%sV OV:%sV", uvText, ovText);

    snprintf(line2, sizeof(line2),
             "IL:%sA F:%s",
             limitText,
             (activeFaultNow || faultLatched) ? "ON" : "OFF");
  }

  printLcdLine(0, line1);
  printLcdLine(1, line2);
}

// --------------------------------------------------
// KEYPAD THRESHOLD ADJUSTMENT
// --------------------------------------------------

void handleKeypad() {
  char key = keypad.getKey();

  if (key == NO_KEY) {
    return;
  }

  float candidate;

  // 1: Increase undervoltage threshold by 5 V
  if (key == '1') {
    candidate = undervoltageLimit + 5.0;

    if (candidate < overvoltageLimit) {
      undervoltageLimit = candidate;
      showVoltageThreshold("UV INCREASED", "UV", undervoltageLimit);

      Serial.print(F("UV set to "));
      Serial.println(undervoltageLimit, 0);
    } else {
      showMessage("UV NOT CHANGED", "Must be below OV");
    }
  }

  // 7: Decrease undervoltage threshold by 5 V
  else if (key == '7') {
    candidate = undervoltageLimit - 5.0;

    if (candidate >= 100.0) {
      undervoltageLimit = candidate;
      showVoltageThreshold("UV DECREASED", "UV", undervoltageLimit);

      Serial.print(F("UV set to "));
      Serial.println(undervoltageLimit, 0);
    } else {
      showMessage("UV MINIMUM", "100 V");
    }
  }

  // 2: Increase overvoltage threshold by 5 V
  else if (key == '2') {
    candidate = overvoltageLimit + 5.0;

    if (candidate <= 300.0) {
      overvoltageLimit = candidate;
      showVoltageThreshold("OV INCREASED", "OV", overvoltageLimit);

      Serial.print(F("OV set to "));
      Serial.println(overvoltageLimit, 0);
    } else {
      showMessage("OV MAXIMUM", "300 V");
    }
  }

  // 8: Decrease overvoltage threshold by 5 V
  else if (key == '8') {
    candidate = overvoltageLimit - 5.0;

    if (candidate > undervoltageLimit) {
      overvoltageLimit = candidate;
      showVoltageThreshold("OV DECREASED", "OV", overvoltageLimit);

      Serial.print(F("OV set to "));
      Serial.println(overvoltageLimit, 0);
    } else {
      showMessage("OV NOT CHANGED", "Must be above UV");
    }
  }

  // 3: Increase current threshold by 0.05 A
  else if (key == '3') {
    candidate = round((currentLimit + 0.05) * 100.0) / 100.0;

    if (candidate <= 5.00) {
      currentLimit = candidate;
      showCurrentThreshold("CURRENT INCREASED");

      Serial.print(F("Current limit set to "));
      Serial.println(currentLimit, 2);
    } else {
      showMessage("CURRENT MAXIMUM", "5.00 A");
    }
  }

  // 9: Decrease current threshold by 0.05 A
  else if (key == '9') {
    candidate = round((currentLimit - 0.05) * 100.0) / 100.0;

    if (candidate >= 0.10) {
      currentLimit = candidate;
      showCurrentThreshold("CURRENT DECREASED");

      Serial.print(F("Current limit set to "));
      Serial.println(currentLimit, 2);
    } else {
      showMessage("CURRENT MINIMUM", "0.10 A");
    }
  }
}

// --------------------------------------------------
// BUTTON DEBOUNCING
// --------------------------------------------------

bool buttonPressed(
    byte pin,
    bool &lastReading,
    bool &stableState,
    unsigned long &lastChangeTime) {

  bool reading = digitalRead(pin);

  if (reading != lastReading) {
    lastChangeTime = millis();
    lastReading = reading;
  }

  if (millis() - lastChangeTime >= DEBOUNCE_MS) {
    if (reading != stableState) {
      stableState = reading;

      if (stableState == LOW) {
        return true;
      }
    }
  }

  return false;
}

// --------------------------------------------------
// SERIAL STATUS AND HELP
// --------------------------------------------------

void printStatus() {
  Serial.println();
  Serial.println(F("===== VOLTAGE PROTECTOR STATUS ====="));

  Serial.print(F("Voltage: "));
  Serial.print(measuredVoltage, 1);
  Serial.println(F(" V"));

  Serial.print(F("Current: "));
  Serial.print(measuredCurrent, 3);
  Serial.println(F(" A RMS"));

  Serial.print(F("UV limit: "));
  Serial.println(undervoltageLimit, 1);

  Serial.print(F("OV limit: "));
  Serial.println(overvoltageLimit, 1);

  Serial.print(F("Current limit: "));
  Serial.println(currentLimit, 2);

  Serial.print(F("Load: "));
  Serial.println(loadConnected ? F("ON") : F("OFF"));

  Serial.print(F("Fault lockout: "));
  Serial.println(faultLatched ? F("ACTIVE") : F("CLEAR"));

  Serial.print(F("Manual OFF: "));
  Serial.println(manualOff ? F("ACTIVE") : F("INACTIVE"));

  Serial.println(F("===================================="));
}

void printHelp() {
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("ON"));
  Serial.println(F("OFF"));
  Serial.println(F("STATUS"));
  Serial.println(F("HELP"));
  Serial.println(F("SET_UV 190"));
  Serial.println(F("SET_OV 250"));
  Serial.println(F("SET_I 1.60"));
}

// --------------------------------------------------
// NUMBER PARSER
// --------------------------------------------------

bool parseNumber(char *text, float *result) {
  char *p = text;
  float value = 0.0;
  float sign = 1.0;
  bool digitFound = false;

  while (*p == ' ') {
    p++;
  }

  if (*p == '-') {
    sign = -1.0;
    p++;
  } else if (*p == '+') {
    p++;
  }

  while (isdigit((unsigned char)*p)) {
    digitFound = true;
    value = value * 10.0 + (*p - '0');
    p++;
  }

  if (*p == '.') {
    p++;

    float place = 0.1;

    while (isdigit((unsigned char)*p)) {
      digitFound = true;
      value += (*p - '0') * place;
      place *= 0.1;
      p++;
    }
  }

  while (*p == ' ') {
    p++;
  }

  if (!digitFound || *p != '\0' || !isfinite(value)) {
    return false;
  }

  *result = value * sign;
  return true;
}

// --------------------------------------------------
// SERIAL COMMAND PROCESSING
// --------------------------------------------------

void processCommand(char *command) {
  while (*command == ' ') {
    command++;
  }

  for (char *p = command; *p; p++) {
    *p = toupper((unsigned char)*p);
  }

  if (strcmp(command, "ON") == 0) {
    manualOnRequested = true;
    Serial.println(F("ON request received."));
    return;
  }

  if (strcmp(command, "OFF") == 0) {
    manualOff = true;
    manualOnRequested = false;
    setLoad(false);

    Serial.println(F("MANUAL OVERRIDE: LOAD OFF"));
    showMessage("MANUAL OVERRIDE", "LOAD OFF");
    return;
  }

  if (strcmp(command, "STATUS") == 0) {
    printStatus();
    return;
  }

  if (strcmp(command, "HELP") == 0) {
    printHelp();
    return;
  }

  char *separator = strchr(command, ' ');

  if (separator == NULL) {
    Serial.println(F("Unknown command. Type HELP."));
    return;
  }

  *separator = '\0';
  char *valueText = separator + 1;
  float value;

  if (!parseNumber(valueText, &value)) {
    Serial.println(F("Invalid number."));
    return;
  }

  if (strcmp(command, "SET_UV") == 0) {
    if (value < 100.0 || value >= overvoltageLimit) {
      Serial.println(F("Rejected UV value."));
      return;
    }

    undervoltageLimit = value;

    Serial.print(F("UV limit set to "));
    Serial.println(undervoltageLimit, 1);

    showVoltageThreshold("UV SET", "UV", undervoltageLimit);
  }
  else if (strcmp(command, "SET_OV") == 0) {
    if (value > 300.0 || value <= undervoltageLimit) {
      Serial.println(F("Rejected OV value."));
      return;
    }

    overvoltageLimit = value;

    Serial.print(F("OV limit set to "));
    Serial.println(overvoltageLimit, 1);

    showVoltageThreshold("OV SET", "OV", overvoltageLimit);
  }
  else if (strcmp(command, "SET_I") == 0) {
    if (value < 0.10 || value > 5.00) {
      Serial.println(F("Rejected current limit."));
      return;
    }

    currentLimit = value;

    Serial.print(F("Current limit set to "));
    Serial.println(currentLimit, 2);

    showCurrentThreshold("CURRENT SET");
  }
  else {
    Serial.println(F("Unknown command. Type HELP."));
  }
}

void readSerialCommands() {
  while (Serial.available() > 0) {
    char incoming = Serial.read();

    if (incoming == '\r' || incoming == '\n') {
      if (commandIndex > 0) {
        commandBuffer[commandIndex] = '\0';
        processCommand(commandBuffer);
        commandIndex = 0;
      }
    }
    else if (commandIndex < sizeof(commandBuffer) - 1) {
      commandBuffer[commandIndex++] = incoming;
    }
    else {
      commandIndex = 0;
      Serial.println(F("Command too long."));
    }
  }
}

// --------------------------------------------------
// STARTUP CALIBRATION
// --------------------------------------------------

void calibrateCurrentSensor() {
  setLoad(false);

  printLcdLine(0, "STARTUP DELAY");
  printLcdLine(1, "Wait 3 seconds");

  Serial.println(F("Startup delay: 3 seconds."));
  Serial.println(F("Load remains OFF during calibration."));

  unsigned long startTime = millis();

  while (millis() - startTime < STARTUP_CALIBRATION_MS) {
    setLoad(false);
    readSerialCommands();
    delay(5);
  }

  double offsetSum = 0.0;
  const int calibrationSamples = 500;

  for (int i = 0; i < calibrationSamples; i++) {
    offsetSum +=
        analogRead(CURRENT_PIN) * ADC_REFERENCE / ADC_MAX;

    delay(2);
  }

  currentZeroOffset = offsetSum / calibrationSamples;

  Serial.print(F("Current sensor zero offset: "));
  Serial.print(currentZeroOffset, 3);
  Serial.println(F(" V"));

  Serial.println(F("Current sensor calibration complete."));
}

// --------------------------------------------------
// SETUP
// --------------------------------------------------

void setup() {
  digitalWrite(RELAY_PIN, LOW);
  pinMode(RELAY_PIN, OUTPUT);
  setLoad(false);

  pinMode(MANUAL_OFF_BUTTON, INPUT_PULLUP);
  pinMode(MANUAL_ON_BUTTON, INPUT_PULLUP);

  lcd.begin(16, 2);
  lcd.clear();

  Serial.begin(9600);

  Serial.println();
  Serial.println(F("MICROCONTROLLER VOLTAGE PROTECTOR"));

  protectionReady = false;
  calibrateCurrentSensor();

  // Initial measurements with the load disconnected.
  sensorVoltage = readVoltageSensor();
  measuredVoltage = convertSensorToACVoltage(sensorVoltage);
  measuredCurrent = readCurrentRMS();

  bool voltageFault =
      measuredVoltage < undervoltageLimit ||
      measuredVoltage > overvoltageLimit;

  bool currentFault = measuredCurrent > currentLimit;

  if (voltageFault || currentFault) {
    faultLatched = true;

    Serial.println(F("Startup fault detected. Load remains OFF."));

    if (voltageFault) {
      Serial.println(F("Check input voltage."));
    }

    if (currentFault) {
      Serial.println(F("Check current sensor offset and wiring."));
    }
  }

  protectionReady = true;

  printLcdLine(0, "System Ready");
  printLcdLine(1, "Checking limits");

  Serial.println(F("System ready."));
  printHelp();
}

// --------------------------------------------------
// MAIN LOOP
// --------------------------------------------------

void loop() {
  readSerialCommands();

  bool manualOffPressed = buttonPressed(
      MANUAL_OFF_BUTTON,
      lastD10Reading,
      stableD10State,
      lastD10Change
  );

  bool manualOnPressed = buttonPressed(
      MANUAL_ON_BUTTON,
      lastD11Reading,
      stableD11State,
      lastD11Change
  );

  if (manualOffPressed) {
    manualOff = true;
    manualOnRequested = false;
    setLoad(false);

    Serial.println(F("D10 PRESSED: LOAD OFF"));
    showMessage("MANUAL OVERRIDE", "LOAD OFF");
  }

  handleKeypad();

  sensorVoltage = readVoltageSensor();
  measuredVoltage = convertSensorToACVoltage(sensorVoltage);
  measuredCurrent = readCurrentRMS();

  bool undervoltageFault =
      measuredVoltage < undervoltageLimit;

  bool overvoltageFault =
      measuredVoltage > overvoltageLimit;

  bool currentFault =
      measuredCurrent > currentLimit;

  activeFaultNow =
      undervoltageFault ||
      overvoltageFault ||
      currentFault;

  if (activeFaultNow) {
    if (!previousActiveFault) {
      Serial.println();
      Serial.println(F("PROTECTION FAULT DETECTED!"));

      if (undervoltageFault) {
        Serial.println(F("FAULT: UNDERVOLTAGE"));
      }

      if (overvoltageFault) {
        Serial.println(F("FAULT: OVERVOLTAGE"));
      }

      if (currentFault) {
        Serial.println(F("FAULT: OVERCURRENT"));
      }

      Serial.println(F("LOAD DISCONNECTED."));
    }

    faultLatched = true;
    setLoad(false);
  }

  previousActiveFault = activeFaultNow;

  // D11 or ON acknowledges the fault and requests reconnection.
  // The request is accepted only after all fault conditions clear.
  bool onRequest =
      manualOnRequested ||
      (manualOnPressed && !manualOffPressed);

  if (onRequest) {
    if (!activeFaultNow && !manualOffPressed) {
      faultLatched = false;
      manualOff = false;

      Serial.println(F("ON REQUEST ACCEPTED."));
      Serial.println(F("Fault lockout cleared; load enabled."));
      showMessage("FAULT CLEARED", "LOAD TURNED ON");
    }
    else {
      Serial.println(F("ON REJECTED: FAULT STILL ACTIVE."));
      showMessage("ON REJECTED", "FAULT ACTIVE");
    }

    manualOnRequested = false;
  }

  // Final relay decision
  if (activeFaultNow || faultLatched || manualOff) {
    setLoad(false);
  }
  else {
    setLoad(true);
  }

  updateDisplay();

  delay(5);
}
