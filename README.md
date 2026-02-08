# Solar_mppt_monitor-GateWay-
Renogy MPPT monitor with ESP32 + LTE-M
# Solar MPPT Monitor Gateway

ESP32 + SIM7080G + RS485(Modbus) により
Renogy Rover MPPT からデータを取得し
LTEでサーバー送信するゲートウェイ。

## Features
- 10min interval measurement
- Auto Modbus slave-id scan
- RTC sequence counter
- Time sync from LTE-M modem
- HTTP JSON upload

## Hardware
- ESP32 DevKit
- M5Stack SIM7080G
- RS485 U034

## Docs
- docs/flowchart.md
- docs/system_architecture.svg
