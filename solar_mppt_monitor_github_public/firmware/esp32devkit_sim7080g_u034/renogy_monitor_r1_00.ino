/*
  Renogy Rover MPPT Charge Controller Monitor Gateway v1.00 (based on r0_42)
  - ESP32-DevKit
  - RS485: M5Stack U034 (auto-direction)  RX=16 TX=17  (Serial2)
  - LTE-M: M5Stack SIM7080G Unit          RX=26 TX=27  (SerialAT)
  - Upload: http://example.com/solar_mppt_monitor/ingest.php

  Identify:
  - DEVICE_ID 固定（交換時も同じIDを書き込むだけ）
  - secret は devices.json と一致必須

  LEDs:
  A) PWR  GPIO18 : always ON while awake
  B) MB   GPIO19 : Modbus access pulse 50-150ms
  C) LTE  GPIO21 : ON when GPRS connected

  Notes:
  - 10分周期：起床→Modbus→LTE接続→時刻同期→POST→切断→DeepSleep
  - seq は RTC保持（DeepSleepしても連番維持）
*/

#include <Arduino.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include "ModbusMaster.h"

// TinyGSM
#define TINY_GSM_MODEM_SIM7080
#include <TinyGsmClient.h>

// -------------------- User settings --------------------
static const char* POST_URL = POST_URL_CFG;

// ★IDは指定どおり固定
static const char* DEVICE_ID = DEVICE_ID_CFG;

// ★devices.json の secret と一致させること（必須）
static const char* DEVICE_SECRET = DEVICE_SECRET_CFG;

static const char* APN = APN_CFG;
static const char* APN_USER = APN_USER_CFG;
static const char* APN_PASS = APN_PASS_CFG;

// ★10分周期
static const uint32_t UPLOAD_INTERVAL_MS = 10UL * 60UL * 1000UL;

// RS485 / Modbus
static const int RS485_RX = 16;
static const int RS485_TX = 17;
static const uint32_t MODBUS_BAUD = 9600;
static const uint8_t SLAVE_ID_DEFAULT = 1;

// LTE-M UART
static const int MODEM_RX = 26;
static const int MODEM_TX = 27;
static const uint32_t MODEM_BAUD = 115200;

// wait network
static const uint32_t WAIT_NETWORK_MS = 180000; // 180s

// -------------------- LED pins --------------------
static const int LED_PWR = 18;
static const int LED_MB  = 19;
static const int LED_LTE = 21;
static uint32_t mbLedOffAt = 0;

// -------------------- Globals --------------------
HardwareSerial RS485Serial(2);
HardwareSerial SerialAT(1);

ModbusMaster mb;

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);

// ---- Auto slave-id detection ----
static uint8_t activeSlaveId = SLAVE_ID_DEFAULT;
static uint8_t mbFailCount = 0;
static const uint8_t MB_FAIL_RESCAN_THRESHOLD = 3;

// ---- RTC keep ----
RTC_DATA_ATTR uint32_t g_seq = 0;  // DeepSleepでも保持

// -------------------- Telemetry --------------------
struct Telemetry {
  uint32_t uptime_s = 0;
  bool mb_ok = true;

  uint16_t soc = 0;     // %
  float batt_v = NAN;   // V
  float batt_a = NAN;   // A (signed)
  float pv_v   = NAN;   // V
  float pv_a   = NAN;   // A (signed)
  float pv_w   = NAN;   // W  (Renogy側が返す値)
  float load_w = NAN;   // W  (可能なら取得)
  int charge_state = -1; // -1 unknown
  uint32_t fault = 0;

  bool time_ok = false;
};

// -------------------- LED helpers --------------------
static inline void ledInit() {
  pinMode(LED_PWR, OUTPUT);
  pinMode(LED_MB, OUTPUT);
  pinMode(LED_LTE, OUTPUT);
  digitalWrite(LED_PWR, HIGH);
  digitalWrite(LED_MB, LOW);
  digitalWrite(LED_LTE, LOW);
}

static inline void mbLedPulse(uint16_t ms = 120) {
  digitalWrite(LED_MB, HIGH);
  mbLedOffAt = millis() + ms;
}

static inline void mbLedTask() {
  if (mbLedOffAt && (int32_t)(millis() - mbLedOffAt) >= 0) {
    digitalWrite(LED_MB, LOW);
    mbLedOffAt = 0;
  }
}

