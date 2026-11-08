#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <LiquidCrystal_SR.h>
#include "secrets.h"

#define DHTPIN        19
#define LDR_PIN       35
#define TRIG_PIN      25
#define ECHO_PIN      26
#define MUX_OUT       36
#define MUX_A         4
#define MUX_B         2
#define MUX_C         5
#define WATER_FLOW    23
#define MAIN_PUMP_PWM 33
#define OXY_PUMP      27
#define NUTRI_PUMP    12
#define SOLENOID_SRC  14
#define SOLENOID_NUT  13
#define RELAY_AC      3
#define BUZZER        15
#define GREEN_LED     18
#define L293D_EN      22

#define CH_PH     0
#define CH_TDS    1
#define CH_MOIST1 2
#define CH_MOIST2 4
#define CH_MOIST3 5
#define CH_MOIST4 6
#define CH_POT    7

float waterDistanceThreshold = 10.0;
float pH_low = 5.5, pH_high = 6.5;
int tds_max = 1400;
int tds_min = 800;

const float AC_ON_TEMP = 28.0;
const float AC_OFF_TEMP = 24.0;
const int LIGHT_OFF_BELOW = 500;
const float LEVEL_HYST_CM = 1.0;
const int MAIN_PUMP_DUTY = 200;

const unsigned long SAMPLE_MS = 1000;
const unsigned long DHT_MS = 2000;
const unsigned long LCD_MS = 500;
const unsigned long PAGE_MS = 3000;
const unsigned long DOSE_RUN_MS = 5000;
const unsigned long DOSE_WAIT_MS = 120000;

const float FLOW_HZ_PER_LPM = 7.5;
const float ADC_DIVIDER = 1.0;
const bool POT_SETS_LEVEL = true;

int PH_RAW_AT_4 = 0;
int PH_RAW_AT_7 = 0;
int MOIST_DRY_RAW = 3300;
int MOIST_WET_RAW = 1300;

DHT dht(DHTPIN, DHT11);
LiquidCrystal_SR lcd(17, 16, 21);
WebServer server(80);

volatile unsigned int flowPulses = 0;

float temperature = 0, humidity = 0;
int ldrVal = 0;
float distanceCm = 0;
bool distanceOk = false;
float phValue = 7.0, tdsValue = 0, flowRate = 0;
int moisture[4] = {0, 0, 0, 0};

bool lightsOff = false;
bool acOn = false;
bool refilling = false;
bool alarmOn = false;
bool dosing = false;
bool everDosed = false;
unsigned long doseStart = 0, doseEnd = 0;

unsigned long lastSample = 0, lastDht = 0, lastLcd = 0, lastFlowCalc = 0, lastPrint = 0;

void IRAM_ATTR pulseCounter() {
  flowPulses++;
}

void setMainPump(int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(MAIN_PUMP_PWM, duty);
#else
  ledcWrite(0, duty);
#endif
}

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 0;
  return duration * 0.0343 / 2;
}

int readMux(int channel) {
  digitalWrite(MUX_A, channel & 1);
  digitalWrite(MUX_B, (channel >> 1) & 1);
  digitalWrite(MUX_C, (channel >> 2) & 1);
  delayMicroseconds(20);
  return analogRead(MUX_OUT);
}

int readMuxAvg(int channel, int samples = 8) {
  long total = 0;
  for (int i = 0; i < samples; i++) total += readMux(channel);
  return total / samples;
}

float rawToVolts(int raw) {
  return raw / 4095.0 * 3.3 / ADC_DIVIDER;
}

float rawToPh(int raw) {
  if (PH_RAW_AT_4 != 0 && PH_RAW_AT_7 != 0 && PH_RAW_AT_4 != PH_RAW_AT_7) {
    float slope = (7.0 - 4.0) / (float)(PH_RAW_AT_7 - PH_RAW_AT_4);
    return 7.0 + slope * (raw - PH_RAW_AT_7);
  }
  return raw * 5.0 / 4095.0 * 3.5;
}

