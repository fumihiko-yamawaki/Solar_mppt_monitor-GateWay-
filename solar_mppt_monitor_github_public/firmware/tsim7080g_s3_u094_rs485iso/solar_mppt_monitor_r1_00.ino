/*
  Solar MPPT Monitor Gateway v1.00 (based on r0_03)
  - LilyGO T-SIM7080G-S3 (ESP32-S3 + SIM7080G)
  - RS485: M5Stack Unit RS485-ISO (U094)  UART2 RX=GPIO18 TX=GPIO17
  - Upload: http://example.com/solar_mppt_monitor/ingest.php

  Policy:
  - Modbus: slave_id=16 fixed (default), optional scan by switch
  - DeepSleep: wake -> Modbus -> LTE -> POST -> shutdown -> sleep
  - seq: kept in RTC memory across DeepSleep (power loss/reset resets)

  Registers (confirmed / protocol v1.0):
  - Holding 0x0100 x3 : SOC(%), BattV(0.1V), BattA(0.01A, signed)
  - Holding 0x0107 x3 : PVV(0.1V), PVA(0.01A, signed), CHG_W(W)
  - Holding 0x0104 x3 : LoadV(0.1V), LoadA(0.01A), LoadW(W)   <-- use LoadW
  - Holding 0x0120 x1 : Light status(Hi 8bit) + Charging status(Low 8bit)
  - Holding 0x0121 x2 : FAULT (32-bit hi/lo)
*/

#include <Arduino.h>
#include <esp_sleep.h>
#include <Wire.h>
#include "ModbusMaster.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#include "utilities.h"  // BOARD_MODEM_* pins, I2C_SDA/SCL

#define TINY_GSM_MODEM_SIM7080
#include <TinyGsmClient.h>

// -------------------- User settings --------------------

// v1.00 ingest endpoint (http only in this sketch)
static const char* POST_URL = POST_URL_CFG;

// ★現場識別子（固定指定）
static const char* DEVICE_ID = DEVICE_ID_CFG;

// ★devices.json の secret と同じ値にすること（必須）
static const char* DEVICE_SECRET = DEVICE_SECRET_CFG;

// APN (SORACOM)
static const char* APN = APN_CFG;
static const char* APN_USER = APN_USER_CFG;
static const char* APN_PASS = APN_PASS_CFG;

// 10分周期
static const uint32_t UPLOAD_INTERVAL_MS = 10UL * 60UL * 1000UL;
// NG時リトライ（短め）
static const uint32_t RETRY_INTERVAL_MS  = 60UL * 1000UL;

// RS485 pins (T-SIM7080G-S3 confirmed)
static const int RS485_RX = 18;
static const int RS485_TX = 17;
static const uint32_t MODBUS_BAUD = 9600;

// Renogy slave id (default fixed)
static const uint8_t RENOGY_SLAVE_ID_DEFAULT = 01;

// ★ID再スキャン（交換対策）: デフォルトOFF
static const bool ENABLE_SLAVE_SCAN = false;
static const uint8_t SCAN_MIN_ID = 1;
static const uint8_t SCAN_MAX_ID = 20;
static const uint8_t MB_FAIL_RESCAN_THRESHOLD = 3;

// LTE modem
static const uint32_t MODEM_BAUD = 115200;
static const uint32_t WAIT_NETWORK_MS = 180000; // 180s

// Network preference
// 0 = CATM+NBIOT (CMNB=3)
// 1 = CATM only   (CMNB=1)
// 2 = NBIOT only  (CMNB=2)
static const int NET_PREF_MODE = 0;

// CNMP profile
// 2: auto（あなたの成功ログでOK）
static const int CNMP_MODE = 2;

// -------------------- Globals --------------------
XPowersPMU PMU;

HardwareSerial RS485Serial(2);
HardwareSerial SerialAT(1);

ModbusMaster mb;

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);

static uint8_t activeSlaveId = RENOGY_SLAVE_ID_DEFAULT;
static uint8_t mbFailCount = 0;

