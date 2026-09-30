// Page Turner V1.19 - guarded deep sleep with retained LEDs forced off
#include <bluefruit.h>
#include <LSM6DS3.h>
#include <Wire.h>
#include <nrf_gpio.h>
#include <InternalFileSystem.h>
#include <utility/bonding.h>

using namespace Adafruit_LittleFS_Namespace;

// ============================================================================
// USER SETTINGS
// Change values in this section to tune the remote. The defaults reproduce
// Roy's preferred V1.2 reversed behavior.
// ============================================================================

// Name shown in the iPad Bluetooth menu.
constexpr char BLUETOOTH_NAME[] = "Roy's Page Turner";

// true: Roy's natural book-like motion (right-to-left advances the page).
// false: use the original gesture-to-arrow mapping.
constexpr bool REVERSE_PAGE_DIRECTION = true;

// Park after this many continuous minutes without meaningful movement.
// Picking it up, shifting it, or turning a page restarts the timer. Once
// parked, page gestures remain disabled until the deliberate shake-to-wake.
constexpr uint32_t MOTION_INACTIVITY_MINUTES = 5;

// After an IMU wake-up, allow this long for the deliberate three-shake
// pattern. Random movement that does not complete the pattern returns the
// remote to true deep sleep without reconnecting permanently.
constexpr uint32_t WAKE_QUALIFICATION_SECONDS = 8;
constexpr uint32_t POST_WAKE_SENSOR_GUARD_MS = 250;

// Movement must cross either threshold to restart the inactivity timer.
// These are deliberately above normal stationary IMU noise.
constexpr float ACTIVITY_GYRO_THRESHOLD_DPS = 12.0f;
constexpr float ACTIVITY_ACCEL_DELTA_G = 0.12f;

// After the wake flashes, motion is ignored until calmly held for this long.
constexpr uint32_t PICKUP_SETTLE_MS = 400;
// Never remain indefinitely in the solid-blue settling state.
constexpr uint32_t PICKUP_SETTLE_TIMEOUT_MS = 3000;

// Shake-to-wake values trained from Roy's five shakes and pocket-walking log.
// Three rapid impacts are required; isolated walking impacts are ignored.
constexpr uint8_t WAKE_IMPULSE_COUNT = 3;
constexpr float WAKE_IMPULSE_THRESHOLD_G = 3.5f;
constexpr float WAKE_IMPULSE_RELEASE_G = 2.0f;
constexpr float WAKE_GYRO_CONFIRM_DPS = 500.0f;
constexpr uint32_t WAKE_IMPULSE_WINDOW_MS = 1200;
constexpr uint32_t WAKE_IMPULSE_REFRACTORY_MS = 120;

// A successful shake produces three large blue flashes: "Hey Boss! I'm awake!"
constexpr uint8_t WAKE_FLASH_COUNT = 3;
constexpr uint32_t WAKE_FLASH_ON_MS = 300;
constexpr uint32_t WAKE_FLASH_OFF_MS = 180;

// Main gesture sensitivity in degrees/second. Lower is more sensitive and may
// cause accidental turns; higher requires a sharper gesture.
constexpr float TURN_THRESHOLD_DPS = 400.0f;

// Automatically make tiny, session-only adjustments from clean gestures.
// All learned adjustments reset whenever the remote parks.
constexpr bool ENABLE_INCREMENTAL_LEARNING = true;

// Treat two matching near-misses close together as an intentional retry.
constexpr bool ENABLE_RETRY_LEARNING = true;
constexpr float RETRY_CANDIDATE_DPS = 300.0f;
constexpr uint32_t RETRY_WINDOW_MS = 1400;

// LED preferences. Settling is solid blue; READY remains dark after wake.
constexpr bool SHOW_SETTLING_LED = false;
constexpr uint8_t ARMED_FLASH_COUNT = 0;
constexpr uint32_t ARMED_FLASH_ON_MS = 150;
constexpr uint32_t ARMED_FLASH_OFF_MS = 150;

// Slowly blink blue while the onboard charger reports active charging.
// This overrides the other LED patterns until charging finishes.
constexpr bool SHOW_CHARGING_LED = true;
constexpr uint32_t CHARGING_LED_ON_MS = 1000;
constexpr uint32_t CHARGING_LED_OFF_MS = 1000;

// Battery information is printed to Serial Monitor at 115200 baud.
constexpr bool SHOW_BATTERY_SERIAL_STATUS = true;
constexpr uint32_t BATTERY_STATUS_INTERVAL_SECONDS = 5;

// Hold the remote face-down to leave the current iPad and connect to another
// previously paired iPad. The old iPad is ignored briefly so it cannot grab
// the connection straight back. Pairing a third host forgets the oldest host.
constexpr bool ENABLE_FACE_DOWN_HANDOFF = true;
constexpr uint32_t FACE_DOWN_HOLD_MS = 3000;
constexpr uint32_t FACE_DOWN_GRACE_MS = 400;
constexpr uint32_t HANDOFF_WINDOW_SECONDS = 60;
constexpr uint8_t REMEMBERED_HOST_COUNT = 2;

// Set true only when diagnosing gestures; it prints several lines per second.
constexpr bool SHOW_MOTION_SERIAL_STATUS = false;

// ============================================================================
// ADVANCED TUNING
// These values were derived from Roy's recorded gestures. Most users should
// leave them alone unless diagnosing a particular motion or mounting angle.
// ============================================================================

