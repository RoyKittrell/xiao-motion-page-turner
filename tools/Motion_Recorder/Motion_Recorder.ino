// Page Turner Motion Recorder
//
// USB connected: streams CSV measurements to Serial Monitor at 115200 baud.
// USB disconnected: records up to two minutes to internal flash.
// USB reconnected: automatically prints the saved walk as CSV.

#include <LSM6DS3.h>
#include <Wire.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

constexpr uint32_t SAMPLE_INTERVAL_MS = 40;  // 25 samples/second.
constexpr uint16_t MAX_SAVED_SAMPLES = 3000; // Two minutes.
constexpr char RECORDING_FILE[] = "/page_turner_motion.bin";

LSM6DS3 imu(I2C_MODE, 0x6A);

// Fixed-point storage keeps a two-minute recording to roughly 36 KB.
struct __attribute__((packed)) MotionSample {
  int16_t axMg;
  int16_t ayMg;
  int16_t azMg;
  int16_t gxTenths;
  int16_t gyTenths;
  int16_t gzTenths;
};

File recordingFile(InternalFS);
bool usbWasPowered = false;
bool serialSessionStarted = false;
bool recording = false;
bool recordingFull = false;
uint16_t savedSampleCount = 0;
uint32_t lastSampleAt = 0;
uint32_t sessionStartedAt = 0;

bool isUsbPowered() {
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

int16_t toInt16(float value, float scale) {
  const float scaled = value * scale;
  return static_cast<int16_t>(constrain(scaled, -32768.0f, 32767.0f));
}

MotionSample readMotion() {
  MotionSample sample;
  sample.axMg = toInt16(imu.readFloatAccelX(), 1000.0f);
  sample.ayMg = toInt16(imu.readFloatAccelY(), 1000.0f);
  sample.azMg = toInt16(imu.readFloatAccelZ(), 1000.0f);
  sample.gxTenths = toInt16(imu.readFloatGyroX(), 10.0f);
  sample.gyTenths = toInt16(imu.readFloatGyroY(), 10.0f);
  sample.gzTenths = toInt16(imu.readFloatGyroZ(), 10.0f);
  return sample;
}

void printCsvHeader() {
  Serial.println("time_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps");
}

void printSample(uint32_t timeMs, const MotionSample &sample) {
  Serial.print(timeMs);
  Serial.print(',');
  Serial.print(sample.axMg / 1000.0f, 3);
  Serial.print(',');
  Serial.print(sample.ayMg / 1000.0f, 3);
  Serial.print(',');
  Serial.print(sample.azMg / 1000.0f, 3);
  Serial.print(',');
  Serial.print(sample.gxTenths / 10.0f, 1);
  Serial.print(',');
  Serial.print(sample.gyTenths / 10.0f, 1);
  Serial.print(',');
  Serial.println(sample.gzTenths / 10.0f, 1);
}

void beginPocketRecording() {
  if (recordingFile) {
    recordingFile.close();
  }

  InternalFS.remove(RECORDING_FILE);
  if (!recordingFile.open(RECORDING_FILE, FILE_O_WRITE)) {
    recording = false;
    return;
  }

  recording = true;
  recordingFull = false;
  savedSampleCount = 0;
  sessionStartedAt = millis();
  lastSampleAt = 0;
}

void finishPocketRecording() {
  if (recordingFile) {
    recordingFile.close();
  }
  recording = false;
}

void dumpSavedRecording() {
  File file(RECORDING_FILE, FILE_O_READ, InternalFS);
  if (!file || file.size() < sizeof(MotionSample)) {
    if (file) {
      file.close();
    }
    Serial.println("No pocket recording found.");
    return;
  }

  const uint32_t sampleCount = file.size() / sizeof(MotionSample);
  Serial.println("--- SAVED POCKET WALK START ---");
  printCsvHeader();

  MotionSample sample;
  uint32_t index = 0;
  while (file.read(reinterpret_cast<uint8_t *>(&sample), sizeof(sample)) ==
         sizeof(sample)) {
    printSample(index * SAMPLE_INTERVAL_MS, sample);
    index++;
  }

  file.close();
  Serial.print("--- SAVED POCKET WALK END: ");
  Serial.print(sampleCount);
  Serial.println(" samples ---");
}

void setup() {
  Serial.begin(115200);
  InternalFS.begin();

  if (imu.begin() != 0) {
    while (true) {
      delay(1000);
    }
  }

  usbWasPowered = isUsbPowered();
  if (!usbWasPowered) {
    beginPocketRecording();
  }
}

void loop() {
  const uint32_t now = millis();
  const bool usbPowered = isUsbPowered();
  const bool serialConnected = static_cast<bool>(Serial);

  if (usbPowered && !usbWasPowered) {
    finishPocketRecording();
    serialSessionStarted = false;
  } else if (!usbPowered && usbWasPowered) {
    serialSessionStarted = false;
    beginPocketRecording();
  }
  usbWasPowered = usbPowered;

  if (usbPowered && serialConnected && !serialSessionStarted) {
    Serial.println("Page Turner Motion Recorder ready.");
    dumpSavedRecording();
    Serial.println("--- LIVE DESK RECORDING START ---");
    printCsvHeader();
    sessionStartedAt = now;
    lastSampleAt = 0;
    serialSessionStarted = true;
  }

  if (now - lastSampleAt < SAMPLE_INTERVAL_MS) {
    delay(1);
    return;
  }
  lastSampleAt = now;

  const MotionSample sample = readMotion();
  if (usbPowered && serialConnected && serialSessionStarted) {
    printSample(now - sessionStartedAt, sample);
  } else if (!usbPowered && recording && !recordingFull) {
    if (savedSampleCount < MAX_SAVED_SAMPLES) {
      recordingFile.write(reinterpret_cast<const uint8_t *>(&sample),
                          sizeof(sample));
      savedSampleCount++;
      if (savedSampleCount % 25 == 0) {
        recordingFile.flush();
      }
    } else {
      recordingFull = true;
      finishPocketRecording();
    }
  }
}
