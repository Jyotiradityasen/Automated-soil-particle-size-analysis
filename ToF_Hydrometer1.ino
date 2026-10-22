/*
  ToF Automated Hydrometer — ESP32-S3 Reverse TFT Feather rebuild (no screen)
  Replicates: Murad, Jones & Minasny (2020) SSSAJ 84:690-699, doi:10.1002/saj2.20053

  Hardware
    - Adafruit ESP32-S3 Reverse TFT Feather (PID 5691) — screen is switched off
    - Adafruit VL6180X ToF sensor (PID 3316)  -> STEMMA QT cable
    - DS18B20 waterproof probe (PID 381): red -> 3V, black -> GND,
                                          data -> pin 5, 4.7k between data and 3V

  Buttons
    D1 : START a run  -> press the moment you pull the stirrer out (t = 0)
    D2 : STOP the run

  Serial commands (115200 baud, send one letter):
    s = start run      x = stop run
    d = dump log file  p = dump previous run's log
    i = info           e = erase both logs

  Log format (CSV, one row every SAMPLE_INTERVAL_MS):
    t_s, dist_mm, dist_sd_mm, n_valid, temp_C
  Written to the Feather's flash (survives power loss) AND printed to Serial.

  Libraries: Adafruit VL6180X, OneWire, DallasTemperature
  Board: esp32 by Espressif (3.x) -> "Adafruit Feather ESP32-S3 Reverse TFT"
*/

#include <Wire.h>
#include <Adafruit_VL6180X.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <FS.h>
#include <LittleFS.h>

// ---------------- user settings ----------------
const uint8_t  ONE_WIRE_PIN       = 5;        // DS18B20 data pin
const uint32_t SAMPLE_INTERVAL_MS = 5000;     // paper: 5 s
const uint32_t RUN_HOURS          = 12;       // paper: 8 h. 12 h gets you all the way to 2 um
const uint8_t  READS_PER_SAMPLE   = 15;       // ToF reads per logged sample (median)
const uint32_t I2C_CLOCK_HZ       = 100000;   // slow bus = robust over the 400 mm cable
// ------------------------------------------------

const char *LOG_PATH  = "/run.csv";
const char *PREV_PATH = "/run_prev.csv";

// Reverse TFT buttons: D1 and D2 are pulled LOW and read HIGH when pressed
const uint8_t BTN_D1 = 1, BTN_D2 = 2;

Adafruit_VL6180X tof;
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature tempSensor(&oneWire);

bool tofOK = false, tempOK = false, fsOK = false;
bool running = false;
uint32_t runStartMs = 0, lastSampleMs = 0, nSamples = 0;
float lastDist = NAN, lastSd = NAN, lastTemp = NAN;
uint8_t lastValid = 0;

