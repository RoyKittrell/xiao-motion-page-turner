# Changelog

## 1.13 - 2026-09-29

- Replaced orientation-specific table detection with motion inactivity parking.
- Parks after five continuous minutes without meaningful movement.
- Requires the trained three-impulse shake to wake after parking.

## 1.12 - 2026-09-29

- Added persistent face-down Bluetooth handoff mode.
- Repeatedly refuses the previous iPad while accepting another paired host.
- Keeps handoff active while face-down and for 60 seconds after returning upright.

## 1.9

- Added Nordic BLE UART diagnostics alongside USB Serial.

## 1.8

- Added trained three-impulse shake-to-wake recognition.
- Added three large blue wake-confirmation flashes.
- Tuned against recorded deliberate shakes and pocket walking.

## 1.7

- Added two persistent Bluetooth host slots with oldest-bond eviction.
- Added face-down handoff gesture.

## 1.6

- Added stable battery-voltage diagnostics and charging indication.
- Collected user-facing configuration at the top of the sketch.

## 1.2

- Added reversed, paper-like page-turn direction.
- Extended active reading sessions to five minutes.
