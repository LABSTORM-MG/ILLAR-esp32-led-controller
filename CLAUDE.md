# ILLAR – ESP32 LED Controller

This repo is the ESP32 firmware component of the **ILLAR** project (Intelligentes Lagerverwaltungssystem mit LED-Regalführung und Authentifizierung per RFID), a school project under the BEAT / Kompetenzen der Zukunft program.

## Project Goal

The ILLAR system turns a disorganized basement storage room into a smart warehouse: LEDs on shelves guide users to the right compartment, every withdrawal is logged, and users are identified automatically via RFID at the room entrance.

## Overall System Architecture

| Layer | Technology | Runs on |
|---|---|---|
| Backend | InvenTree (Docker, PostgreSQL) | Raspberry Pi |
| Frontend | Mobile-first web app | Raspberry Pi (served) |
| Middleware | Custom service, WebSocket | Raspberry Pi |
| LED control | ESP32 + WS2812B | Per shelf (3×) |
| RFID auth | ESP32 + MFRC522 | Room entrance (1×) |

All components communicate over an **isolated local WLAN** — no internet access in the storage room.

## This Repo's Role

ESP32 firmware for LED shelf guidance. There are 3 shelf ESP32s (up to 120 WS2812B LEDs each) and 1 RFID ESP32 at the entrance. The middleware on the Raspberry Pi controls LEDs in real time via WebSocket.

## LED Color Scheme

| Color | Meaning |
|---|---|
| Yellow | Withdrawal pending — item must be taken from here |
| Green | Withdrawal confirmed — item was taken |
| Blue | Inventory check — please verify stock in this compartment |
| Purple | Store / return — material must be placed here |

## Hardware

- ESP32 DevKit (4×: 3 shelf + 1 RFID)
- WS2812B 4-pin RGB LEDs (up to 120 per shelf, JST-XH connectors for hot-swap)
- MFRC522 RFID module (reads Siemens employee badges, 13.56 MHz)
- 5V/6A PSU per shelf (powers ESP32 + LEDs)
- 0.14mm² wire, 3-pin JST-XH connectors

## Team

| Person | Responsibilities |
|---|---|
| Jan | Frontend, InvenTree backend config, 3D-printed housings |
| Lennart | Frontend, LED wiring & mounting, ESP32 programming |
| Lukas | LED wiring & mounting, RFID module, electrics/PSUs |
| Yannick | Middleware service, ESP32 programming, RFID module |

**Deadline:** 2026-07-17 — documentation + short demo video.

## Related Repos

The full ILLAR system spans multiple repos:
- `ILLAR-esp32-led-controller` (this repo) — ESP32 firmware
- Frontend (web app) — separate repo
- Middleware service — separate repo
