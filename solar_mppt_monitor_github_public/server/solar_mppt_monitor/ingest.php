<?php
declare(strict_types=1);

mb_internal_encoding("UTF-8");
mb_language("uni");
date_default_timezone_set('Asia/Tokyo');

$BASE = __DIR__;
$DEVICES_FN = $BASE . "/devices.json";
$DATA_DIR = $BASE . "/data";

function jexit(int $code, array $obj): void {
  http_response_code($code);
  header("Content-Type: application/json; charset=utf-8");
  echo json_encode($obj, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
  exit;
}

function load_json(string $fn): array {
  if (!file_exists($fn)) return [];
  $s = file_get_contents($fn);
  $j = json_decode($s, true);
  return is_array($j) ? $j : [];
}

$devices = load_json($DEVICES_FN);
$deviceMap = [];
foreach (($devices["devices"] ?? []) as $d) {
  if (!empty($d["id"])) $deviceMap[$d["id"]] = $d;
}

$raw = file_get_contents("php://input");
if ($raw === false || trim($raw) === "") jexit(400, ["ok"=>false, "error"=>"empty body"]);

$payload = json_decode($raw, true);
if (!is_array($payload)) jexit(400, ["ok"=>false, "error"=>"invalid json"]);

$ver = (string)($payload["v"] ?? "");
$device = (string)($payload["device"] ?? "");
$metrics = $payload["metrics"] ?? null;

if ($ver !== "1.00") jexit(400, ["ok"=>false, "error"=>"unsupported version"]);
if ($device === "" || !isset($deviceMap[$device])) jexit(403, ["ok"=>false, "error"=>"unknown device"]);
if (!is_array($metrics)) jexit(400, ["ok"=>false, "error"=>"metrics missing"]);

$secret = (string)($payload["secret"] ?? "");
$expectedSecret = (string)($deviceMap[$device]["secret"] ?? "");
if ($expectedSecret === "" || !hash_equals($expectedSecret, $secret)) {
  jexit(403, ["ok"=>false, "error"=>"auth failed"]);
}

$now = time();
$ts = (int)($payload["ts"] ?? 0);
if ($ts <= 0 || abs($now - $ts) > 86400*7) {
  // 端末時刻が異常ならサーバ時刻を採用
  $ts = $now;
}
$seq = (int)($payload["seq"] ?? 0);

$devDir = $DATA_DIR . "/" . $device;
$logDir = $devDir . "/log";
@mkdir($logDir, 0775, true);

$dt = new DateTime("@".$ts);
$dt->setTimezone(new DateTimeZone("Asia/Tokyo"));
$ym = $dt->format("Y-m");
$csvFn = $logDir . "/" . $ym . ".csv";

// CSVヘッダは「受信したキーに追従」ではなく固定（ビュー側を安定させる）
$fields = ["ts","iso","seq","batt_v","batt_a","soc","pv_v","pv_a","pv_w","load_w","temp_c","charge_state"];
$iso = $dt->format(DateTimeInterface::ATOM);

$row = [
  "ts" => $ts,
  "iso" => $iso,
  "seq" => $seq,
  "batt_v" => $metrics["batt_v"] ?? "",
  "batt_a" => $metrics["batt_a"] ?? "",
  "soc" => $metrics["soc"] ?? "",
  "pv_v" => $metrics["pv_v"] ?? "",
  "pv_a" => $metrics["pv_a"] ?? "",
  "pv_w" => $metrics["pv_w"] ?? "",
  "load_w" => $metrics["load_w"] ?? "",
  "temp_c" => $metrics["temp_c"] ?? "",
  "charge_state" => $metrics["charge_state"] ?? ""
];

$needHeader = !file_exists($csvFn);
$fp = fopen($csvFn, "ab");
if ($fp === false) jexit(500, ["ok"=>false, "error"=>"cannot open csv"]);
if (!flock($fp, LOCK_EX)) { fclose($fp); jexit(500, ["ok"=>false, "error"=>"lock failed"]); }

if ($needHeader) {
  fputcsv($fp, $fields);
}
$line = [];
foreach ($fields as $f) $line[] = $row[$f];
fputcsv($fp, $line);

fflush($fp);
flock($fp, LOCK_UN);
fclose($fp);

// latest.json
$latest = [
  "v" => "1.00",
  "device" => $device,
  "ts" => $ts,
  "iso" => $iso,
  "seq" => $seq,
  "metrics" => $metrics,
  "server_rx_ts" => $now
];
file_put_contents($devDir . "/latest.json", json_encode($latest, JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES|JSON_PRETTY_PRINT));

// state.json（watchdog 用）
$stateFn = $devDir . "/state.json";
$state = load_json($stateFn);
$state["last_seen_ts"] = $now;
$state["last_seen_device_ts"] = $ts;
$state["last_seq"] = $seq;
$state["last_rx_ip"] = $_SERVER["REMOTE_ADDR"] ?? "";
$state["offline"] = $state["offline"] ?? false;
$state["last_alert_offline_ts"] = $state["last_alert_offline_ts"] ?? 0;
$state["last_alert_recover_ts"] = $state["last_alert_recover_ts"] ?? 0;
file_put_contents($stateFn, json_encode($state, JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES|JSON_PRETTY_PRINT));

jexit(200, ["ok"=>true, "device"=>$device, "server_ts"=>$now]);