float rawToTds(int raw, float tempC) {
  float v = rawToVolts(raw);
  float comp = 1.0 + 0.02 * (tempC - 25.0);
  float vc = v / comp;
  float tds = (133.42 * vc * vc * vc - 255.86 * vc * vc + 857.39 * vc) * 0.5;
  return tds < 0 ? 0 : tds;
}

int rawToMoisture(int raw) {
  int pct = map(raw, MOIST_DRY_RAW, MOIST_WET_RAW, 0, 100);
  return constrain(pct, 0, 100);
}

void updateFlow() {
  unsigned long now = millis();
  unsigned long elapsed = now - lastFlowCalc;
  if (elapsed == 0) return;
  noInterrupts();
  unsigned int count = flowPulses;
  flowPulses = 0;
  interrupts();
  float hz = count * 1000.0 / elapsed;
  flowRate = hz / FLOW_HZ_PER_LPM;
  lastFlowCalc = now;
}

void sampleSensors() {
  unsigned long now = millis();

  if (now - lastDht >= DHT_MS) {
    lastDht = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temperature = t;
    if (!isnan(h)) humidity = h;
  }

  ldrVal = analogRead(LDR_PIN);

  float d = getDistance();
  distanceOk = d > 0;
  if (distanceOk) distanceCm = d;

  phValue = rawToPh(readMuxAvg(CH_PH));
  tdsValue = rawToTds(readMuxAvg(CH_TDS), temperature);

  moisture[0] = rawToMoisture(readMuxAvg(CH_MOIST1));
  moisture[1] = rawToMoisture(readMuxAvg(CH_MOIST2));
  moisture[2] = rawToMoisture(readMuxAvg(CH_MOIST3));
  moisture[3] = rawToMoisture(readMuxAvg(CH_MOIST4));

  if (POT_SETS_LEVEL) {
    int p = readMuxAvg(CH_POT);
    waterDistanceThreshold = 5.0 + (p / 4095.0) * 25.0;
  }

  updateFlow();
}

void controlClimate() {
  if (temperature > AC_ON_TEMP) acOn = true;
  else if (temperature < AC_OFF_TEMP) acOn = false;
  digitalWrite(RELAY_AC, acOn ? HIGH : LOW);

  lightsOff = ldrVal < LIGHT_OFF_BELOW;
}

void controlWaterLevel() {
  if (!distanceOk) {
    refilling = false;
  } else if (!refilling && distanceCm > waterDistanceThreshold) {
    refilling = true;
  } else if (refilling && distanceCm <= waterDistanceThreshold - LEVEL_HYST_CM) {
    refilling = false;
  }

  digitalWrite(SOLENOID_SRC, refilling ? HIGH : LOW);
  setMainPump(refilling ? MAIN_PUMP_DUTY : 0);
}

void checkAlerts() {
  bool phBad = phValue < pH_low || phValue > pH_high;
  bool tdsBad = tdsValue > tds_max;
  alarmOn = phBad || tdsBad;

  digitalWrite(BUZZER, alarmOn ? HIGH : LOW);
  digitalWrite(GREEN_LED, alarmOn ? LOW : HIGH);
}

void runDosing() {
  unsigned long now = millis();

  if (dosing) {
    if (now - doseStart >= DOSE_RUN_MS) {
      digitalWrite(NUTRI_PUMP, LOW);
      digitalWrite(SOLENOID_NUT, LOW);
      dosing = false;
      everDosed = true;
      doseEnd = now;
      Serial.println("dosing finished");
    }
    return;
  }

  bool waited = !everDosed || (now - doseEnd >= DOSE_WAIT_MS);
  if (tdsValue < tds_min && waited) {
    digitalWrite(SOLENOID_NUT, HIGH);
    digitalWrite(NUTRI_PUMP, HIGH);
    dosing = true;
    doseStart = now;
    Serial.println("dosing nutrients");
  }
}

