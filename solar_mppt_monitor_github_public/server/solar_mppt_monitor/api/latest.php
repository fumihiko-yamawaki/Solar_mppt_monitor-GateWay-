<?php
declare(strict_types=1);

date_default_timezone_set('Asia/Tokyo');

$BASE = dirname(__DIR__);
$DEVICES_FN = $BASE . '/devices.json';
$DATA_DIR   = $BASE . '/data';
$STATE_DIR  = $BASE . '/state';

function load_json(string $fn): array {
  if (!file_exists($fn)) return [];
  $s = file_get_contents($fn);
  $j = json_decode($s, true);
  return is_array($j) ? $j : [];
}

$devicesJson = load_json($DEVICES_FN);
$devices = $devicesJson['devices'] ?? [];

$out = [];

foreach ($devices as $dev) {
  $id   = $dev['id'];
  $name = $dev['name'] ?? $id;

  // latest.json
  $latestFn = "{$DATA_DIR}/{$id}/latest.json";
  $latest = file_exists($latestFn)
    ? json_decode(file_get_contents($latestFn), true)
    : null;

  // state.json（watchdog が書く）
  $stateFn = "{$STATE_DIR}/{$id}.json";
  $state = file_exists($stateFn)
    ? json_decode(file_get_contents($stateFn), true)
    : null;

  $out[] = [
    'id'     => $id,
    'name'   => $name,
    'latest' => $latest,
    'state'  => $state,
  ];
}

header('Content-Type: application/json; charset=utf-8');
echo json_encode([
  'ok' => true,
  'items' => $out
], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