// ★DeepSleep後も保持される送信シーケンス（RTCメモリ）
//  - DeepSleepからの復帰では保持
//  - 電源断/リセットでは 0 に戻る
RTC_DATA_ATTR uint32_t g_seqNo = 0;

// -------------------- Telemetry --------------------
struct Telemetry {
  uint32_t uptime_s = 0;
  bool mb_ok = false;

  uint16_t soc_pct = 0;   // %
  float bat_v = NAN;      // V
  float chg_a = NAN;      // A (signed possible)

  float pv_v  = NAN;      // V
  float pv_a  = NAN;      // A (signed possible)
  uint16_t chg_w = 0;     // W (Charging power)

  float load_w = NAN;     // W (Load power)
  float temp_c = NAN;     // ℃ (not read in this version)

  uint8_t charge_state = 0; // 00..06 (Charging status low 8bit of 0x0120)
  uint32_t fault = 0;       // 32-bit
};

// -------------------- Helpers --------------------
static const char* mbRcStr(uint8_t rc) {
  switch (rc) {
    case ModbusMaster::ku8MBSuccess: return "Success";
    case ModbusMaster::ku8MBIllegalFunction: return "IllegalFunction";
    case ModbusMaster::ku8MBIllegalDataAddress: return "IllegalDataAddress";
    case ModbusMaster::ku8MBIllegalDataValue: return "IllegalDataValue";
    case ModbusMaster::ku8MBSlaveDeviceFailure: return "SlaveDeviceFailure";
    case ModbusMaster::ku8MBInvalidSlaveID: return "InvalidSlaveID";
    case ModbusMaster::ku8MBInvalidFunction: return "InvalidFunction";
    case ModbusMaster::ku8MBResponseTimedOut: return "ResponseTimedOut";
    case ModbusMaster::ku8MBInvalidCRC: return "InvalidCRC";
    default: return "Other";
  }
}

static void rs485FlushRx(uint32_t ms = 30) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    while (RS485Serial.available()) RS485Serial.read();
    delay(1);
  }
}

static bool scanRenogySlaveId(uint8_t from, uint8_t to) {
  if (!ENABLE_SLAVE_SCAN) return false;

  Serial.printf("[MB] scan slave id %u..%u\n", from, to);
  for (uint8_t id = from; id <= to; id++) {
    mb.begin(id, RS485Serial);
    rs485FlushRx(20);

    mb.clearResponseBuffer();
    uint8_t rc = mb.readHoldingRegisters(0x0100, 3);
    Serial.printf("[MB] scan id=%u rc=%u(%s)\n", id, rc, mbRcStr(rc));

    if (rc == mb.ku8MBSuccess) {
      activeSlaveId = id;
      Serial.printf("[MB] FOUND slave id=%u\n", activeSlaveId);
      return true;
    }
    delay(150);
  }
  Serial.println("[MB] scan result: NOT FOUND");
  return false;
}

