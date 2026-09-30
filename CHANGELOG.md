# Changelog

## 1.19 - 2026-09-30

- Added genuine nRF52840 System OFF after five motionless minutes.
- Added LSM6DS3TR-C INT1 hardware motion wake on XIAO pin D18/P0.11.
- Keeps Bluetooth suppressed until the trained shake confirms wake-up.
- Returns incomplete wake attempts to deep sleep after eight seconds.
- Restores normal IMU operation explicitly after a System OFF reset.
- Fixed false battery-only charging detection from the open-drain CHG pin.
- Forces all active-low LEDs off immediately before entering System OFF.

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
