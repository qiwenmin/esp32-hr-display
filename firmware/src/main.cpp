#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

/* ================= UUID ================= */
static BLEUUID HR_SERVICE_UUID("180D");
static BLEUUID HR_CHAR_UUID("2A37");

/* ================= BLE 对象 ================= */
BLEScan* bleScan = nullptr;
BLEClient* bleClient = nullptr;
BLERemoteCharacteristic* hrChar = nullptr;
BLEAdvertisedDevice* targetDevice = nullptr;

/* ================= 状态机 ================= */
enum BleState {
  BLE_SCANNING,
  BLE_CONNECTING,
  BLE_CONNECTED
};

volatile BleState bleState = BLE_SCANNING;

/* ================= 心率时间管理 ================= */
unsigned long lastHrMillis = 0;
const unsigned long HR_TIMEOUT_MS = 5000;   // 5 秒无心率 → 认为异常

/* ================= 其他状态变量 ================= */
static bool scanning = false;

/* ================= 心率通知回调 ================= */
void hrNotifyCallback(
  BLERemoteCharacteristic*,
  uint8_t* pData,
  size_t length,
  bool
) {
  if (length < 2) return;

  uint8_t flags = pData[0];
  uint16_t hr;

  if ((flags & 0x01) == 0) {
    hr = pData[1];
  } else {
    if (length < 3) return;
    hr = pData[1] | (pData[2] << 8);
  }

  lastHrMillis = millis();

  Serial.print("Heart Rate: ");
  Serial.println(hr);
}

/* ================= Client 回调 ================= */
class MyClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override {
    Serial.println("BLE connected");
    lastHrMillis = millis();
  }

  void onDisconnect(BLEClient*) override {
    Serial.println("BLE disconnected");

    hrChar = nullptr;

    if (bleClient) {
      delete bleClient;
      bleClient = nullptr;
    }

    if (targetDevice) {
      delete targetDevice;
      targetDevice = nullptr;
    }

    scanning = false;
    bleState = BLE_SCANNING;
  }
};

/* ================= 扫描回调 ================= */
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {

    if (bleState != BLE_SCANNING) return;
    if (!advertisedDevice.haveServiceUUID()) return;
    if (!advertisedDevice.isAdvertisingService(HR_SERVICE_UUID)) return;

    Serial.print("Found HR device: ");
    Serial.println(advertisedDevice.getAddress().toString().c_str());

    targetDevice = new BLEAdvertisedDevice(advertisedDevice);

    bleState = BLE_CONNECTING;
    bleScan->stop();
    scanning = false;
  }
};

/* ================= 连接函数 ================= */
bool connectToHeartRateDevice() {

  bleClient = BLEDevice::createClient();
  bleClient->setClientCallbacks(new MyClientCallbacks());

  Serial.println("Connecting...");

  if (!bleClient->connect(targetDevice)) {
    Serial.println("Connect failed");

    delete bleClient;
    bleClient = nullptr;

    delete targetDevice;
    targetDevice = nullptr;

    return false;
  }

  BLERemoteService* hrService = bleClient->getService(HR_SERVICE_UUID);
  if (!hrService) {
    Serial.println("Heart Rate service not found");
    bleClient->disconnect();
    return false;
  }

  hrChar = hrService->getCharacteristic(HR_CHAR_UUID);
  if (!hrChar) {
    Serial.println("Heart Rate characteristic not found");
    bleClient->disconnect();
    return false;
  }

  if (hrChar->canNotify()) {
    hrChar->registerForNotify(hrNotifyCallback);
    Serial.println("Subscribed to heart rate notifications");
  } else {
    Serial.println("Heart Rate characteristic cannot notify");
    bleClient->disconnect();
    return false;
  }

  lastHrMillis = millis();
  bleState = BLE_CONNECTED;
  return true;
}

/* ================= Arduino ================= */
void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32-C3 BLE Heart Rate Receiver (stable + watchdog)");

  pinMode(LED_BUILTIN, OUTPUT);

  BLEDevice::init("");

  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  bleScan->setInterval(120);
  bleScan->setWindow(80);
  bleScan->setActiveScan(true);

  bleState = BLE_SCANNING;
  scanning = false;
}

void loop() {

  /* ---------- SCANNING ---------- */
  if (bleState == BLE_SCANNING) {
    if (!scanning) {
      Serial.println("Start scanning...");
      bleScan->start(0);
      scanning = true;
    }
  }

  /* ---------- CONNECTING ---------- */
  if (bleState == BLE_CONNECTING && targetDevice) {
    if (!connectToHeartRateDevice()) {
      Serial.println("Connect failed, return to scan");
      bleState = BLE_SCANNING;
      scanning = false;
      delay(1000);
    }
  }

  /* ---------- CONNECTED ---------- */
  if (bleState == BLE_CONNECTED) {
    if (millis() - lastHrMillis > HR_TIMEOUT_MS) {
      Serial.println("Heart rate timeout, force disconnect");
      if (bleClient && bleClient->isConnected()) {
        bleClient->disconnect();
      }
    }

    digitalWrite(LED_BUILTIN, digitalRead(LED_BUILTIN) == 0);
  }

  delay(200);
}
