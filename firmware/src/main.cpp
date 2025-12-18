#include <Arduino.h>
#include <NimBLEDevice.h>

// 配置：只保留核心业务参数
#define RSSI_LIMIT -90
static NimBLEAddress targetAddr;
static NimBLEClient* pClient = nullptr;
static bool doConnect = false;

/* =========================================================
 * 业务逻辑：心率处理
 * ========================================================= */
void hrNotifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    if (len < 2) return;
    uint8_t hr = (data[0] & 0x01) ? (data[1] | (data[2] << 8)) : data[1];
    Serial.printf("[HR] %d bpm\n", hr);
}

/* =========================================================
 * BLE 事件驱动（简洁的回调）
 * ========================================================= */
class MyBLECallbacks : public NimBLEClientCallbacks, public NimBLEScanCallbacks {
    // 发现设备
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0x180D)) && dev->getRSSI() >= RSSI_LIMIT) {
            targetAddr = dev->getAddress();
            NimBLEDevice::getScan()->stop();
            doConnect = true; // 告知任务可以连接了
        }
    }
    // 断开连接
    void onDisconnect(NimBLEClient* c, int reason) override {
        Serial.printf("Link Lost (Reason: %d). Scanning resumed.\n", reason);
        doConnect = false;
        NimBLEDevice::getScan()->start(0, false); // 自动恢复扫描
    }
};

static MyBLECallbacks bleHandler;

/* =========================================================
 * 稳定的连接函数（由任务调用）
 * ========================================================= */
bool connectToDevice() {
    if (!pClient->connect(targetAddr, false)) return false;

    NimBLERemoteService* pSvc = pClient->getService("180D");
    if (pSvc) {
        NimBLERemoteCharacteristic* pChar = pSvc->getCharacteristic("2A37");
        if (pChar && pChar->subscribe(true, hrNotifyCallback)) {
            Serial.println(">>> Subscribed Successfully");
            return true;
        }
    }
    pClient->disconnect();
    return false;
}

/* =========================================================
 * 主管理任务（逻辑清晰）
 * ========================================================= */
void hrManagerTask(void* arg) {
    for (;;) {
        if (doConnect && !pClient->isConnected()) {
            Serial.println(">>> Attempting Connection...");
            if (!connectToDevice()) {
                Serial.println(">>> Connect Failed, returning to scan.");
                doConnect = false;
                NimBLEDevice::getScan()->start(0, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500)); // 降低 CPU 占用
    }
}

void setup() {
    Serial.begin(115200);
    NimBLEDevice::init("C3_HR_MONITOR");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // 初始化全局 Client（一生只创建一次）
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&bleHandler, false);

    // 初始化扫描器
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(&bleHandler, false);
    pScan->setInterval(150);
    pScan->setWindow(100);
    pScan->setActiveScan(true);
    pScan->setDuplicateFilter(false); // 关键：关闭过滤以保证重连响应
    pScan->start(0, false);

    xTaskCreate(hrManagerTask, "hr_mgr", 4096, nullptr, 1, nullptr);
}

void loop() { vTaskDelay(portMAX_DELAY); }