static inline void setLteLed(bool on) {
  digitalWrite(LED_LTE, on ? HIGH : LOW);
}

// -------------------- Sleep helper --------------------
static void goDeepSleep(uint32_t sleepMs) {
  Serial.printf("[SLEEP] %lu ms\n", (unsigned long)sleepMs);

  digitalWrite(LED_MB, LOW);
  digitalWrite(LED_LTE, LOW);
  digitalWrite(LED_PWR, LOW);

  esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// -------------------- Modbus slave-id scan --------------------
static bool scanRenogySlaveId(uint8_t minId = 1, uint8_t maxId = 20) {
  Serial.printf("[MB] scanning slave id (%u..%u)...\n", minId, maxId);

  for (uint8_t id = minId; id <= maxId; id++) {
    mb.begin(id, RS485Serial);

    mb.clearResponseBuffer();
    uint8_t rc = mb.readHoldingRegisters(0x0100, 3); // SOC, BattV, BattA
    mbLedPulse(120);

    if (rc == mb.ku8MBSuccess) {
      activeSlaveId = id;
      mbFailCount = 0;
      Serial.printf("[MB] FOUND slave id = %u\n", id);

      for (int i = 0; i < 3; i++) {
        digitalWrite(LED_MB, HIGH); delay(120);
        digitalWrite(LED_MB, LOW);  delay(120);
      }
      return true;
    }
    delay(250);
  }

  Serial.println("[MB] no slave responded");
  return false;
}

// -------------------- Modbus helpers --------------------
static bool readRegs(uint16_t start, uint16_t count) {
  mb.clearResponseBuffer();
  uint8_t rc = mb.readHoldingRegisters(start, count);
  mbLedPulse(120);

  if (rc != mb.ku8MBSuccess) {
    Serial.printf("[MB] read 0x%04X x%u ERR=%u\n", start, count, rc);
    return false;
  }
  return true;
}

static void readMonitoring(Telemetry &t) {
  bool okAll = true;

  // 0x0100 x3 : SOC, BattV(0.1V), BattA(0.01A signed)
  if (readRegs(0x0100, 3)) {
    t.soc    = (uint16_t)mb.getResponseBuffer(0);
    t.batt_v = (uint16_t)mb.getResponseBuffer(1) * 0.1f;

    int16_t batt_a_001 = (int16_t)mb.getResponseBuffer(2);
    t.batt_a = batt_a_001 * 0.01f;
  } else okAll = false;

  // 0x0107 x3 : PVV(0.1V), PVA(0.01A signed), PVW or CHG_W (W)
  if (readRegs(0x0107, 3)) {
    t.pv_v = (uint16_t)mb.getResponseBuffer(0) * 0.1f;

    int16_t pv_a_001 = (int16_t)mb.getResponseBuffer(1);
    t.pv_a = pv_a_001 * 0.01f;

    t.pv_w = (uint16_t)mb.getResponseBuffer(2); // W
  } else okAll = false;

  // load_w（機種・ファームで場所が違うことがあるため、取れたら入れる）
  // r0系で 0x0106 を Load W として扱っていた例に合わせて「1レジスタ試す」
  // 失敗しても全体mb_okは落とさない（オプション扱い）
  {
    if (readRegs(0x0106, 1)) {
      t.load_w = (uint16_t)mb.getResponseBuffer(0) * 1.0f;
    } else {
      // optional
    }
  }

  // charge_state（0x0120 x1 を試す：失敗してもOK）
  {
    if (readRegs(0x0120, 1)) {
  //    t.charge_state = (int)mb.getResponseBuffer(0);
      t.charge_state = -1;
    } else {
      // optional
    }
  }

  // fault 0x0121 x2（取れたら入れる。失敗してもOK）
  {
    if (readRegs(0x0121, 2)) {
      uint16_t hi = mb.getResponseBuffer(0);
      uint16_t lo = mb.getResponseBuffer(1);
      t.fault = (uint32_t(hi) << 16) | lo;
    } else {
      // optional
    }
  }

  t.mb_ok = okAll;
}

static bool syncTimeFromModem_CCLK() {
  // 返答例: +CCLK: "26/02/07,20:55:14+36"
  // +36 は 15分単位（+36 => +9:00）
Serial.println("[TIME] AT+CCLK? query...");
  modem.sendAT("+CCLK?");
  if (modem.waitResponse(2000L, "+CCLK:") != 1) {
    // 念のため応答を取り切る
    modem.waitResponse(2000L);
    return false;
  }

  String line = modem.stream.readStringUntil('\n');
  line.trim();
Serial.print("[TIME] CCLK raw: ");
Serial.println(line);

  int yy=0, mo=0, dd=0, hh=0, mm=0, ss=0, tzq=0;
  // line には "26/02/07,20:55:14+36" が含まれる想定
  // 先頭に " が来たり来なかったりするので緩めに parse
  const char* s = line.c_str();
  const char* q = strchr(s, '"');
  if (q) s = q + 1;

  // tz は +36 / -12 等（15分単位）
  // sscanf は + の前提でも - でも通る
  if (sscanf(s, "%d/%d/%d,%d:%d:%d%d", &yy, &mo, &dd, &hh, &mm, &ss, &tzq) < 6) {
    return false;
  }

  // 年補正（"26" -> 2026）
  int year = 2000 + yy;

  // 端末の表示時刻は「ローカル時刻（TZ込み）」なので、
  // epoch にするには tzq(15分単位)を引いてUTCに戻す。
  // tzq=+36 => +9:00 => local = UTC+9 なので、UTC = local - 9h
  int tzMin = tzq * 15;

  struct tm tmv {};
  tmv.tm_year = year - 1900;
  tmv.tm_mon  = mo - 1;
  tmv.tm_mday = dd;
  tmv.tm_hour = hh;
  tmv.tm_min  = mm;
  tmv.tm_sec  = ss;

  // mktime は「ローカル扱い」なので、そのままだと二重にズレます。
  // ここでは "UTCとして" epoch を作りたいので timegm 相当が必要。
  // ESP32には timegm が無い環境があるので、mktime を使って補正します。
  time_t localEpoch = mktime(&tmv);
  if (localEpoch < 1577836800) return false;

  // ここで tzMin を引いて UTC epoch に近づける（local->UTC）
  time_t utcEpoch = localEpoch - (tzMin * 60);

  struct timeval tv;
  tv.tv_sec = utcEpoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  return true;
}

// -------------------- Time sync from modem --------------------
// modem.getGSMDateTime(DATE_FULL) returns: "YYYY/MM/DD,HH:MM:SS+TZ" or similar
static bool syncTimeFromModem() {
  String dt = modem.getGSMDateTime(DATE_FULL);
  dt.trim();
  if (dt.length() < 17) return false;

  // Example: 2026/02/07,12:34:56+09
  int Y=0,M=0,D=0,h=0,m=0,s=0;
  if (sscanf(dt.c_str(), "%d/%d/%d,%d:%d:%d", &Y,&M,&D,&h,&m,&s) != 6) return false;

  struct tm tmv {};
  tmv.tm_year = Y - 1900;
  tmv.tm_mon  = M - 1;
  tmv.tm_mday = D;
  tmv.tm_hour = h;
  tmv.tm_min  = m;
  tmv.tm_sec  = s;

  time_t epoch = mktime(&tmv); // uses local TZ in firmware; we just need "reasonable epoch"
  if (epoch < 1577836800) return false; // < 2020-01-01

  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  return true;
}

static bool isTimeValid() {
  time_t now = time(nullptr);
  return now >= 1577836800; // 2020-01-01
}

// -------------------- JSON builder (v1.00 schema) --------------------
static void buildJsonV100(const Telemetry &t, char* out, size_t outSize) {
  // seq (RTC保持)
  g_seq++;

  const uint32_t ts = (uint32_t)time(nullptr);

  // null handling
  auto f_or_null = [](float v, char* buf, size_t n, int dec)->void{
    if (isnan(v)) { strncpy(buf, "null", n); buf[n-1] = 0; }
    else {
      char fmt[8];
      snprintf(fmt, sizeof(fmt), "%%.%df", dec);
      snprintf(buf, n, fmt, v);
    }
  };

  char batt_v_s[16], batt_a_s[16], pv_v_s[16], pv_a_s[16], pv_w_s[16], load_w_s[16];
  f_or_null(t.batt_v, batt_v_s, sizeof(batt_v_s), 2);
  f_or_null(t.batt_a, batt_a_s, sizeof(batt_a_s), 2);
  f_or_null(t.pv_v,   pv_v_s,   sizeof(pv_v_s),   2);
  f_or_null(t.pv_a,   pv_a_s,   sizeof(pv_a_s),   2);
  f_or_null(t.pv_w,   pv_w_s,   sizeof(pv_w_s),   0);
  f_or_null(t.load_w, load_w_s, sizeof(load_w_s), 1);

  snprintf(out, outSize,
    "{"
      "\"v\":\"1.00\","
      "\"device\":\"%s\","
      "\"secret\":\"%s\","
      "\"ts\":%lu,"
      "\"seq\":%lu,"
      "\"metrics\":{"
        "\"batt_v\":%s,"
        "\"batt_a\":%s,"
        "\"soc\":%u,"
        "\"pv_v\":%s,"
        "\"pv_a\":%s,"
        "\"pv_w\":%s,"
        "\"load_w\":%s,"
        "\"charge_state\":%d,"
        "\"fault\":%lu,"
        "\"mb_ok\":%s"
      "}"
    "}",
    DEVICE_ID,
    DEVICE_SECRET,
    (unsigned long)ts,
    (unsigned long)g_seq,
    batt_v_s,
    batt_a_s,
    (unsigned)t.soc,
    pv_v_s,
    pv_a_s,
    pv_w_s,
    load_w_s,
    t.charge_state,
    (unsigned long)t.fault,
    (t.mb_ok ? "true" : "false")
  );
}

// -------------------- LTE connect --------------------
static bool connectLTE_once() {
  setLteLed(false);

  Serial.println("[LTE] SerialAT begin...");
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(300);

  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    if (modem.testAT()) break;
    delay(200);
  }

  Serial.println("[LTE] Modem restart...");
  if (!modem.restart()) {
    Serial.println("[LTE] modem.restart() FAILED");
    return false;
  }

  Serial.println("[LTE] Set APN context (CGDCONT)...");
  {
    String cmd = String("+CGDCONT=1,\"IP\",\"") + APN + "\"";
    modem.sendAT(cmd);
    modem.waitResponse(5000L);
  }

  // 優先設定（効かない個体もあるため失敗しても続行）
  modem.sendAT("+CMNB=1");  modem.waitResponse(2000);
  modem.sendAT("+CNMP=38"); modem.waitResponse(2000);

  Serial.printf("[LTE] Wait for network (up to %lus)...\n",
                (unsigned long)(WAIT_NETWORK_MS / 1000UL));
  if (!modem.waitForNetwork(WAIT_NETWORK_MS)) {
    Serial.println("[LTE] waitForNetwork FAIL");
    return false;
  }
  Serial.println("[LTE] Network OK");

  Serial.println("[LTE] GPRS connect (APN)...");
  if (!modem.gprsConnect(APN, APN_USER, APN_PASS)) {
    Serial.println("[LTE] gprsConnect FAIL");
    return false;
  }
  Serial.println("[LTE] GPRS OK");

  Serial.print("[LTE] IP: ");
  Serial.println(modem.getLocalIP());

  setLteLed(true);
  return true;
}