constexpr float QUIET_THRESHOLD_DPS = 40.0f;
constexpr uint32_t QUIET_TO_ARM_MS = 250;
constexpr float HELD_X_MIN_G = 0.15f;
constexpr float SETTLE_GYRO_MAX_DPS = 8.0f;
constexpr float LEARNING_RATE = 0.02f;
constexpr float MIN_TURN_THRESHOLD_DPS = 350.0f;
constexpr float MAX_TURN_THRESHOLD_DPS = 450.0f;
constexpr uint32_t WEAK_GESTURE_END_MS = 100;
constexpr float RETRY_THRESHOLD_STEP_DPS = 6.0f;
constexpr uint32_t CHARGE_STATUS_GPIO = 17;  // Raw Nordic pin P0.17.
constexpr uint32_t LED_RED_GPIO = 26;        // Raw Nordic pin P0.26.
constexpr uint32_t LED_BLUE_GPIO = 6;        // Raw Nordic pin P0.06.
constexpr uint32_t LED_GREEN_GPIO = 30;      // Raw Nordic pin P0.30.
constexpr float BATTERY_DIVIDER_RATIO = 2.9608f;  // 1 MOhm / 510 kOhm divider.
constexpr float FACE_DOWN_Z_MAX_G = -0.65f;
constexpr float FACE_DOWN_GYRO_MAX_DPS = 80.0f;
constexpr uint8_t IMU_WAKE_THRESHOLD = 8;  // 8/64 of 2 g = about 0.25 g.
constexpr uint8_t IMU_WAKE_DURATION = 0x00;  // Validated: first qualifying sample.

// Derived timing values. Do not edit these; use the settings above.
constexpr uint32_t MOTION_INACTIVITY_MS =
    MOTION_INACTIVITY_MINUTES * 60UL * 1000UL;
constexpr uint32_t BATTERY_STATUS_INTERVAL_MS =
    BATTERY_STATUS_INTERVAL_SECONDS * 1000UL;
constexpr uint32_t HANDOFF_WINDOW_MS = HANDOFF_WINDOW_SECONDS * 1000UL;
constexpr uint32_t WAKE_QUALIFICATION_MS =
    WAKE_QUALIFICATION_SECONDS * 1000UL;
constexpr uint32_t WAKE_FLASH_CYCLE_MS =
    WAKE_FLASH_ON_MS + WAKE_FLASH_OFF_MS;
constexpr uint32_t WAKE_FLASH_TOTAL_MS =
    WAKE_FLASH_COUNT * WAKE_FLASH_CYCLE_MS;
constexpr char HOST_SLOTS_FILE[] = "/page_turner_hosts.bin";
constexpr uint32_t HOST_SLOTS_MAGIC = 0x50544731UL;

LSM6DS3 imu(I2C_MODE, 0x6A);
BLEDis deviceInfo;
BLEHidAdafruit keyboard;
BLEUart bleUart;
bool bluetoothReady = false;

class DiagnosticOutput : public Print {
 public:
  size_t write(uint8_t value) override {
    Serial.write(value);
    if (bluetoothReady && Bluefruit.connected()) {
      bleUart.write(value);
    }
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    Serial.write(buffer, size);
    if (bluetoothReady && Bluefruit.connected()) {
      bleUart.write(buffer, size);
    }
    return size;
  }
};

DiagnosticOutput diagnostics;

enum class RemoteState {
  PARKED,
  WAKE_FLASHING,
  PICKUP_SETTLING,
  READY
};

enum class LearningDirection {
  NONE,
  NEXT,
  PREVIOUS
};

struct HostSlots {
  uint32_t magic;
  uint8_t count;
  ble_gap_addr_t hosts[REMEMBERED_HOST_COUNT];
};

// Explicit declarations keep Arduino's automatic prototype generator from
// placing enum-dependent prototypes before LearningDirection is defined.
void sendArrow(uint8_t key, const char *label);
void sendPageForGesture(LearningDirection direction, bool retryConfirmed);
void onBleConnect(uint16_t connHandle);
void onBleSecured(uint16_t connHandle);

bool gestureArmed = false;
uint32_t quietSince = 0;
uint32_t pickupQuietSince = 0;
uint32_t pickupSettlingStartedAt = 0;
uint32_t lastSettlingDiagnosticAt = 0;
uint32_t readyFlashStartedAt = 0;
uint32_t lastMeaningfulMotionAt = 0;
uint32_t lastPrintAt = 0;
uint32_t lastBatteryPrintAt = 0;
uint32_t faceDownSince = 0;
uint32_t lastFaceDownAt = 0;
uint32_t handoffUntil = 0;
uint32_t handoffFeedbackUntil = 0;
uint32_t wakeWindowStartedAt = 0;
uint32_t lastWakeImpulseAt = 0;
uint32_t wakeFlashStartedAt = 0;
uint32_t parkedAwakeStartedAt = 0;
uint32_t wakeDetectionNotBefore = 0;
uint8_t wakeImpulseCount = 0;
float wakePeakGyroDps = 0.0f;
bool wakeImpulseReady = true;
bool blockedHostValid = false;
bool handoffDisconnectIssued = false;
bool faceDownLatched = false;
bool wokeFromSystemOff = false;
bool bluetoothSuppressedForWake = false;
ble_gap_addr_t blockedHost = {};
HostSlots hostSlots = {};
RemoteState remoteState = RemoteState::PARKED;
LearningDirection learningDirection = LearningDirection::NONE;

// Initial values are the means of the clean peaks in the first recording.
float expectedNextPeakDps = 750.0f;
float expectedPreviousPeakDps = 1000.0f;
float nextTurnThresholdDps = TURN_THRESHOLD_DPS;
float previousTurnThresholdDps = TURN_THRESHOLD_DPS;
float gesturePeakDps = 0.0f;
float oppositePeakDps = 0.0f;
uint32_t learnedGestureCount = 0;
LearningDirection weakDirection = LearningDirection::NONE;
float weakPeakDps = 0.0f;
uint32_t weakQuietSince = 0;
LearningDirection pendingRetryDirection = LearningDirection::NONE;
float pendingRetryPeakDps = 0.0f;
uint32_t pendingRetryAt = 0;

void setRgbLed(bool red, bool green, bool blue) {
  // The onboard RGB LED is active-low.
  digitalWrite(LED_RED, red ? LOW : HIGH);
  digitalWrite(LED_GREEN, green ? LOW : HIGH);
  digitalWrite(LED_BLUE, blue ? LOW : HIGH);
}

