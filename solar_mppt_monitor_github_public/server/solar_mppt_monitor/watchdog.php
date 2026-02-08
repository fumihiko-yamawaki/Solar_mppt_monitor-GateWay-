<?php
declare(strict_types=1);

mb_internal_encoding("UTF-8");
mb_language("uni");
date_default_timezone_set('Asia/Tokyo');

$BASE = __DIR__;
$DEVICES_FN = $BASE . '/devices.json';
$ALERT_FN   = $BASE . '/alert_recipients.json';
$DATA_DIR   = $BASE . '/data';
$STATE_DIR  = $BASE . '/state';
$LOG_DIR    = $BASE . '/logs';

@mkdir($STATE_DIR, 0755, true);
@mkdir($LOG_DIR, 0755, true);

function load_json(string $fn): array {
  if (!file_exists($fn)) return [];
  $s = @file_get_contents($fn);
  $j = json_decode($s ?: '', true);
  return is_array($j) ? $j : [];
}

function save_json(string $fn, array $obj): void {
  $tmp = $fn . '.tmp';
  file_put_contents($tmp, json_encode($obj, JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES|JSON_PRETTY_PRINT));
  rename($tmp, $fn);
}

function log_line(string $msg): void {
  global $LOG_DIR;
  $fn = $LOG_DIR . '/watchdog_' . date('Ymd') . '.log';
  file_put_contents($fn, date('c') . " " . $msg . "\n", FILE_APPEND);
}

function get_last_seen_ts(?array $latest): int {
  if (!$latest) return 0;
  if (isset($latest['ts']) && is_numeric($latest['ts'])) return (int)$latest['ts'];
  if (isset($latest['iso'])) {
    $t = strtotime((string)$latest['iso']);
    return $t ? (int)$t : 0;
  }
  return 0;
}

function send_alert_mail(array $recips, string $subject, string $body, string $fromName, string $fromMail): bool {
  $to = implode(',', $recips);

  $encodedFromName = mb_encode_mimeheader($fromName, 'UTF-8');
  $headers = [];
  $headers[] = "From: {$encodedFromName} <{$fromMail}>";
  $headers[] = "Reply-To: {$fromMail}";
  $headers[] = "Content-Type: text/plain; charset=UTF-8";

  return mb_send_mail($to, $subject, $body, implode("\r\n", $headers));
}

$devicesJson = load_json($DEVICES_FN);
$devices = $devicesJson['devices'] ?? [];

$alertJson = load_json($ALERT_FN);
$recips = array_values(array_unique(array_filter(
  $alertJson['emails'] ?? [],
  fn($x)=>is_string($x) && trim($x)!==''
)));

$fromName = $alertJson['from_name'] ?? 'Solar MPPT Monitor';
$fromMail = $alertJson['from_mail'] ?? 'no-reply@example.com';

$now = time();
$sentCount = 0;

foreach ($devices as $dev) {
  $id   = (string)($dev['id'] ?? '');
  if ($id === '') continue;

  $name = (string)($dev['name'] ?? $id);

  $latestFn = "{$DATA_DIR}/{$id}/latest.json";

  // ★ 一度も通信していない端末は監視対象外
  if (!file_exists($latestFn)) {
    log_line("SKIP {$id} : latest.json not found");
    continue;
  }

  $latest = load_json($latestFn);

  $offlineGrace = max(60, (int)($dev['offline_grace_sec'] ?? 900));

  $lastSeen = get_last_seen_ts($latest);
  if ($lastSeen <= 0) continue;

  $age = $now - $lastSeen;
  $offline = ($age > $offlineGrace);

  $stateFn = "{$STATE_DIR}/{$id}.json";
  $state = file_exists($stateFn) ? load_json($stateFn) : [];

  $prevOffline = (bool)($state['offline'] ?? false);

  $state = [
    'id' => $id,
    'name' => $name,
    'last_seen_ts' => $lastSeen,
    'age_sec' => $age,
    'offline_grace_sec' => $offlineGrace,
    'offline' => $offline,
    'updated_ts' => $now,
  ];

  $transition = null;
  if ($prevOffline === false && $offline === true) $transition = 'OFFLINE';
  if ($prevOffline === true  && $offline === false) $transition = 'RECOVER';

  if ($transition && count($recips)) {

    if ($transition === 'OFFLINE') {
      $subject = "【太陽光監視】通信断を検知：{$id} {$name}";
      $body =
"以下の設備で通信断を検知しました。

ID: {$id}
名称: {$name}
最終受信: ".date('Y-m-d H:i:s', $lastSeen)."
経過時間: {$age} 秒
判定猶予: {$offlineGrace} 秒

時刻: ".date('Y-m-d H:i:s', $now)."
";
    } else {
      $subject = "【太陽光監視】復旧を確認：{$id} {$name}";
      $body =
"以下の設備で通信復旧を確認しました。

ID: {$id}
名称: {$name}
最終受信: ".date('Y-m-d H:i:s', $lastSeen)."

時刻: ".date('Y-m-d H:i:s', $now)."
";
    }

    $ok = send_alert_mail($recips, $subject, $body, $fromName, $fromMail);

    $state['last_alert_ts'] = $now;
    $state['last_alert_type'] = $transition;
    $state['last_alert_ok'] = $ok;

    log_line("MAIL {$transition} {$id} ok=" . ($ok ? "1":"0"));
    $sentCount++;
  }

  save_json($stateFn, $state);
}

log_line("DONE sent={$sentCount}");
echo "OK sent={$sentCount}\n";