// -------------------- HTTP POST --------------------
static bool httpPostJsonFixed(const char* url, const char* json) {
  if (strncmp(url, "http://", 7) != 0) {
    Serial.println("[HTTP] Only http:// supported");
    return false;
  }

  const char* hostStart = url + 7;
  const char* pathStart = strchr(hostStart, '/');

  char host[96];
  const char* path = "/";

  if (pathStart) {
    size_t hl = (size_t)(pathStart - hostStart);
    if (hl >= sizeof(host)) hl = sizeof(host) - 1;
    memcpy(host, hostStart, hl);
    host[hl] = '\0';
    path = pathStart;
  } else {
    strncpy(host, hostStart, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
  }

  int port = 80;
  char* colon = strchr(host, ':');
  if (colon) {
    port = atoi(colon + 1);
    *colon = '\0';
  }

  Serial.printf("[HTTP] connect %s:%d\n", host, port);
  if (!client.connect(host, port)) {
    Serial.println("[HTTP] connect FAIL");
    return false;
  }

  size_t len = strlen(json);

  client.print("POST ");
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(host);
  client.print("\r\nUser-Agent: solar-mppt/1.00\r\nConnection: close\r\n");
  client.print("Content-Type: application/json\r\nContent-Length: ");
  client.print((unsigned long)len);
  client.print("\r\n\r\n");
  client.print(json);

  uint32_t t0 = millis();
  while (client.connected() && !client.available() && (millis() - t0 < 10000)) {
    mbLedTask();
    delay(20);
  }

  if (!client.available()) {
    Serial.println("[HTTP] no response");
    client.stop();
    return false;
  }

  String status = client.readStringUntil('\n');
  status.trim();
  Serial.println("[HTTP] " + status);

  bool ok = status.startsWith("HTTP/1.1 200") || status.startsWith("HTTP/1.1 204");

  while (client.available()) client.read();
  client.stop();
  return ok;
}

// -------------------- LTE disconnect / poweroff --------------------
static void shutdownLTE() {
  setLteLed(false);

  modem.gprsDisconnect();
  delay(200);

  modem.sendAT("+CPOWD=1");
  modem.waitResponse(3000);
  delay(200);

  SerialAT.end();
}

// -------------------- setup/loop --------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\n=== Renogy Rover MPPT Charge Controller Monitor Gateway v1.00 ===");
  Serial.print("[CFG] device_id=");
  Serial.println(DEVICE_ID);

  ledInit();

  // RS485(Modbus)
  RS485Serial.begin(MODBUS_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);

  // 起床ごとにID検出（確実性優先）
  if (!scanRenogySlaveId(1, 20)) {
    Serial.println("[MB] no slave detected at startup");
  }

  Serial.printf("[MB] activeSlaveId=%u\n", activeSlaveId);
  Serial.println("[MB] RS485 started @9600 8N1 (U034 auto-direction)");
}

