#include <Arduino.h>
#include <NimBLEDevice.h>
#include <TM1638plus_Model2.h>

extern "C" {
    #include "atlast.h"
    #include "atldef.h"
}

/* =========================================================
 * 引脚配置 (WeAct ESP32-C3)
 * ========================================================= */
#define TM_STB 10
#define TM_CLK 6
#define TM_DIO 7

#define RSSI_LIMIT -90

TM1638plus_Model2 module(TM_STB, TM_CLK, TM_DIO);

static NimBLEClient* pClient = nullptr;
static NimBLEAddress targetAddr;
static bool doConnect = false;
static uint8_t currentHR = 0;

/* =========================================================
 * BLE 通知处理
 * ========================================================= */
void hrNotifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    if (len < 1) return;

    // 心率数据解析：通常第1字节是Flag，第2字节是HR值
    uint8_t hrValue = (data[0] & 0x01) ? (data[1] | (data[2] << 8)) : data[1];

    // 串口输出心率值
    if (hrValue > 0 && hrValue != currentHR) {
        Serial.printf("[DATA] Heart Rate: %d bpm\n", hrValue);
    }

    // 更新全局变量
    currentHR = hrValue;
}

/* =========================================================
 * BLE 回调
 * ========================================================= */
class MyBLECallbacks : public NimBLEClientCallbacks, public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0x180D)) && dev->getRSSI() >= RSSI_LIMIT) {
            targetAddr = dev->getAddress();
            Serial.printf("[SCAN] Target found: %s, RSSI: %d\n", targetAddr.toString().c_str(), dev->getRSSI());
            NimBLEDevice::getScan()->stop();
            doConnect = true;
        }
    }

    void onDisconnect(NimBLEClient* c, int reason) override {
        Serial.printf("[BLE] Disconnected, reason: %d\n", reason);
        currentHR = 0;
        doConnect = false;
        // 断开后稍微延迟再扫描，增加稳定性
        vTaskDelay(pdMS_TO_TICKS(1000));
        Serial.println("[SCAN] Resuming scan...");
        NimBLEDevice::getScan()->start(0, false);
    }
};

static MyBLECallbacks bleHandler;

/* =========================================================
 * 连接逻辑
 * ========================================================= */
bool connectToDevice() {
    Serial.printf("[CONN] Attempting to connect to %s\n", targetAddr.toString().c_str());
    if (!pClient->connect(targetAddr, false)) {
        Serial.println("[CONN] Connection failed");
        return false;
    }

    Serial.println("[CONN] Connected, discovering services...");
    pClient->getServices(true);

    NimBLERemoteService* pSvc = pClient->getService("180D");
    if (pSvc) {
        NimBLERemoteCharacteristic* pChar = pSvc->getCharacteristic("2A37");
        if (pChar && pChar->canNotify()) {
            if (pChar->subscribe(true, hrNotifyCallback)) {
                Serial.println("[CONN] HR service subscribed successfully");
                return true;
            }
        }
    }

    Serial.println("[CONN] Service or characteristic not found");
    pClient->disconnect();
    return false;
}

/* =========================================================
 * Model 2 显示任务
 * ========================================================= */
void displayTask(void* arg) {
    module.displayBegin();
    module.brightness(1);

    char textBuffer[16];

    for (;;) {
        if (pClient && pClient->isConnected()) {
            // Model 2 使用 DisplayStr
            if (currentHR) {
                snprintf(textBuffer, sizeof(textBuffer), "%3d", currentHR);
            } else {
                snprintf(textBuffer, sizeof(textBuffer), "---");
            }
        } else if (doConnect) {
            snprintf(textBuffer, sizeof(textBuffer), "Con");
        } else {
            snprintf(textBuffer, sizeof(textBuffer), "Scn");
        }

        // Model 2 专用的显示函数：DisplayStr(字符串, 填充ASCII)
        // 注意：Model 2 库对字符位置映射比较严格
        module.DisplayStr(textBuffer, 0);

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* =========================================================
 * BLE 管理任务
 * ========================================================= */
void hrManagerTask(void* arg) {
    for (;;) {
        if (doConnect && !pClient->isConnected()) {
            if (!connectToDevice()) {
                doConnect = false;
                Serial.println("[MGR] Connection failed, back to scanning");
                NimBLEDevice::getScan()->start(0, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* =========================================================
 * forth解释器任务
 * ========================================================= */
void forthTask(void* arg) {
    char inputBuffer[128];
    int idx = 0;

    // 初始化 Atlast 实例
    atl_init();

    Serial.println("[FORTH] Interpreter Ready.");

    for (;;) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\r') continue;

            if (c == '\n') {
                inputBuffer[idx] = '\0';
                if (idx > 0) {
                    printf(" ");
                    atl_eval(inputBuffer);
                    printf(" ok\n");
                    fflush(stdout);
                } else {
                    printf("\n");
                    fflush(stdout);
                }
                Serial.print("[FORTH] ");
                Serial.flush();
                idx = 0;
            } else if (idx < sizeof(inputBuffer) - 1) {
                inputBuffer[idx++] = c;
                Serial.print(c); // 回显，这里用printf或putc，然后用fflush，不起作用。
                Serial.flush();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* =========================================================
 * Setup & Loop
 * ========================================================= */
void setup() {
    Serial.begin(115200);
    Serial.println("\n[SYS] ESP32-C3 HR Monitor Starting...");

    // 初始化蓝牙
    NimBLEDevice::init("C3_HR_MON");
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&bleHandler, false);

    // 配置扫描
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(&bleHandler, false);
    pScan->setInterval(150);
    pScan->setWindow(100);
    pScan->setDuplicateFilter(false);

    Serial.println("[SCAN] Initial scan started...");
    pScan->start(0, false);

    // 创建 FreeRTOS 任务
    xTaskCreate(hrManagerTask, "hr_mgr", 4096, nullptr, 1, nullptr);
    xTaskCreate(displayTask,   "ds_mgr", 2048, nullptr, 1, nullptr);
    xTaskCreate(forthTask,  "forth_cli", 4096, nullptr, 2, nullptr);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
