<?php
declare(strict_types=1);
date_default_timezone_set('Asia/Tokyo');

$BASE = dirname(__DIR__);
$DATA_DIR = $BASE . '/data';

function bad(string $msg, int $code=400): void {
  http_response_code($code);
  header('Content-Type: text/plain; charset=utf-8');
  echo $msg;
  exit;
}

$device = preg_replace('/[^A-Z0-9_\-]/', '', (string)($_GET['device'] ?? ''));
$from = (string)($_GET['from'] ?? '');
$to   = (string)($_GET['to'] ?? '');

if ($device === '') bad('device required');
if (!preg_match('/^\d{4}-\d{2}-\d{2}$/', $from)) bad('from must be YYYY-MM-DD');
if (!preg_match('/^\d{4}-\d{2}-\d{2}$/', $to)) bad('to must be YYYY-MM-DD');

$fromTs = strtotime($from . ' 00:00:00');
$toTs   = strtotime($to . ' 23:59:59');
if ($fromTs === false || $toTs === false) bad('invalid date');
if ($toTs < $fromTs) bad('to must be >= from');

// サーバ負荷対策：最大93日
if (($toTs - $fromTs) > 93 * 86400) bad('range too large (max 93 days)');

// 対象月リスト
$months = [];
$cur = new DateTimeImmutable($from . ' 00:00:00', new DateTimeZone('Asia/Tokyo'));
$end = new DateTimeImmutable($to . ' 00:00:00', new DateTimeZone('Asia/Tokyo'));
while ($cur <= $end) {
  $months[] = $cur->format('Y-m');
  $cur = $cur->modify('first day of next month');
}
$months = array_values(array_unique($months));

$files = [];
foreach ($months as $ym) {
  $fn = "{$DATA_DIR}/{$device}/log/{$ym}.csv";
  if (file_exists($fn)) $files[] = $fn;
}
if (count($files) === 0) bad('no data', 404);

// 出力（Excel対策でUTF-8 BOM）
$filename = "{$device}_{$from}_to_{$to}.csv";
header('Content-Type: text/csv; charset=utf-8');
header('Content-Disposition: attachment; filename="'.$filename.'"');

echo "\xEF\xBB\xBF"; // BOM

$headerWritten = false;

foreach ($files as $fn) {
  $fp = fopen($fn, 'rb');
  if (!$fp) continue;

  $header = fgetcsv($fp);
  if (!$header || !in_array('ts', $header, true)) { fclose($fp); continue; }

  $tsIdx = array_search('ts', $header, true);

  if (!$headerWritten) {
    echo implode(',', $header) . "\n";
    $headerWritten = true;
  }

  while (($row = fgetcsv($fp)) !== false) {
    if (!isset($row[$tsIdx])) continue;
    $ts = (int)$row[$tsIdx];
    if ($ts < $fromTs || $ts > $toTs) continue;
    echo implode(',', $row) . "\n";
  }
  fclose($fp);
}