// -------------------- Modbus read (Holding Registers) --------------------
static void readMonitoring(Telemetry &t) {
  t.mb_ok = false;

  mb.begin(activeSlaveId, RS485Serial);
  rs485FlushRx(20);

  // 0x0100 x3 : SOC, BattV, BattA
  mb.clearResponseBuffer();
  uint8_t rc = mb.readHoldingRegisters(0x0100, 3);
  if (rc != mb.ku8MBSuccess) {
    Serial.printf("[MB] read 0x0100 x3 rc=%u(%s)\n", rc, mbRcStr(rc));
    return;
  }
  const uint16_t soc = mb.getResponseBuffer(0);
  const uint16_t batt_v_01 = mb.getResponseBuffer(1);                 // 0.1V
  const int16_t  batt_a_001 = (int16_t)mb.getResponseBuffer(2);        // 0.01A (signed)

  // 0x0107 x3 : PVV, PVA, CHG_W
  mb.clearResponseBuffer();
  rc = mb.readHoldingRegisters(0x0107, 3);
  if (rc != mb.ku8MBSuccess) {
    Serial.printf("[MB] read 0x0107 x3 rc=%u(%s)\n", rc, mbRcStr(rc));
    return;
  }
  const uint16_t pv_v_01 = mb.getResponseBuffer(0);                    // 0.1V
  const int16_t  pv_a_001 = (int16_t)mb.getResponseBuffer(1);          // 0.01A (signed)
  const uint16_t chg_w = mb.getResponseBuffer(2);                      // W

  // 0x0104 x3 : LoadV, LoadA, LoadW
  float load_w = NAN;
  mb.clearResponseBuffer();
  rc = mb.readHoldingRegisters(0x0104, 3);
  if (rc == mb.ku8MBSuccess) {
    const uint16_t load_w_reg = mb.getResponseBuffer(2);               // LoadW (W)
    load_w = (float)load_w_reg;
  } else {
    Serial.printf("[MB] read 0x0104 x3 rc=%u(%s) (continue)\n", rc, mbRcStr(rc));
  }

  // 0x0120 x1 : charging status low 8bit
  uint8_t charge_state = 0;
  mb.clearResponseBuffer();
  rc = mb.readHoldingRegisters(0x0120, 1);
  if (rc == mb.ku8MBSuccess) {
    const uint16_t st = mb.getResponseBuffer(0);
    charge_state = (uint8_t)(st & 0xFF);                               // 00..06
  } else {
    Serial.printf("[MB] read 0x0120 x1 rc=%u(%s) (continue)\n", rc, mbRcStr(rc));
  }

  // 0x0121 x2 : fault (32-bit)
  uint32_t fault = 0;
  mb.clearResponseBuffer();
  rc = mb.readHoldingRegisters(0x0121, 2);
  if (rc == mb.ku8MBSuccess) {
    uint16_t hi = mb.getResponseBuffer(0);
    uint16_t lo = mb.getResponseBuffer(1);
    fault = (uint32_t(hi) << 16) | lo;
  } else {
    Serial.printf("[MB] read 0x0121 x2 rc=%u(%s) (continue)\n", rc, mbRcStr(rc));
  }

  // reflect
  t.soc_pct = soc;
  t.bat_v = batt_v_01 * 0.1f;
  t.chg_a = batt_a_001 * 0.01f;

  t.pv_v  = pv_v_01 * 0.1f;
  t.pv_a  = pv_a_001 * 0.01f;
  t.chg_w = chg_w;

  t.load_w = load_w;
  t.charge_state = charge_state;

  t.fault = fault;

  // temp_c is not read in this version (keep NAN)
  t.mb_ok = true;
}

// -------------------- JSON builder (v1.00 ingest.php) --------------------
static void buildJsonV100(const Telemetry &t, char *out, size_t outLen) {
  const uint32_t ts = (uint32_t)time(nullptr);

  // ★RTC保持の連番
  g_seqNo++;
  const uint32_t seq = g_seqNo;

  char batv[16], chga[16], pvv[16], pva[16], loadw[16], tempc[16];
  if (isnan(t.bat_v))  strcpy(batv,  "null"); else snprintf(batv,  sizeof(batv),  "%.2f", t.bat_v);
  if (isnan(t.chg_a))  strcpy(chga,  "null"); else snprintf(chga,  sizeof(chga),  "%.2f", t.chg_a);
  if (isnan(t.pv_v))   strcpy(pvv,   "null"); else snprintf(pvv,   sizeof(pvv),   "%.2f", t.pv_v);
  if (isnan(t.pv_a))   strcpy(pva,   "null"); else snprintf(pva,   sizeof(pva),   "%.2f", t.pv_a);
  if (isnan(t.load_w)) strcpy(loadw, "null"); else snprintf(loadw, sizeof(loadw), "%.1f", t.load_w);
  if (isnan(t.temp_c)) strcpy(tempc, "null"); else snprintf(tempc, sizeof(tempc), "%.1f", t.temp_c);

  snprintf(
    out, outLen,
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
        "\"pv_w\":%u,"
        "\"load_w\":%s,"
        "\"temp_c\":%s,"
        "\"charge_state\":%u,"
        "\"fault\":%lu,"
        "\"mb_ok\":%s"
      "}"
    "}",
    DEVICE_ID,
    DEVICE_SECRET,
    (unsigned long)ts,
    (unsigned long)seq,
    batv,
    chga,
    (unsigned)t.soc_pct,
    pvv,
    pva,
    (unsigned)t.chg_w,
    loadw,
    tempc,
    (unsigned)t.charge_state,
    (unsigned long)t.fault,
    (t.mb_ok ? "true" : "false")
  );
}