bool sameAddress(const ble_gap_addr_t &left, const ble_gap_addr_t &right) {
  return left.addr_type == right.addr_type &&
         memcmp(left.addr, right.addr, sizeof(left.addr)) == 0;
}

void printAddress(const ble_gap_addr_t &address) {
  for (int8_t i = 5; i >= 0; i--) {
    if (address.addr[i] < 0x10) {
      diagnostics.print('0');
    }
    diagnostics.print(address.addr[i], HEX);
    if (i > 0) {
      diagnostics.print(':');
    }
  }
}

void saveHostSlots() {
  InternalFS.remove(HOST_SLOTS_FILE);
  File file(HOST_SLOTS_FILE, FILE_O_WRITE, InternalFS);
  if (!file) {
    diagnostics.println("Could not save Bluetooth host slots");
    return;
  }
  file.write(reinterpret_cast<uint8_t *>(&hostSlots), sizeof(hostSlots));
  file.close();
}

void loadHostSlots() {
  hostSlots = {};
  InternalFS.begin();
  File file(HOST_SLOTS_FILE, FILE_O_READ, InternalFS);
  if (file) {
    const size_t bytesRead =
        file.read(reinterpret_cast<uint8_t *>(&hostSlots), sizeof(hostSlots));
    file.close();
    if (bytesRead != sizeof(hostSlots) ||
        hostSlots.magic != HOST_SLOTS_MAGIC ||
        hostSlots.count > REMEMBERED_HOST_COUNT) {
      hostSlots = {};
    }
  }
  hostSlots.magic = HOST_SLOTS_MAGIC;
}

void rememberHost(const ble_gap_addr_t &address) {
  for (uint8_t i = 0; i < hostSlots.count; i++) {
    if (sameAddress(hostSlots.hosts[i], address)) {
      diagnostics.print("Known Bluetooth host in slot ");
      diagnostics.println(i + 1);
      return;
    }
  }

  if (hostSlots.count == REMEMBERED_HOST_COUNT) {
    diagnostics.print("Host slots full; forgetting oldest host ");
    printAddress(hostSlots.hosts[0]);
    diagnostics.println();
    bond_remove_key(BLE_GAP_ROLE_PERIPH, &hostSlots.hosts[0]);
    for (uint8_t i = 1; i < REMEMBERED_HOST_COUNT; i++) {
      hostSlots.hosts[i - 1] = hostSlots.hosts[i];
    }
    hostSlots.count--;
  }

  hostSlots.hosts[hostSlots.count++] = address;
  saveHostSlots();
  diagnostics.print("Remembered Bluetooth host in slot ");
  diagnostics.println(hostSlots.count);
}

void onBleConnect(uint16_t connHandle) {
  BLEConnection *connection = Bluefruit.Connection(connHandle);
  if (connection == nullptr) {
    return;
  }
  handoffDisconnectIssued = false;

  const ble_gap_addr_t peer = connection->getPeerAddr();
  const bool handoffActive = blockedHostValid &&
                             static_cast<int32_t>(handoffUntil - millis()) > 0;
  if (handoffActive && sameAddress(peer, blockedHost)) {
    diagnostics.println("Handoff: declining the previous iPad");
    handoffDisconnectIssued = true;
    connection->disconnect();
    return;
  }

  if (handoffActive) {
    // Bonded Apple devices commonly reconnect with a resolvable private
    // address. Preserve the block until security resolves the real identity.
    diagnostics.println("Handoff: connection candidate awaiting identity");
    return;
  }

  blockedHostValid = false;
  handoffUntil = 0;
  diagnostics.print("Bluetooth connected: ");
  printAddress(peer);
  diagnostics.println();
}

void onBleSecured(uint16_t connHandle) {
  BLEConnection *connection = Bluefruit.Connection(connHandle);
  if (connection == nullptr) {
    return;
  }

  const ble_gap_addr_t peer = connection->getPeerAddr();
  const bool handoffActive = blockedHostValid &&
                             static_cast<int32_t>(handoffUntil - millis()) > 0;
  // A bonded iPad may reconnect with a temporary private address. Once the
  // secure link resolves its identity, check again before accepting it.
  if (handoffActive && sameAddress(peer, blockedHost)) {
    diagnostics.println("Handoff: secured identity is the previous iPad; declining");
    handoffDisconnectIssued = true;
    connection->disconnect();
    return;
  }

  if (handoffActive) {
    diagnostics.println("Handoff: different iPad accepted");
    blockedHostValid = false;
    handoffUntil = 0;
  }

  if (connection->bonded()) {
    rememberHost(peer);
  }
}

void beginFaceDownHandoff(uint32_t now) {
  faceDownSince = 0;
  handoffFeedbackUntil = now + 1200;
  keyboard.keyRelease();

  if (!Bluefruit.connected()) {
    diagnostics.println("Handoff requested; already advertising");
    return;
  }

  BLEConnection *connection = Bluefruit.Connection(Bluefruit.connHandle());
  if (connection == nullptr) {
    return;
  }

  blockedHost = connection->getPeerAddr();
  blockedHostValid = true;
  handoffDisconnectIssued = true;
  handoffUntil = now + HANDOFF_WINDOW_MS;
  diagnostics.print("Handoff: releasing ");
  printAddress(blockedHost);
  diagnostics.println(" and waiting for the other iPad");
  connection->disconnect();
}

void enforceHandoffConnection(uint32_t now) {
  const bool handoffActive = blockedHostValid &&
                             static_cast<int32_t>(handoffUntil - now) > 0;
  if (!handoffActive || !Bluefruit.connected() || handoffDisconnectIssued) {
    return;
  }

  BLEConnection *connection = Bluefruit.Connection(Bluefruit.connHandle());
  if (connection == nullptr || !connection->secured()) {
    return;
  }

  const ble_gap_addr_t peer = connection->getPeerAddr();
  if (sameAddress(peer, blockedHost)) {
    diagnostics.println("Handoff monitor: old iPad reconnected; disconnecting again");
    handoffDisconnectIssued = true;
    connection->disconnect();
  } else {
    diagnostics.println("Handoff monitor: new iPad accepted");
    blockedHostValid = false;
    handoffUntil = 0;
    rememberHost(peer);
  }
}

