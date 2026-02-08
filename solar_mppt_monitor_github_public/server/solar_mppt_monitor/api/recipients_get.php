<?php
declare(strict_types=1);
date_default_timezone_set('Asia/Tokyo');

$BASE = dirname(__DIR__);
$ALERT_FN = $BASE . '/alert_recipients.json';

function load_json(string $fn): array {
  if (!file_exists($fn)) return [];
  $s = file_get_contents($fn);
  $j = json_decode($s, true);
  return is_array($j) ? $j : [];
}

$alert = load_json($ALERT_FN);
$emails = $alert['emails'] ?? [];
if (!is_array($emails)) $emails = [];

header('Content-Type: application/json; charset=utf-8');
echo json_encode([
  'ok' => true,
  'emails' => array_values(array_unique(array_filter($emails, fn($x)=>is_string($x) && trim($x)!=='')))
], JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES);
