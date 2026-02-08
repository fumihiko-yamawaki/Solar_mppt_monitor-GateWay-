<?php
declare(strict_types=1);
date_default_timezone_set('Asia/Tokyo');

$BASE = dirname(__DIR__);
$DATA_DIR = $BASE . "/data";

$device = preg_replace('/[^A-Z0-9_\-]/', '', (string)($_GET["device"] ?? ""));
$ym = preg_replace('/[^0-9\-]/', '', (string)($_GET["ym"] ?? date("Y-m"))); // 例: 2026-02

if ($device === "") { http_response_code(400); exit("device required"); }

$csvFn = $DATA_DIR . "/{$device}/log/{$ym}.csv";
if (!file_exists($csvFn)) { http_response_code(404); exit("not found"); }

header("Content-Type: text/csv; charset=utf-8");
readfile($csvFn);