void resetWakeDetector() {
  wakeWindowStartedAt = 0;
  lastWakeImpulseAt = 0;
  wakeImpulseCount = 0;
  wakePeakGyroDps = 0.0f;
  wakeImpulseReady = true;
}

bool updateShakeToWake(float accelMagnitude, float gyroMagnitude,
                       uint32_t now) {
  if (wakeImpulseCount > 0 &&
      now - wakeWindowStartedAt > WAKE_IMPULSE_WINDOW_MS) {
    resetWakeDetector();
  }

  if (wakeImpulseCount > 0) {
    wakePeakGyroDps = max(wakePeakGyroDps, gyroMagnitude);
  }

  if (accelMagnitude <= WAKE_IMPULSE_RELEASE_G) {
    wakeImpulseReady = true;
  }

  const bool refractoryComplete = wakeImpulseCount == 0 ||
                                  now - lastWakeImpulseAt >=
                                      WAKE_IMPULSE_REFRACTORY_MS;
  if (wakeImpulseReady && refractoryComplete &&
      accelMagnitude >= WAKE_IMPULSE_THRESHOLD_G) {
    if (wakeImpulseCount == 0) {
      wakeWindowStartedAt = now;
      wakePeakGyroDps = gyroMagnitude;
    }
    wakeImpulseCount++;
    lastWakeImpulseAt = now;
    wakeImpulseReady = false;

    diagnostics.print("Wake impulse ");
    diagnostics.print(wakeImpulseCount);
    diagnostics.print('/');
    diagnostics.println(WAKE_IMPULSE_COUNT);
  }

  if (wakeImpulseCount >= WAKE_IMPULSE_COUNT &&
      wakePeakGyroDps >= WAKE_GYRO_CONFIRM_DPS) {
    resetWakeDetector();
    return true;
  }

  return false;
}

