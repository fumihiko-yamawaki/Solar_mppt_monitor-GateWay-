<?php
declare(strict_types=1);
date_default_timezone_set('Asia/Tokyo');

$BASE = dirname(__DIR__);
$ALERT_FN = $BASE . '/alert_recipients.json';

function jexit(int $code, array $obj): void {
  http_response_code($code);
  header('Content-Type: application/json; charset=utf-8');
  echo json_encode($obj, JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES);
  exit;
}
function load_json(string $fn): array {
  if (!file_exists($fn)) return [];
  $s = file_get_contents($fn);
  $j = json_decode($s, true);
  return is_array($j) ? $j : [];
}

if (($_SERVER['REQUEST_METHOD'] ?? 'GET') !== 'POST') {
  jexit(405, ['ok'=>false, 'error'=>'POST only']);
}

/*
  ★重要（最低限の保護）
  このAPIは「通知先メールを書き換え」できてしまうので、
  viewer/ あるいは api/recipients_set.php に Basic認証をかけるのを推奨。
  （.htaccess例はこの回答の最後に載せます）
*/

$raw = file_get_contents('php://input');
$payload = json_decode($raw ?: '', true);
if (!is_array($payload)) jexit(400, ['ok'=>false, 'error'=>'invalid json']);

$emails = $payload['emails'] ?? null;
if (!is_array($emails)) jexit(400, ['ok'=>false, 'error'=>'emails must be array']);

$clean = [];
foreach ($emails as $e) {
  if (!is_string($e)) continue;
  $e = trim($e);
  if ($e === '') continue;
  if (!filter_var($e, FILTER_VALIDATE_EMAIL)) continue;
  $clean[] = $e;
}
$clean = array_values(array_unique($clean));

$alert = load_json($ALERT_FN);
$alert['emails'] = $clean;
if (!isset($alert['from_name'])) $alert['from_name'] = 'Solar MPPT Monitor';
if (!isset($alert['from_mail'])) $alert['from_mail'] = 'no-reply@example.com';

if (file_put_contents($ALERT_FN, json_encode($alert, JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES|JSON_PRETTY_PRINT)) === false) {
  jexit(500, ['ok'=>false, 'error'=>'write failed']);
}

jexit(200, ['ok'=>true, 'emails'=>$clean]);