void updateLcd() {
  char l1[17], l2[17];
  bool pageTwo = (millis() / PAGE_MS) % 2;

  if (!pageTwo) {
    snprintf(l1, sizeof(l1), "T:%.1fC H:%.0f%%", temperature, humidity);
    snprintf(l2, sizeof(l2), "pH:%.2f TDS:%d", phValue, (int)tdsValue);
  } else {
    snprintf(l1, sizeof(l1), "Lvl:%.1fcm", distanceOk ? distanceCm : 0.0);
    snprintf(l2, sizeof(l2), "Flow:%.2f L/min", flowRate);
  }

  char row[17];
  lcd.setCursor(0, 0);
  snprintf(row, sizeof(row), "%-16s", l1);
  lcd.print(row);
  lcd.setCursor(0, 1);
  snprintf(row, sizeof(row), "%-16s", l2);
  lcd.print(row);
}

void handleStatus() {
  String j = "{";
  j += "\"temp\":" + String(temperature, 1);
  j += ",\"hum\":" + String(humidity, 0);
  j += ",\"ph\":" + String(phValue, 2);
  j += ",\"tds\":" + String(tdsValue, 0);
  j += ",\"level_cm\":" + String(distanceCm, 1);
  j += ",\"flow_lpm\":" + String(flowRate, 2);
  j += ",\"ldr\":" + String(ldrVal);
  j += ",\"lights_off\":" + String(lightsOff ? "true" : "false");
  j += ",\"ac\":" + String(acOn ? "true" : "false");
  j += ",\"refilling\":" + String(refilling ? "true" : "false");
  j += ",\"dosing\":" + String(dosing ? "true" : "false");
  j += ",\"alarm\":" + String(alarmOn ? "true" : "false");
  j += ",\"moisture\":[" + String(moisture[0]) + "," + String(moisture[1]) + "," +
       String(moisture[2]) + "," + String(moisture[3]) + "]";
  j += "}";
  server.send(200, "application/json", j);
}

void connectWifi() {
  WiFi.begin(ssid, password);
  Serial.print("wifi");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" connected, ip " + WiFi.localIP().toString());
    server.on("/status", handleStatus);
    server.begin();
  } else {
    Serial.println(" failed, running offline");
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(L293D_EN, OUTPUT);
  pinMode(RELAY_AC, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(OXY_PUMP, OUTPUT);
  pinMode(NUTRI_PUMP, OUTPUT);
  pinMode(SOLENOID_SRC, OUTPUT);
  pinMode(SOLENOID_NUT, OUTPUT);
  pinMode(MUX_A, OUTPUT);
  pinMode(MUX_B, OUTPUT);
  pinMode(MUX_C, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  pinMode(WATER_FLOW, INPUT_PULLUP);

  digitalWrite(RELAY_AC, LOW);
  digitalWrite(BUZZER, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(NUTRI_PUMP, LOW);
  digitalWrite(SOLENOID_SRC, LOW);
  digitalWrite(SOLENOID_NUT, LOW);
  digitalWrite(L293D_EN, HIGH);
  digitalWrite(OXY_PUMP, HIGH);

  attachInterrupt(digitalPinToInterrupt(WATER_FLOW), pulseCounter, RISING);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(MAIN_PUMP_PWM, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(MAIN_PUMP_PWM, 0);
#endif
  setMainPump(0);

  dht.begin();
  lcd.begin(16, 2);
  lcd.clear();
  lcd.print("Hydroponics");
  lcd.setCursor(0, 1);
  lcd.print("starting...");

  connectWifi();

  lastFlowCalc = millis();
  delay(1500);
  lcd.clear();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    sampleSensors();
    controlClimate();
    controlWaterLevel();
    checkAlerts();
  }

  runDosing();
  digitalWrite(OXY_PUMP, HIGH);

  if (now - lastLcd >= LCD_MS) {
    lastLcd = now;
    updateLcd();
  }

  if (now - lastPrint >= 5000) {
    lastPrint = now;
    Serial.printf("T=%.1f H=%.0f pH=%.2f TDS=%d lvl=%.1f flow=%.2f ldr=%d ac=%d refill=%d dose=%d alarm=%d\n",
                  temperature, humidity, phValue, (int)tdsValue, distanceCm, flowRate,
                  ldrVal, acOn, refilling, dosing, alarmOn);
  }
}