bool isBatteryCharging() {
  // The BQ25101 CHG output is active-low. Raw GPIO access is required because
  // Arduino pin 17 is not the same thing as Nordic pin P0.17 on this board.
  // CHG is open-drain and can float LOW on battery power, so it is meaningful
  // only while USB VBUS is actually present.
  const bool usbPowered =
      (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
  const bool chargePinLow =
      (NRF_P0->IN & (1UL << CHARGE_STATUS_GPIO)) == 0;
  return usbPowered && chargePinLow;
}

bool isUsbPowered() {
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

void enterImuDeepSleep() {
  if (isUsbPowered() || isBatteryCharging()) {
    return;
  }

  diagnostics.println("Deep sleep: IMU interrupt armed");
  delay(30);
  Bluefruit.Advertising.stop();
  if (Bluefruit.connected()) {
    Bluefruit.disconnect(Bluefruit.connHandle());
    delay(80);
  }

  // Leave only the accelerometer running at 26 Hz in low-power mode. Its
  // wake-up detector drives INT1, physically routed to nRF52840 P0.11 (D18).
  imu.writeRegister(LSM6DS3_ACC_GYRO_MD1_CFG, 0x00);
  imu.writeRegister(LSM6DS3_ACC_GYRO_CTRL2_G, 0x00);
  imu.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, 0x20);
  imu.writeRegister(LSM6DS3_ACC_GYRO_WAKE_UP_THS, IMU_WAKE_THRESHOLD);
  imu.writeRegister(LSM6DS3_ACC_GYRO_WAKE_UP_DUR, IMU_WAKE_DURATION);
  // LSM6DS3TR-C bit 7 globally enables basic interrupts. V1.14 omitted this
  // bit, so INT1 never asserted; the awake diagnostic validated 0x81.
  imu.writeRegister(LSM6DS3_ACC_GYRO_TAP_CFG1, 0x81);  // Enable + latch INT1.
  imu.writeRegister(LSM6DS3_ACC_GYRO_MD1_CFG, 0x20);   // Wake-up on INT1.

  uint8_t wakeSource = 0;
  imu.readRegister(&wakeSource, LSM6DS3_ACC_GYRO_WAKE_UP_SRC);
  delay(20);

  // Disable the battery measurement divider before System OFF. systemOff()
  // configures D18 as a high-level GPIO wake source and does not return.
  digitalWrite(VBAT_ENABLE, HIGH);

  // System OFF retains GPIO state. Force each active-low LED output HIGH as
  // the final operation so disconnect callbacks and peripheral shutdown cannot
  // leave the blue LED illuminated for the entire sleep period.
  nrf_gpio_cfg_output(LED_RED_GPIO);
  nrf_gpio_cfg_output(LED_BLUE_GPIO);
  nrf_gpio_cfg_output(LED_GREEN_GPIO);
  nrf_gpio_pin_set(LED_RED_GPIO);
  nrf_gpio_pin_set(LED_BLUE_GPIO);
  nrf_gpio_pin_set(LED_GREEN_GPIO);
  systemOff(PIN_LSM6DS3TR_C_INT1, HIGH);
}

void restoreActiveImuMode() {
  // The LSM6DS3 remains powered while the nRF52840 is in System OFF, so its
  // wake routing and latched source survive the processor reset. Disable that
  // routing explicitly after imu.begin() restores normal accel/gyro sampling.
  imu.writeRegister(LSM6DS3_ACC_GYRO_MD1_CFG, 0x00);
  imu.writeRegister(LSM6DS3_ACC_GYRO_TAP_CFG1, 0x00);
  uint8_t staleWakeSource = 0;
  imu.readRegister(&staleWakeSource, LSM6DS3_ACC_GYRO_WAKE_UP_SRC);

  // Remove the GPIO level-sense configuration installed by systemOff().
  pinMode(PIN_LSM6DS3TR_C_INT1, INPUT_PULLDOWN);
}

float readBatteryVoltage() {
  // Keep the divider enabled (active-low) while charging and measuring.
  digitalWrite(VBAT_ENABLE, LOW);
  delay(10);

  // Discard initial conversions while the high-impedance divider settles.
  for (uint8_t i = 0; i < 3; i++) {
    analogRead(PIN_VBAT);
    delay(2);
  }

  constexpr uint8_t SAMPLE_COUNT = 9;
  uint16_t samples[SAMPLE_COUNT];
  for (uint8_t i = 0; i < SAMPLE_COUNT; i++) {
    samples[i] = analogRead(PIN_VBAT);
    delay(2);
  }

  // A median rejects occasional USB/ADC spikes better than a simple average.
  for (uint8_t i = 1; i < SAMPLE_COUNT; i++) {
    const uint16_t value = samples[i];
    int8_t j = i - 1;
    while (j >= 0 && samples[j] > value) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = value;
  }

  const float median = samples[SAMPLE_COUNT / 2];
  return median * (3.6f / 4095.0f) * BATTERY_DIVIDER_RATIO;
}

uint8_t estimateBatteryPercent(float voltage) {
  // Approximate resting-voltage curve for a single-cell LiPo. Charging and
  // recent activity can temporarily make this estimate read higher.
  struct BatteryPoint {
    float voltage;
    uint8_t percent;
  };

  static const BatteryPoint curve[] = {
      {3.30f, 0},  {3.50f, 10}, {3.60f, 20}, {3.68f, 30},
      {3.74f, 40}, {3.79f, 50}, {3.85f, 60}, {3.92f, 70},
      {4.00f, 80}, {4.10f, 90}, {4.20f, 100},
  };

  if (voltage <= curve[0].voltage) {
    return 0;
  }

  const size_t pointCount = sizeof(curve) / sizeof(curve[0]);
  for (size_t i = 1; i < pointCount; i++) {
    if (voltage <= curve[i].voltage) {
      const float span = curve[i].voltage - curve[i - 1].voltage;
      const float fraction = (voltage - curve[i - 1].voltage) / span;
      const float percent = curve[i - 1].percent +
                            fraction * (curve[i].percent -
                                        curve[i - 1].percent);
      return static_cast<uint8_t>(percent + 0.5f);
    }
  }

  return 100;
}

void printBatteryStatus() {
  const bool charging = isBatteryCharging();
  const float voltage = readBatteryVoltage();
  const uint8_t percent = estimateBatteryPercent(voltage);

  diagnostics.print("Battery: ");
  diagnostics.print(voltage, 2);
  diagnostics.print(" V (~");
  diagnostics.print(percent);
  diagnostics.print("%) | charger: ");
  diagnostics.println(charging ? "CHARGING" : "not charging / full");
}

void updateStatusLed(uint32_t now) {
  if (SHOW_CHARGING_LED && isBatteryCharging()) {
    const uint32_t cycleMs = CHARGING_LED_ON_MS + CHARGING_LED_OFF_MS;
    const bool validTiming = cycleMs > 0;
    const bool blueOn = validTiming &&
                        now % cycleMs < CHARGING_LED_ON_MS;
    setRgbLed(false, false, blueOn);
    return;
  }

  if (static_cast<int32_t>(handoffFeedbackUntil - now) > 0) {
    const bool blueOn = (now / 120) % 2 == 0;
    setRgbLed(false, false, blueOn);
    return;
  }

  if (remoteState == RemoteState::WAKE_FLASHING) {
    const uint32_t elapsed = now - wakeFlashStartedAt;
    const bool blueOn = elapsed < WAKE_FLASH_TOTAL_MS &&
                        elapsed % WAKE_FLASH_CYCLE_MS < WAKE_FLASH_ON_MS;
    setRgbLed(false, false, blueOn);
    return;
  }

  if (faceDownSince != 0) {
    setRgbLed(true, false, true);
    return;
  }

  if (remoteState == RemoteState::PARKED) {
    setRgbLed(false, false, false);
  } else if (remoteState == RemoteState::PICKUP_SETTLING) {
    setRgbLed(false, false, SHOW_SETTLING_LED);
  } else {
    const uint32_t elapsed = now - readyFlashStartedAt;
    const uint32_t flashCycleMs = ARMED_FLASH_ON_MS + ARMED_FLASH_OFF_MS;
    const bool validFlashTiming = ARMED_FLASH_COUNT > 0 && flashCycleMs > 0;
    const uint32_t flashNumber = validFlashTiming ? elapsed / flashCycleMs : 0;
    const bool flashOn = validFlashTiming &&
                         flashNumber < ARMED_FLASH_COUNT &&
                         elapsed % flashCycleMs < ARMED_FLASH_ON_MS;
    setRgbLed(false, false, flashOn);
  }
}

void beginLearning(LearningDirection direction, float gz) {
  learningDirection = direction;
  gesturePeakDps = fabs(gz);
  oppositePeakDps = 0.0f;
}

void observeGesture(float gz) {
  if (learningDirection == LearningDirection::NEXT) {
    gesturePeakDps = max(gesturePeakDps, -gz);
    oppositePeakDps = max(oppositePeakDps, gz);
  } else if (learningDirection == LearningDirection::PREVIOUS) {
    gesturePeakDps = max(gesturePeakDps, gz);
    oppositePeakDps = max(oppositePeakDps, -gz);
  }
}

void clearRetryTracking() {
  weakDirection = LearningDirection::NONE;
  weakPeakDps = 0.0f;
  weakQuietSince = 0;
  pendingRetryDirection = LearningDirection::NONE;
  pendingRetryPeakDps = 0.0f;
  pendingRetryAt = 0;
}

void resetSessionLearning() {
  expectedNextPeakDps = 750.0f;
  expectedPreviousPeakDps = 1000.0f;
  nextTurnThresholdDps = TURN_THRESHOLD_DPS;
  previousTurnThresholdDps = TURN_THRESHOLD_DPS;
  learnedGestureCount = 0;
  learningDirection = LearningDirection::NONE;
  gesturePeakDps = 0.0f;
  oppositePeakDps = 0.0f;
  clearRetryTracking();
  diagnostics.println("Adaptive tuning reset to baseline");
}

float *thresholdFor(LearningDirection direction) {
  return direction == LearningDirection::NEXT
             ? &nextTurnThresholdDps
             : &previousTurnThresholdDps;
}

void learnFromRetry(LearningDirection direction, float firstPeak,
                    float secondPeak) {
  if (!ENABLE_RETRY_LEARNING) {
    return;
  }

  float *threshold = thresholdFor(direction);
  *threshold = max(MIN_TURN_THRESHOLD_DPS,
                   *threshold - RETRY_THRESHOLD_STEP_DPS);

  diagnostics.print("Retry confirmed; first peak=");
  diagnostics.print(firstPeak, 1);
  diagnostics.print(" second peak=");
  diagnostics.print(secondPeak, 1);
  diagnostics.print(" new direction threshold=");
  diagnostics.println(*threshold, 1);
}

void sendPageForGesture(LearningDirection direction, bool retryConfirmed) {
  // NEXT/PREVIOUS identify the two trained physical swing directions. This
  // setting decides which page command each physical direction should send.
  const bool originalMappingAdvances = direction == LearningDirection::NEXT;
  const bool advancePage = REVERSE_PAGE_DIRECTION
                               ? !originalMappingAdvances
                               : originalMappingAdvances;

  if (advancePage) {
    sendArrow(HID_KEY_ARROW_RIGHT,
              retryConfirmed ? "NEXT PAGE (retry confirmed)  ->"
                             : "NEXT PAGE  ->");
  } else {
    sendArrow(HID_KEY_ARROW_LEFT,
              retryConfirmed ? "<-  PREVIOUS PAGE (retry confirmed)"
                             : "<-  PREVIOUS PAGE");
  }
}

void rememberWeakGesture(LearningDirection direction, float peak,
                         uint32_t now) {
  const bool matchingRetry = pendingRetryDirection == direction &&
                             now - pendingRetryAt <= RETRY_WINDOW_MS;

  if (matchingRetry) {
    sendPageForGesture(direction, true);
    learnFromRetry(direction, pendingRetryPeakDps, peak);
    gestureArmed = false;
    quietSince = 0;
    clearRetryTracking();
  } else {
    pendingRetryDirection = direction;
    pendingRetryPeakDps = peak;
    pendingRetryAt = now;
  }
}

void finishLearning() {
  if (learningDirection == LearningDirection::NONE) {
    return;
  }

  float *expected = learningDirection == LearningDirection::NEXT
                        ? &expectedNextPeakDps
                        : &expectedPreviousPeakDps;

  // Learn only plausible, clean motions. A large opposite-direction component
  // indicates handling or an ambiguous gesture and is deliberately rejected.
  const bool plausibleStrength = gesturePeakDps >= *expected * 0.60f &&
                                 gesturePeakDps <= *expected * 1.60f;
  const bool cleanDirection = oppositePeakDps <= gesturePeakDps * 0.55f;

  if (ENABLE_INCREMENTAL_LEARNING && plausibleStrength && cleanDirection) {
    *expected = (1.0f - LEARNING_RATE) * *expected +
                LEARNING_RATE * gesturePeakDps;
    learnedGestureCount++;

    float *directionThreshold = thresholdFor(learningDirection);
    const float targetThreshold = constrain(*expected * 0.53f,
                                            MIN_TURN_THRESHOLD_DPS,
                                            MAX_TURN_THRESHOLD_DPS);
    // Threshold adaptation is itself damped, on top of the 2% template update.
    *directionThreshold = 0.90f * *directionThreshold +
                          0.10f * targetThreshold;

    diagnostics.print("Learned clean gesture #");
    diagnostics.print(learnedGestureCount);
    diagnostics.print("; peak=");
    diagnostics.print(gesturePeakDps, 1);
    diagnostics.print(" dps; direction threshold=");
    diagnostics.println(*directionThreshold, 1);
  } else if (ENABLE_INCREMENTAL_LEARNING) {
    diagnostics.println("Gesture used, but excluded from learning (ambiguous/outlier)");
  }

  learningDirection = LearningDirection::NONE;
  gesturePeakDps = 0.0f;
  oppositePeakDps = 0.0f;
}

void startAdvertising() {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_KEYBOARD);
  Bluefruit.Advertising.addService(keyboard);
  Bluefruit.Advertising.addName();
  // The 128-bit Nordic UART UUID lives in the scan response because the main
  // advertising packet is already carrying the keyboard identity and name.
  Bluefruit.ScanResponse.addService(bleUart);
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

void sendArrow(uint8_t key, const char *label) {
  // An intentional page turn is meaningful activity.
  lastMeaningfulMotionAt = millis();

  if (!Bluefruit.connected()) {
    diagnostics.println("Gesture recognized; iPad not connected");
    return;
  }

  uint8_t keys[6] = {key, 0, 0, 0, 0, 0};
  keyboard.keyboardReport(0, keys);
  delay(25);
  keyboard.keyRelease();
  diagnostics.println(label);
}

void setup() {
  wokeFromSystemOff = (readResetReason() & POWER_RESETREAS_OFF_Msk) != 0;
  Serial.begin(115200);
  delay(wokeFromSystemOff ? 50 : 600);

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  setRgbLed(false, false, false);

  // P0.17 is driven by the onboard charger: LOW while charging, HIGH when
  // charging is complete or inactive. Do not enable an internal pull resistor.
  nrf_gpio_cfg_input(CHARGE_STATUS_GPIO, NRF_GPIO_PIN_NOPULL);

  // Seeed specifies P0.14 LOW to enable the protected battery divider. Leaving
  // it LOW is safe during charging and costs only a few microamps.
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);
  analogReadResolution(12);
  analogSampleTime(40);
  analogOversampling(16);

  if (imu.begin() != 0) {
    diagnostics.println("IMU initialization failed");
    while (true) {
      delay(1000);
    }
  }
  restoreActiveImuMode();

  Bluefruit.begin();
  bluetoothReady = true;
  Bluefruit.setName(BLUETOOTH_NAME);
  Bluefruit.setTxPower(4);
  Bluefruit.Periph.setConnectCallback(onBleConnect);
  Bluefruit.Security.setSecuredCallback(onBleSecured);
  loadHostSlots();

  deviceInfo.setManufacturer("Roy");
  deviceInfo.setModel("Trained XIAO Sense Page Turner");
  deviceInfo.begin();

  keyboard.begin();
  bleUart.begin();
  startAdvertising();

  diagnostics.println("Roy Page Turner V1.19 experimental ready");
  if (Serial) {
    printBatteryStatus();
  }
  diagnostics.println("State: PARKED. Shake three times rapidly to wake.");
  parkedAwakeStartedAt = millis();
  if (wokeFromSystemOff) {
    // The hardware wake event counts as the first of the trained impulses.
    // Two more strong, rotating impacts must follow after the IMU settles.
    wakeImpulseCount = 1;
    wakeWindowStartedAt = millis();
    lastWakeImpulseAt = millis();
    wakeImpulseReady = true;
    wakeDetectionNotBefore = millis() + POST_WAKE_SENSOR_GUARD_MS;

    // A random bump may wake the processor, but it must not reconnect an iPad.
    // Advertising resumes only after the deliberate shake is accepted.
    Bluefruit.Advertising.stop();
    bluetoothSuppressedForWake = true;
    diagnostics.println("IMU motion wake: waiting for deliberate shake");
  }
}