void loop() {
  mbLedTask();

  Telemetry t;
  t.uptime_s = millis() / 1000UL;

  // ID反映
  mb.begin(activeSlaveId, RS485Serial);

  // ---- Modbus read ----
  readMonitoring(t);

  if (!t.mb_ok) {
    mbFailCount++;
    Serial.printf("[MB] fail count=%u\n", mbFailCount);

    if (mbFailCount >= MB_FAIL_RESCAN_THRESHOLD) {
      Serial.println("[MB] re-scan slave id...");
      scanRenogySlaveId(1, 20);
      mbFailCount = 0;

      mb.begin(activeSlaveId, RS485Serial);
      readMonitoring(t);
    }
  } else {
    mbFailCount = 0;
  }

  // ---- LTE connect -> time sync -> POST ----
  bool lteOk = connectLTE_once();
  setLteLed(lteOk && modem.isGprsConnected());

  // 時刻同期（成功したら ts が有効になる）
  if (lteOk && modem.isGprsConnected()) {
t.time_ok = syncTimeFromModem_CCLK();
if (!t.time_ok) {
  // 予備：TinyGSM API でも試す（どちらか成功すればOK）
  t.time_ok = syncTimeFromModem();
}
Serial.printf("[TIME] sync %s (now=%ld)\n", t.time_ok ? "OK" : "NG", (long)time(nullptr));
  }
  if (!isTimeValid()) {
    Serial.println("[TIME] WARNING: system time invalid; ts may be wrong");
  }

  // JSON build (v1.00)
  char json[768];
  buildJsonV100(t, json, sizeof(json));
  Serial.print("[DATA] ");
  Serial.println(json);

  bool postOk = false;

  if (lteOk && modem.isGprsConnected()) {
    postOk = httpPostJsonFixed(POST_URL, json);
    Serial.printf("[POST] %s\n", postOk ? "OK" : "NG");

    // 失敗時：1回だけ再接続して再送
    if (!postOk) {
      Serial.println("[POST] retry once after reconnect...");
      shutdownLTE();
      delay(500);
      if (connectLTE_once() && modem.isGprsConnected()) {
        postOk = httpPostJsonFixed(POST_URL, json);
        Serial.printf("[POST] retry %s\n", postOk ? "OK" : "NG");
      }
    }
  } else {
    Serial.println("[LTE] connect failed; skip POST");
  }

  shutdownLTE();

  goDeepSleep(UPLOAD_INTERVAL_MS);
}
