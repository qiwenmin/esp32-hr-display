#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <TM1638plus_Model2.h>

#include <set>

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

static Preferences prefs;
const char* pref_namespace = "sys_cfg";

static uint8_t brightness = 1;
static uint8_t verbose = 1;
static bool enable_whitelist = false;
static std::set<std::string> whitelist;

#define ERROR if (verbose >= 1)
#define INFO if (verbose >= 2)

/* =========================================================
 * BLE 通知处理
 * ========================================================= */
void hrNotifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    if (len < 1) return;

    // 心率数据解析：通常第1字节是Flag，第2字节是HR值
    uint8_t hrValue = (data[0] & 0x01) ? (data[1] | (data[2] << 8)) : data[1];

    // 串口输出心率值
    if (hrValue > 0 && hrValue != currentHR) {
        INFO Serial.printf("[DATA] Heart Rate: %d bpm\n", hrValue);
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
            INFO Serial.printf("[SCAN] Target found: %s, RSSI: %d\n", targetAddr.toString().c_str(), dev->getRSSI());
            if (enable_whitelist) {
                if (!whitelist.contains(targetAddr.toString())) {
                    INFO Serial.printf("[SCAN] %s is not in the whitelist. Ignored.\n", targetAddr.toString().c_str());
                    return;
                } else {
                    INFO Serial.printf("[SCAN] %s is in the whitelist.\n", targetAddr.toString().c_str());
                }
            }
            NimBLEDevice::getScan()->stop();
            doConnect = true;
        }
    }

    void onDisconnect(NimBLEClient* c, int reason) override {
        INFO Serial.printf("[BLE] Disconnected, reason: %d\n", reason);
        currentHR = 0;
        doConnect = false;
        // 断开后稍微延迟再扫描，增加稳定性
        vTaskDelay(pdMS_TO_TICKS(1000));
        INFO Serial.println("[SCAN] Resuming scan...");
        NimBLEDevice::getScan()->start(0, false);
    }
};

static MyBLECallbacks bleHandler;

/* =========================================================
 * 连接逻辑
 * ========================================================= */
bool connectToDevice() {
    INFO Serial.printf("[CONN] Attempting to connect to %s\n", targetAddr.toString().c_str());
    if (!pClient->connect(targetAddr, false)) {
        ERROR Serial.println("[CONN] Connection failed");
        return false;
    }

    INFO Serial.println("[CONN] Connected, discovering services...");
    pClient->getServices(true);

    NimBLERemoteService* pSvc = pClient->getService("180D");
    if (pSvc) {
        NimBLERemoteCharacteristic* pChar = pSvc->getCharacteristic("2A37");
        if (pChar && pChar->canNotify()) {
            if (pChar->subscribe(true, hrNotifyCallback)) {
                INFO Serial.println("[CONN] HR service subscribed successfully");
                return true;
            }
        }
    }

    ERROR Serial.println("[CONN] Service or characteristic not found");
    pClient->disconnect();
    return false;
}

/* =========================================================
 * Model 2 显示任务
 * ========================================================= */