void loop() {
  const float ax = imu.readFloatAccelX();
  const float ay = imu.readFloatAccelY();
  const float az = imu.readFloatAccelZ();
  const float gx = imu.readFloatGyroX();
  const float gy = imu.readFloatGyroY();
  const float gz = imu.readFloatGyroZ();
  const uint32_t now = millis();
  const float gyroMagnitude = sqrtf(gx * gx + gy * gy + gz * gz);
  const float accelMagnitude = sqrtf(ax * ax + ay * ay + az * az);
  const bool meaningfulMotion =
      gyroMagnitude >= ACTIVITY_GYRO_THRESHOLD_DPS ||
      fabsf(accelMagnitude - 1.0f) >= ACTIVITY_ACCEL_DELTA_G;

  const bool calmlyFaceDown = ENABLE_FACE_DOWN_HANDOFF &&
                              az <= FACE_DOWN_Z_MAX_G &&
                              gyroMagnitude <= FACE_DOWN_GYRO_MAX_DPS;
  if (calmlyFaceDown && !faceDownLatched) {
    lastFaceDownAt = now;
    if (faceDownSince == 0) {
      faceDownSince = now;
      diagnostics.println("Face-down handoff: keep holding...");
    } else if (now - faceDownSince >= FACE_DOWN_HOLD_MS) {
      faceDownLatched = true;
      beginFaceDownHandoff(now);
    }
  } else if (calmlyFaceDown) {
    lastFaceDownAt = now;
    // Remaining face-down keeps handoff mode alive indefinitely. Once turned
    // upright, the full timeout remains available for choosing another iPad.
    if (faceDownLatched && blockedHostValid) {
      handoffUntil = now + HANDOFF_WINDOW_MS;
    }
  } else if (lastFaceDownAt != 0 &&
             now - lastFaceDownAt > FACE_DOWN_GRACE_MS) {
    faceDownSince = 0;
    lastFaceDownAt = 0;
    faceDownLatched = false;
  }

  if (blockedHostValid &&
      static_cast<int32_t>(now - handoffUntil) >= 0) {
    blockedHostValid = false;
    handoffUntil = 0;
    diagnostics.println("Handoff window ended; either iPad may connect");
  }

  enforceHandoffConnection(now);

  observeGesture(gz);

  if (remoteState == RemoteState::PARKED) {
    if (now >= wakeDetectionNotBefore && !isBatteryCharging() &&
        updateShakeToWake(accelMagnitude, gyroMagnitude, now)) {
      remoteState = RemoteState::WAKE_FLASHING;
      wakeFlashStartedAt = now;
      gestureArmed = false;
      clearRetryTracking();
      if (bluetoothSuppressedForWake) {
        startAdvertising();
        bluetoothSuppressedForWake = false;
      }
      diagnostics.println("Shake accepted: HEY BOSS! I'M AWAKE!");
    } else if (!isUsbPowered() &&
               now - parkedAwakeStartedAt >= WAKE_QUALIFICATION_MS) {
      enterImuDeepSleep();
    }
  } else if (remoteState == RemoteState::WAKE_FLASHING) {
    if (now - wakeFlashStartedAt >= WAKE_FLASH_TOTAL_MS) {
      remoteState = RemoteState::PICKUP_SETTLING;
      pickupQuietSince = 0;
      pickupSettlingStartedAt = now;
      lastSettlingDiagnosticAt = 0;
      diagnostics.println("State: SETTLING (gestures ignored)");
    }
  } else if (remoteState == RemoteState::PICKUP_SETTLING) {
    const bool calmlyHeld = gyroMagnitude <= SETTLE_GYRO_MAX_DPS;
    if (lastSettlingDiagnosticAt == 0 ||
        now - lastSettlingDiagnosticAt >= 1000) {
      lastSettlingDiagnosticAt = now;
      diagnostics.print("Settling gyro: ");
      diagnostics.print(gyroMagnitude, 1);
      diagnostics.println(" dps");
    }
    if (calmlyHeld) {
      if (pickupQuietSince == 0) {
        pickupQuietSince = now;
      } else if (now - pickupQuietSince >= PICKUP_SETTLE_MS) {
        remoteState = RemoteState::READY;
        gestureArmed = true;
        quietSince = now;
        readyFlashStartedAt = now;
        lastMeaningfulMotionAt = now;
        diagnostics.println("State: READY");
      }
    } else {
      pickupQuietSince = 0;
    }

    if (remoteState == RemoteState::PICKUP_SETTLING &&
        now - pickupSettlingStartedAt >= PICKUP_SETTLE_TIMEOUT_MS) {
      remoteState = RemoteState::READY;
      gestureArmed = true;
      quietSince = now;
      readyFlashStartedAt = now;
      lastMeaningfulMotionAt = now;
      diagnostics.println("State: READY (settling timeout fail-safe)");
    }
  } else {
    if (meaningfulMotion) {
      lastMeaningfulMotionAt = now;
    } else if (now - lastMeaningfulMotionAt >= MOTION_INACTIVITY_MS) {
      remoteState = RemoteState::PARKED;
      gestureArmed = false;
      quietSince = 0;
      resetSessionLearning();
      resetWakeDetector();
      diagnostics.println("State: PARKED after motion inactivity");
      enterImuDeepSleep();
    }
  }

  // A short quiet period arms one gesture. After a turn, it cannot repeat
  // until the hand settles again.
  if (remoteState == RemoteState::READY &&
      fabs(gz) <= QUIET_THRESHOLD_DPS) {
    if (quietSince == 0) {
      quietSince = now;
    }
    if (!gestureArmed && now - quietSince >= QUIET_TO_ARM_MS) {
      finishLearning();
      gestureArmed = true;
    }
  } else {
    quietSince = 0;
  }

  if (remoteState == RemoteState::READY && gestureArmed) {
    // Recorded next-page swings were strongly negative on gyro Z. Their
    // positive wind-up never approached this threshold.
    if (gz <= -nextTurnThresholdDps) {
      const bool followsMiss = pendingRetryDirection == LearningDirection::NEXT &&
                               now - pendingRetryAt <= RETRY_WINDOW_MS;
      sendPageForGesture(LearningDirection::NEXT, false);
      if (followsMiss) {
        learnFromRetry(LearningDirection::NEXT, pendingRetryPeakDps, -gz);
      }
      clearRetryTracking();
      beginLearning(LearningDirection::NEXT, gz);
      gestureArmed = false;
      quietSince = 0;
    } else if (gz >= previousTurnThresholdDps) {
      const bool followsMiss =
          pendingRetryDirection == LearningDirection::PREVIOUS &&
          now - pendingRetryAt <= RETRY_WINDOW_MS;
      sendPageForGesture(LearningDirection::PREVIOUS, false);
      if (followsMiss) {
        learnFromRetry(LearningDirection::PREVIOUS, pendingRetryPeakDps, gz);
      }
      clearRetryTracking();
      beginLearning(LearningDirection::PREVIOUS, gz);
      gestureArmed = false;
      quietSince = 0;
    } else if (ENABLE_RETRY_LEARNING && gz <= -RETRY_CANDIDATE_DPS) {
      if (weakDirection != LearningDirection::NEXT) {
        weakPeakDps = 0.0f;
      }
      weakDirection = LearningDirection::NEXT;
      weakPeakDps = max(weakPeakDps, -gz);
      weakQuietSince = 0;
    } else if (ENABLE_RETRY_LEARNING && gz >= RETRY_CANDIDATE_DPS) {
      if (weakDirection != LearningDirection::PREVIOUS) {
        weakPeakDps = 0.0f;
      }
      weakDirection = LearningDirection::PREVIOUS;
      weakPeakDps = max(weakPeakDps, gz);
      weakQuietSince = 0;
    }
  }

  if (remoteState == RemoteState::READY && gestureArmed &&
      weakDirection != LearningDirection::NONE &&
      fabs(gz) <= QUIET_THRESHOLD_DPS) {
    if (weakQuietSince == 0) {
      weakQuietSince = now;
    } else if (now - weakQuietSince >= WEAK_GESTURE_END_MS) {
      const LearningDirection completedDirection = weakDirection;
      const float completedPeak = weakPeakDps;
      weakDirection = LearningDirection::NONE;
      weakPeakDps = 0.0f;
      weakQuietSince = 0;
      rememberWeakGesture(completedDirection, completedPeak, now);
    }
  }

  if (pendingRetryDirection != LearningDirection::NONE &&
      now - pendingRetryAt > RETRY_WINDOW_MS) {
    pendingRetryDirection = LearningDirection::NONE;
    pendingRetryPeakDps = 0.0f;
    pendingRetryAt = 0;
  }

  if (SHOW_MOTION_SERIAL_STATUS && now - lastPrintAt >= 150) {
    diagnostics.print("gyro Z: ");
    diagnostics.print(gz, 1);
    diagnostics.print(" dps   state: ");
    if (remoteState == RemoteState::PARKED) {
      diagnostics.print("PARKED");
    } else if (remoteState == RemoteState::WAKE_FLASHING) {
      diagnostics.print("WAKE FLASHING");
    } else if (remoteState == RemoteState::PICKUP_SETTLING) {
      diagnostics.print("PICKUP SETTLING");
    } else {
      diagnostics.print(gestureArmed ? "READY/ARMED" : "READY/settling");
    }
    diagnostics.print("   BLE: ");
    diagnostics.println(Bluefruit.connected() ? "connected" : "advertising");
    lastPrintAt = now;
  }

  if (SHOW_BATTERY_SERIAL_STATUS && Serial &&
      now - lastBatteryPrintAt >= BATTERY_STATUS_INTERVAL_MS) {
    printBatteryStatus();
    lastBatteryPrintAt = now;
  }

  updateStatusLed(now);

  delay(5);
}