// -------------------- PMU init --------------------
static bool initPMU_AXP2101() {
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    Serial.println("[PMU] begin failed");
    return false;
  }

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    PMU.disableDC3();
    delay(200);
  }

  PMU.setDC3Voltage(3000);
  PMU.enableDC3();

  PMU.setBLDO2Voltage(3300);
  PMU.enableBLDO2();

  PMU.disableTSPinMeasure();

  Serial.println("[PMU] DC3 enabled");
  return true;
}

// -------------------- Modem start (PWRKEY) --------------------
static bool startModem_PwrKey() {
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);
  delay(100);

  pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
  pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
  pinMode(BOARD_MODEM_RI_PIN, INPUT);

  digitalWrite(BOARD_MODEM_DTR_PIN, HIGH);

  int retry = 0;
  while (!modem.testAT(1000)) {
    Serial.print(".");
    if (retry++ > 6) {
      Serial.println("\n[MODEM] toggle PWRKEY");
      digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
      delay(100);
      digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
      delay(1000);
      digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
      retry = 0;
    }
    delay(200);
  }
  Serial.println("\n[MODEM] AT OK");
  return true;
}

static void applyNetworkProfile() {
  Serial.println("[LTE] RF OFF");
  modem.sendAT("+CFUN=0");
  modem.waitResponse(20000);

  Serial.printf("[LTE] CNMP=%d\n", CNMP_MODE);
  modem.sendAT("+CNMP=", CNMP_MODE);
  modem.waitResponse(5000);

  if (NET_PREF_MODE == 0) {
    Serial.println("[LTE] CMNB=3 (CATM+NBIOT)");
    modem.sendAT("+CMNB=3");
  } else if (NET_PREF_MODE == 1) {
    Serial.println("[LTE] CMNB=1 (CATM only)");
    modem.sendAT("+CMNB=1");
  } else {
    Serial.println("[LTE] CMNB=2 (NBIOT only)");
    modem.sendAT("+CMNB=2");
  }
  modem.waitResponse(5000);

  Serial.println("[LTE] set APN (CGDCONT/CNCFG)");
  modem.sendAT("+CGDCONT=1,\"IP\",\"", APN, "\"");
  modem.waitResponse(5000);

  modem.sendAT("+CNCFG=0,1,\"", APN, "\"");
  modem.waitResponse(5000);

  Serial.println("[LTE] RF ON");
  modem.sendAT("+CFUN=1");
  modem.waitResponse(20000);
}

static bool connectLTE_once() {
  Serial.println("[LTE] PMU init...");
  if (!initPMU_AXP2101()) return false;

  Serial.println("[LTE] start modem (PWRKEY)...");
  if (!startModem_PwrKey()) return false;

  Serial.println("[LTE] modem restart...");
  if (!modem.restart()) {
    Serial.println("[LTE] modem.restart FAILED");
    return false;
  }

  if (modem.getSimStatus() != SIM_READY) {
    Serial.print("[LTE] SIM status=");
    Serial.println(modem.getSimStatus());
    return false;
  }

  Serial.println("[LTE] warmup 8s...");
  delay(8000);

  applyNetworkProfile();

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
  return true;
}