void displayTask(void* arg) {
    module.displayBegin();
    module.brightness(brightness);

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
                ERROR Serial.println("[MGR] Connection failed, back to scanning");
                NimBLEDevice::getScan()->start(0, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* =========================================================
 * 配置的保存和加载
 * ========================================================= */
static void saveSettings() {
    // 参数2为 false 表示读写模式
    prefs.begin(pref_namespace, false);

    prefs.clear();

    prefs.putUChar("brightness", brightness);
    prefs.putUChar("verbose", verbose);

    prefs.putBool("wl_en", enable_whitelist);
    prefs.putInt("wl_len", whitelist.size());
    int i = 0;
    for (const auto &mac : whitelist) {
        char key[10];
        snprintf(key, 10, "wl_%d", i++);
        prefs.putString(key, mac.c_str());
    }

    prefs.end(); // 关闭并保存

    INFO Serial.println("Settings saved to NVS.");
}

static void loadSettings() {
    // 参数2为 true 表示只读模式
    prefs.begin(pref_namespace, true);

    // 第二个参数是默认值。如果 NVS 中还没保存过该项，则返回此值。
    brightness = prefs.getUChar("brightness", 1);
    verbose = prefs.getUChar("verbose", 1);

    enable_whitelist = prefs.getBool("wl_en", false);

    int wl_len = prefs.getInt("wl_len", 0);
    for (int i = 0; i < wl_len; i++) {
        char key[10];
        snprintf(key, 10, "wl_%d", i);
        String mac = prefs.getString(key, "");
        if (mac.length() > 0) {
            whitelist.insert(mac.c_str());
        }
    }

    prefs.end();
}

/* =========================================================
 * forth解释器任务及相关的词
 * ========================================================= */
static void forth_get_hr() {
    So(1);
    Push = (atl_int) currentHR;
}

static void forth_set_br() {
    Sl(1);
    brightness = (int)S0;
    Pop;

    if (brightness > 7) brightness = 7;
    module.brightness(brightness);
}

static void forth_get_br() {
    So(1);
    Push = (atl_int) brightness;
}

static void forth_set_verbose() {
    Sl(1);
    verbose = (int)S0;
    Pop;

    if (verbose > 2) verbose = 2;
}

static void forth_get_verbose() {
    So(1);
    Push = (atl_int) verbose;
}

static void forth_set_enable_whitelist() {
    Sl(1);
    enable_whitelist = (int)S0;
    Pop;

    if (enable_whitelist) enable_whitelist = 1;
}

static void forth_get_enable_whitelist() {
    So(1);
    Push = (atl_int) enable_whitelist;
}

static void forth_whitelist_list() {
    Serial.print("mac-address white list\n");
    Serial.print("----------------------\n");
    for (auto &mac : whitelist) {
        Serial.println(mac.c_str());
    }
}

static void forth_whitelist_insert() {
    Sl(1);
    Hpc(S0);
    whitelist.insert((char *) S0);
    Pop;
}

static void forth_whitelist_erase() {
    Sl(1);
    Hpc(S0);
    whitelist.erase((char *)S0);
    Pop;
}

static void forth_list_tasks() {
    // 1. 获取当前任务总数
    UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();

    // 2. 为任务状态数组分配内存
    TaskStatus_t *pxTaskStatusArray = (TaskStatus_t *)pvPortMalloc(uxArraySize * sizeof(TaskStatus_t));

    if (pxTaskStatusArray != NULL) {
        // 3. 获取所有任务的状态信息
        // 第三个参数为 NULL 表示不计算 CPU 使用率百分比（需要额外配置计时器）
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, NULL);

        Serial.println("\n--- Task Debug Info ---");
        Serial.printf("%-16s %-10s %-10s %-10s %-10s\n", "Name", "State", "Priority", "StackMin", "Number");

        for (UBaseType_t x = 0; x < uxArraySize; x++) {
            char stateChar;
            switch (pxTaskStatusArray[x].eCurrentState) {
                case eRunning:   stateChar = 'X'; break; // 正在运行
                case eReady:     stateChar = 'R'; break; // 就绪
                case eBlocked:   stateChar = 'B'; break; // 阻塞
                case eSuspended: stateChar = 'S'; break; // 挂起
                case eDeleted:   stateChar = 'D'; break; // 已删除
                default:         stateChar = '?'; break;
            }

            Serial.printf("%-16s %-10c %-10u %-10u %-10u\n",
                          pxTaskStatusArray[x].pcTaskName,
                          stateChar,
                          (unsigned int)pxTaskStatusArray[x].uxCurrentPriority,
                          (unsigned int)pxTaskStatusArray[x].usStackHighWaterMark, // 剩余堆栈最小值
                          (unsigned int)pxTaskStatusArray[x].xTaskNumber);
        }

        // 4. 释放内存
        vPortFree(pxTaskStatusArray);
    } else {
        Serial.println("Failed to allocate memory for task stats.");
    }
}

static struct primfcn my_primitives[] = {
    {"0HR", forth_get_hr},

    {"0BR!", forth_set_br},
    {"0BR@", forth_get_br},

    {"0VERB!", forth_set_verbose},
    {"0VERB@", forth_get_verbose},

    {"0WLEN!", forth_set_enable_whitelist},
    {"0WLEN@", forth_get_enable_whitelist},

    {"0WL?", forth_whitelist_list},
    {"0WL+", forth_whitelist_insert},
    {"0WL-", forth_whitelist_erase},

    {"0SAVE", saveSettings},

    {"0PS", forth_list_tasks},
    {"0REBOOT", esp_restart},

    {NULL, NULL}
};

void forthTask(void* arg) {
    char inputBuffer[128];
    int idx = 0;

    // 初始化 Atlast 实例
    atl_init();
    atl_primdef(my_primitives);

    Serial.println("[FORTH] Interpreter Ready.");
    Serial.print("[FORTH] ");
    Serial.flush();

    for (;;) {
        if (Serial.available()) {
            char c = Serial.read();

            if (c == '\n') {
                inputBuffer[idx] = '\0';
                if (idx > 0) {
                    Serial.print(" ");
                    Serial.flush();
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
            } else if (!isprint(c)) {
                if (c == '\b') {
                    if (idx > 0) {
                        Serial.print("\b \b");
                        Serial.flush();
                        idx--;
                    }
                }
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

    INFO Serial.println("\n[SYS] ESP32-C3 HR Monitor Starting...");

    // 加载配置
    loadSettings();

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

    INFO Serial.println("[SCAN] Initial scan started...");
    pScan->start(0, false);

    // 创建 FreeRTOS 任务
    xTaskCreate(hrManagerTask, "hr_mgr", 4096, nullptr, 10, nullptr);
    xTaskCreate(displayTask,   "ds_mgr", 2048, nullptr,  5, nullptr);
    xTaskCreate(forthTask,  "forth_cli", 4096, nullptr,  2, nullptr);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