// ---------- helpers ----------
static void sortBytes(uint8_t *a, uint8_t n) {
  for (uint8_t i = 1; i < n; i++) {
    uint8_t v = a[i]; int8_t j = i - 1;
    while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}

// Take several ToF readings; return the median, spread and count of valid readings.
void readDistance(float &medianMm, float &sdMm, uint8_t &nValid) {
  uint8_t buf[READS_PER_SAMPLE];
  nValid = 0;
  for (uint8_t i = 0; i < READS_PER_SAMPLE; i++) {
    uint8_t r = tof.readRange();
    if (tof.readRangeStatus() == VL6180X_ERROR_NONE) buf[nValid++] = r;
    delay(5);
  }
  if (nValid == 0) { medianMm = NAN; sdMm = NAN; return; }
  sortBytes(buf, nValid);
  medianMm = (nValid % 2) ? buf[nValid / 2]
                          : 0.5f * (buf[nValid / 2 - 1] + buf[nValid / 2]);
  float mean = 0; for (uint8_t i = 0; i < nValid; i++) mean += buf[i];
  mean /= nValid;
  float var = 0; for (uint8_t i = 0; i < nValid; i++) var += (buf[i] - mean) * (buf[i] - mean);
  sdMm = (nValid > 1) ? sqrtf(var / (nValid - 1)) : 0;
}

float readTempC() {
  tempSensor.requestTemperatures();               // ~750 ms at 12-bit
  float c = tempSensor.getTempCByIndex(0);
  return (c == DEVICE_DISCONNECTED_C) ? NAN : c;
}

String fmt(float v, int dp) { return isnan(v) ? String("--") : String(v, dp); }

void appendLine(const String &line) {
  if (!fsOK) return;
  File f = LittleFS.open(LOG_PATH, FILE_APPEND);  // open/close each time: power-loss safe
  if (f) { f.println(line); f.close(); }
}

void dumpFile(const char *path) {
  if (!fsOK || !LittleFS.exists(path)) { Serial.println("# no log file"); return; }
  File f = LittleFS.open(path, FILE_READ);
  Serial.println("#BEGIN");
  while (f.available()) Serial.write(f.read());
  f.close();
  Serial.println("#END");
}

void startRun() {
  if (fsOK) {
    if (LittleFS.exists(PREV_PATH)) LittleFS.remove(PREV_PATH);
    if (LittleFS.exists(LOG_PATH))  LittleFS.rename(LOG_PATH, PREV_PATH);  // never lose a run
  }
  running = true; nSamples = 0;
  runStartMs = millis();
  lastSampleMs = runStartMs - SAMPLE_INTERVAL_MS;   // take the first sample right away
  appendLine("# ToF hydrometer log; t=0 at START press; interval_ms=" + String(SAMPLE_INTERVAL_MS));
  appendLine("t_s,dist_mm,dist_sd_mm,n_valid,temp_C");
  Serial.println("# RUN STARTED");
  Serial.println("t_s,dist_mm,dist_sd_mm,n_valid,temp_C");
}

void stopRun() {
  if (!running) return;
  running = false;
  Serial.println("# RUN STOPPED, samples=" + String(nSamples));
}

void printInfo() {
  Serial.printf("# ToF:%s  DS18B20:%s  flash:%s  running:%d  samples:%lu\n",
                tofOK ? "ok" : "MISSING", tempOK ? "ok" : "MISSING",
                fsOK ? "ok" : "FAILED", running, (unsigned long)nSamples);
  if (fsOK) Serial.printf("# flash used %u / %u bytes\n",
                          (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

bool pressed(uint8_t pin) {           // D1/D2 read HIGH when pressed; debounce + wait for release
  if (digitalRead(pin) != HIGH) return false;
  delay(30);
  if (digitalRead(pin) != HIGH) return false;
  while (digitalRead(pin) == HIGH) delay(5);
  return true;
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BTN_D1, INPUT_PULLDOWN);
  pinMode(BTN_D2, INPUT_PULLDOWN);

  // This pin powers the STEMMA QT port (and the screen) — must stay HIGH for the ToF sensor.
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  // Screen not used: keep its backlight off.
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, LOW);
  delay(20);

  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);
  tofOK = tof.begin(&Wire);

  tempSensor.begin();
  tempSensor.setResolution(12);       // 0.0625 C steps
  tempOK = tempSensor.getDeviceCount() > 0;

  // Default partition scheme on this board names the data partition "ffat";
  // other schemes call it "spiffs". Try both.
  fsOK = LittleFS.begin(true);
  if (!fsOK) fsOK = LittleFS.begin(true, "/littlefs", 5, "ffat");

  printInfo();
  Serial.println("# commands: s=start x=stop d=dump p=prev i=info e=erase");
}

void loop() {
  // ---- inputs ----
  if (pressed(BTN_D1) && !running) startRun();
  if (pressed(BTN_D2)) stopRun();

  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 's': if (!running) startRun(); break;
      case 'x': stopRun(); break;
      case 'd': dumpFile(LOG_PATH); break;
      case 'p': dumpFile(PREV_PATH); break;
      case 'i': printInfo(); break;
      case 'e':
        if (!running && fsOK) { LittleFS.remove(LOG_PATH); LittleFS.remove(PREV_PATH);
                                Serial.println("# logs erased"); }
        break;
    }
  }

  // ---- sampling ----
  uint32_t now = millis();
  uint32_t interval = running ? SAMPLE_INTERVAL_MS : 1000;   // live readings at 1 Hz when idle
  if (now - lastSampleMs >= interval) {
    lastSampleMs += interval;
    if (!running) lastSampleMs = now;

    if (tofOK) readDistance(lastDist, lastSd, lastValid);
    lastTemp = readTempC();

    if (running) {
      // timestamp = when this sample was scheduled, so 5 s spacing never drifts
      float t_s = (lastSampleMs - runStartMs) / 1000.0f;
      String line = String(t_s, 1) + "," + fmt(lastDist, 1) + "," + fmt(lastSd, 2) + "," +
                    String(lastValid) + "," + fmt(lastTemp, 3);
      appendLine(line);
      Serial.println(line);
      nSamples++;
      if (t_s >= RUN_HOURS * 3600.0f) stopRun();
    } else {
      Serial.printf("# live: dist=%s mm  sd=%s  ok=%u/%u  T=%s C\n",
                    fmt(lastDist, 1).c_str(), fmt(lastSd, 2).c_str(),
                    lastValid, READS_PER_SAMPLE, fmt(lastTemp, 2).c_str());
    }
  }
}