// -------------------- HTTP POST (plain http) --------------------
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
    size_t hostLen = (size_t)(pathStart - hostStart);
    if (hostLen >= sizeof(host)) hostLen = sizeof(host) - 1;
    memcpy(host, hostStart, hostLen);
    host[hostLen] = '\0';
    path = pathStart;
  } else {
    strncpy(host, hostStart, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    path = "/";
  }

  if (!client.connect(host, 80)) {
    Serial.println("[HTTP] connect fail");
    return false;
  }

  String req;
  req.reserve(256 + strlen(json));
  req += "POST ";
  req += path;
  req += " HTTP/1.1\r\n";
  req += "Host: ";
  req += host;
  req += "\r\n";
  req += "User-Agent: solar-mppt-monitor\r\n";
  req += "Connection: close\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Content-Length: ";
  req += String(strlen(json));
  req += "\r\n\r\n";
  req += json;

  client.print(req);

  uint32_t t0 = millis();
  while (!client.available() && (millis() - t0) < 15000) delay(20);

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

static void shutdownLTE() {
  modem.gprsDisconnect();
  delay(200);

  modem.sendAT("+CPOWD=1");
  modem.waitResponse(3000);
  delay(200);

  SerialAT.end();
  PMU.disableDC3();
  Serial.println("[PMU] DC3 disabled");
}

static void goDeepSleepMs(uint32_t ms) {
  Serial.printf("[SLEEP] %lu ms\n", (unsigned long)ms);
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// -------------------- setup/loop --------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== Solar MPPT Monitor Gateway v1.00 (T-SIM7080G-S3) ===");
  Serial.print("[CFG] device=");
  Serial.println(DEVICE_ID);
  Serial.printf("[CFG] RS485 UART2 RX=%d TX=%d\n", RS485_RX, RS485_TX);

  auto cause = esp_sleep_get_wakeup_cause();
  Serial.printf("[BOOT] wakeup_cause=%d\n", (int)cause);
  Serial.printf("[BOOT] rtc_seq=%lu\n", (unsigned long)g_seqNo);

  RS485Serial.begin(MODBUS_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);
  delay(80);

  activeSlaveId = RENOGY_SLAVE_ID_DEFAULT;
  if (ENABLE_SLAVE_SCAN) {
    // 起動時だけスキャン（見つかったら更新）
    scanRenogySlaveId(SCAN_MIN_ID, SCAN_MAX_ID);
  }
  Serial.printf("[MB] activeSlaveId=%u\n", activeSlaveId);
}

void loop() {
  Telemetry t;
  t.uptime_s = millis() / 1000UL;

  // ---- Modbus ----
  readMonitoring(t);

  if (!t.mb_ok) {
    mbFailCount++;
    Serial.printf("[MB] fail count=%u\n", mbFailCount);

    if (ENABLE_SLAVE_SCAN && mbFailCount >= MB_FAIL_RESCAN_THRESHOLD) {
      Serial.println("[MB] rescan slave id...");
      if (scanRenogySlaveId(SCAN_MIN_ID, SCAN_MAX_ID)) {
        mbFailCount = 0;
        readMonitoring(t); // 1回だけ読み直し
      } else {
        mbFailCount = 0;
      }
    }
  } else {
    mbFailCount = 0;
  }

  // ---- JSON (v1.00) ----
  char json[640];
  buildJsonV100(t, json, sizeof(json));
  Serial.print("[DATA] ");
  Serial.println(json);

  // ---- LTE -> POST ----
  bool lteOk = connectLTE_once();
  bool postOk = false;

  if (lteOk) {
    postOk = httpPostJsonFixed(POST_URL, json);
    Serial.printf("[POST] %s\n", postOk ? "OK" : "NG");
  } else {
    Serial.println("[LTE] connect failed; skip POST");
  }

  shutdownLTE();

  // ---- Sleep ----
  if (lteOk && postOk && t.mb_ok) {
    goDeepSleepMs(UPLOAD_INTERVAL_MS);
  } else {
    goDeepSleepMs(RETRY_INTERVAL_MS);
  }
}
