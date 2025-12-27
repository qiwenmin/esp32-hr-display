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
 * 引脚配置
 * ========================================================= */
#if defined(BOARD_C3)
#define TM_STB 10
#define TM_CLK 6
#define TM_DIO 7
#elif defined(BOARD_C3_SUPER_MINI)
#define TM_STB 10
#define TM_CLK 6
#define TM_DIO 7
#elif defined(BOARD_DEVKITV1)
#define TM_STB 4
#define TM_CLK 16
#define TM_DIO 17
#endif

#define RSSI_LIMIT -90

TM1638plus_Model2 g_display(TM_STB, TM_CLK, TM_DIO);

static NimBLEClient* g_client = nullptr;
static NimBLEAddress g_target_addr;
static SemaphoreHandle_t g_target_addr_mutex = xSemaphoreCreateMutex();
static bool g_do_connect = false;
static bool g_need_scan = false;
static uint32_t g_last_disconnect_time = 0;
static uint8_t g_hr = 0;

static Preferences g_prefs;
const char* k_pref_namespace = "sys_cfg";

static uint8_t g_brightness = 1;
static uint8_t g_verbose = 1;
static bool g_enable_allowlist = false;
static std::set<std::string> g_allowlist;
static SemaphoreHandle_t g_allowlist_mutex = xSemaphoreCreateMutex();

#define ERROR if (g_verbose >= 1)
#define INFO if (g_verbose >= 2)

/* =========================================================
 * BLE 通知处理
 * ========================================================= */
void HrNotifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool is_notify) {
    if (len < 1) return;

    // 心率数据解析：通常第1字节是Flag，第2字节是HR值
    uint8_t hr = (data[0] & 0x01) ? (data[1] | (data[2] << 8)) : data[1];

    // 串口输出心率值
    if (hr > 0 && hr != g_hr) {
        INFO printf("[DATA] Heart Rate: %d bpm\n", hr);
    }

    // 更新全局变量
    g_hr = hr;
}

/* =========================================================
 * BLE 回调
 * ========================================================= */
class MyBLECallbacks : public NimBLEClientCallbacks, public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0x180D)) && dev->getRSSI() >= RSSI_LIMIT) {
            NimBLEAddress addr = dev->getAddress();

            xSemaphoreTake(g_target_addr_mutex, portMAX_DELAY);
            g_target_addr = addr;
            xSemaphoreGive(g_target_addr_mutex);

            INFO printf("[SCAN] Target found: %s, RSSI: %d\n", addr.toString().c_str(), dev->getRSSI());
            if (g_enable_allowlist) {
                xSemaphoreTake(g_allowlist_mutex, portMAX_DELAY);
                bool in_allowlist = g_allowlist.contains(addr.toString());
                xSemaphoreGive(g_allowlist_mutex);
                if (!in_allowlist) {
                    INFO printf("[SCAN] %s is not in the allowlist. Ignored.\n", addr.toString().c_str());
                    return;
                } else {
                    INFO printf("[SCAN] %s is in the allowlist.\n", addr.toString().c_str());
                }
            }
            NimBLEDevice::getScan()->stop();
            g_do_connect = true;
        }
    }

    void onDisconnect(NimBLEClient* c, int reason) override {
        INFO printf("[BLE] Disconnected, reason: %d\n", reason);
        g_hr = 0;
        g_do_connect = false;
        g_need_scan = true;
    }
};

static MyBLECallbacks g_ble_handler;

/* =========================================================
 * 连接逻辑
 * ========================================================= */
bool ConnectToDevice() {
    NimBLEAddress addr;
    xSemaphoreTake(g_target_addr_mutex, portMAX_DELAY);
    addr = g_target_addr;
    xSemaphoreGive(g_target_addr_mutex);

    INFO printf("[CONN] Attempting to connect to %s\n", addr.toString().c_str());
    if (!g_client->connect(addr, false)) {
        ERROR printf("[CONN] Connection failed\n");
        return false;
    }

    INFO printf("[CONN] Connected, discovering services...\n");
    g_client->getServices(true);

    NimBLERemoteService* remote_svc = g_client->getService("180D");
    if (remote_svc) {
        NimBLERemoteCharacteristic* remote_char = remote_svc->getCharacteristic("2A37");
        if (remote_char && remote_char->canNotify()) {
            if (remote_char->subscribe(true, HrNotifyCallback)) {
                INFO printf("[CONN] HR service subscribed successfully\n");
                return true;
            }
        }
    }

    ERROR printf("[CONN] Service or characteristic not found\n");
    g_client->disconnect();
    return false;
}

/* =========================================================
 * Model 2 显示任务
 * ========================================================= */
void DisplayTask(void* arg) {
    g_display.displayBegin();
    g_display.brightness(g_brightness);

    char text_buffer[16];

    for (;;) {
        if (g_client && g_client->isConnected()) {
            // Model 2 使用 DisplayStr
            if (g_hr) {
                snprintf(text_buffer, sizeof(text_buffer), "%3d", g_hr);
            } else {
                snprintf(text_buffer, sizeof(text_buffer), "---");
            }
        } else if (g_do_connect) {
            snprintf(text_buffer, sizeof(text_buffer), "Con");
        } else {
            snprintf(text_buffer, sizeof(text_buffer), "Scn");
        }

        // Model 2 专用的显示函数：DisplayStr(字符串, 填充ASCII)
        // 注意：Model 2 库对字符位置映射比较严格
        g_display.DisplayStr(text_buffer, 0);

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* =========================================================
 * BLE 管理任务
 * ========================================================= */
void HrManagerTask(void* arg) {
    const uint32_t SCAN_DELAY_MS = 1000;

    for (;;) {
        if (g_do_connect && !g_client->isConnected()) {
            if (!ConnectToDevice()) {
                g_do_connect = false;
                g_need_scan = true;
                g_last_disconnect_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                ERROR printf("[MGR] Connection failed, back to scanning\n");
            }
        } else if (g_need_scan && !g_client->isConnected()) {
            uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (current_time - g_last_disconnect_time >= SCAN_DELAY_MS) {
                g_need_scan = false;
                INFO printf("[SCAN] Resuming scan...\n");
                NimBLEDevice::getScan()->start(0, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* =========================================================
 * 配置的保存和加载
 * ========================================================= */
static void SaveSettings() {
    // 参数2为 false 表示读写模式
    g_prefs.begin(k_pref_namespace, false);

    g_prefs.clear();

    g_prefs.putUChar("brightness", g_brightness);
    g_prefs.putUChar("verbose", g_verbose);

    g_prefs.putBool("al_en", g_enable_allowlist);
    xSemaphoreTake(g_allowlist_mutex, portMAX_DELAY);
    g_prefs.putInt("al_len", g_allowlist.size());
    int i = 0;
    for (const auto &mac : g_allowlist) {
        char key[10];
        snprintf(key, 10, "al_%d", i++);
        g_prefs.putString(key, mac.c_str());
    }
    xSemaphoreGive(g_allowlist_mutex);

    g_prefs.end(); // 关闭并保存

    INFO printf("Settings saved to NVS.\n");
}

static void LoadSettings() {
    // 参数2为 true 表示只读模式
    g_prefs.begin(k_pref_namespace, true);

    // 第二个参数是默认值。如果 NVS 中还没保存过该项，则返回此值。
    g_brightness = g_prefs.getUChar("brightness", 1);
    g_verbose = g_prefs.getUChar("verbose", 1);

    g_enable_allowlist = g_prefs.getBool("al_en", false);

    int al_len = g_prefs.getInt("al_len", 0);
    xSemaphoreTake(g_allowlist_mutex, portMAX_DELAY);
    for (int i = 0; i < al_len; i++) {
        char key[10];
        snprintf(key, 10, "al_%d", i);
        String mac = g_prefs.getString(key, "");
        if (mac.length() > 0) {
            g_allowlist.insert(mac.c_str());
        }
    }
    xSemaphoreGive(g_allowlist_mutex);

    g_prefs.end();
}

/* =========================================================
 * forth解释器任务及相关的词
 * ========================================================= */
static void forth_get_hr() {
    So(1);
    Push = (atl_int) g_hr;
}

static void forth_set_br() {
    Sl(1);
    g_brightness = (int)S0;
    Pop;

    if (g_brightness > 7) g_brightness = 7;
    g_display.brightness(g_brightness);
}

static void forth_get_br() {
    So(1);
    Push = (atl_int) g_brightness;
}

static void forth_set_verbose() {
    Sl(1);
    g_verbose = (int)S0;
    Pop;

    if (g_verbose > 2) g_verbose = 2;
}

static void forth_get_verbose() {
    So(1);
    Push = (atl_int) g_verbose;
}

static void forth_set_enable_allowlist() {
    Sl(1);
    g_enable_allowlist = (int)S0;
    Pop;

    if (g_enable_allowlist) g_enable_allowlist = 1;
}

static void forth_get_enable_allowlist() {
    So(1);
    Push = (atl_int) g_enable_allowlist;
}

static void forth_allowlist_list() {
    printf("mac-address allowlist\n");
    printf("---------------------\n");
    xSemaphoreTake(g_allowlist_mutex, portMAX_DELAY);
    for (auto &mac : g_allowlist) {
        printf("%s\n", mac.c_str());
    }
    xSemaphoreGive(g_allowlist_mutex);
}

static void forth_allowlist_insert() {
    Sl(1);
    Hpc(S0);
    xSemaphoreTake(g_allowlist_mutex, portMAX_DELAY);
    g_allowlist.insert((char *) S0);
    xSemaphoreGive(g_allowlist_mutex);
    Pop;
}

static void forth_allowlist_erase() {
    Sl(1);
    Hpc(S0);
    xSemaphoreTake(g_allowlist_mutex, portMAX_DELAY);
    g_allowlist.erase((char *)S0);
    xSemaphoreGive(g_allowlist_mutex);
    Pop;
}

static void forth_list_tasks() {
    // 1. 获取当前任务总数
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    // 2. 为任务状态数组分配内存
    TaskStatus_t *task_status_array = (TaskStatus_t *)pvPortMalloc(task_count * sizeof(TaskStatus_t));

    if (task_status_array != NULL) {
        // 3. 获取所有任务的状态信息
        // 第三个参数为 NULL 表示不计算 CPU 使用率百分比（需要额外配置计时器）
        task_count = uxTaskGetSystemState(task_status_array, task_count, NULL);

        printf("\n--- Task Debug Info ---\n");
        printf("%-16s %-10s %-10s %-10s %-10s\n", "Name", "State", "Priority", "StackMin", "Number");

        for (UBaseType_t x = 0; x < task_count; x++) {
            char state_char;
            switch (task_status_array[x].eCurrentState) {
                case eRunning:   state_char = 'X'; break; // 正在运行
                case eReady:     state_char = 'R'; break; // 就绪
                case eBlocked:   state_char = 'B'; break; // 阻塞
                case eSuspended: state_char = 'S'; break; // 挂起
                case eDeleted:   state_char = 'D'; break; // 已删除
                default:         state_char = '?'; break;
            }

            printf("%-16s %-10c %-10u %-10u %-10u\n",
                task_status_array[x].pcTaskName,
                state_char,
                (unsigned int)task_status_array[x].uxCurrentPriority,
                (unsigned int)task_status_array[x].usStackHighWaterMark, // 剩余堆栈最小值
                (unsigned int)task_status_array[x].xTaskNumber);
        }

        // 4. 释放内存
        vPortFree(task_status_array);
    } else {
        printf("Failed to allocate memory for task stats.\n");
    }
}

static void forth_pin_mode() {
    Sl(2);
    int mode = (int)S0;
    int pin = (int)S1;
    Pop2;

    pinMode(pin, mode);
}

static void forth_digital_write() {
    Sl(2);
    int val = (int)S0;
    int pin = (int)S1;
    Pop2;

    digitalWrite(pin, val);
}

static void forth_delay_ms() {
    Sl(1);
    int ms = (int)S0;
    Pop;

    vTaskDelay(pdMS_TO_TICKS(ms));
}

static struct primfcn my_primitives[] = {
    {"0HR", forth_get_hr},

    {"0BR!", forth_set_br},
    {"0BR@", forth_get_br},

    {"0VERB!", forth_set_verbose},
    {"0VERB@", forth_get_verbose},

    {"0ALEN!", forth_set_enable_allowlist},
    {"0ALEN@", forth_get_enable_allowlist},

    {"0AL?", forth_allowlist_list},
    {"0AL+", forth_allowlist_insert},
    {"0AL-", forth_allowlist_erase},

    {"0SAVE", SaveSettings},

    {"0PS", forth_list_tasks},
    {"0REBOOT", esp_restart},

    {"0MODE", forth_pin_mode},
    {"0PIN!", forth_digital_write},
    {"0MS", forth_delay_ms},

    {NULL, NULL}
};

static inline void flush_stdout() {
    fflush(stdout);
    fsync(fileno(stdout));
}

void ForthTask(void* arg) {
    char input_buffer[128];
    int idx = 0;

    // 初始化 Atlast 实例
    atl_init();
    atl_primdef(my_primitives);

    printf("[FORTH] Interpreter Ready.\n");
    printf("[FORTH] ");
    flush_stdout();

    for (;;) {
        if (Serial.available()) {
            char c = Serial.read();

            if (c == '\n') {
                input_buffer[idx] = '\0';
                if (idx > 0) {
                    printf(" ");
                    flush_stdout();
                    int ret = atl_eval(input_buffer);
                    if (ret == ATL_SNORM) {
                        printf(state || atl_comment ? "\n" : " ok\n");
                    } else if (ret == ATL_UNDEFINED) { // 错误信息没有换行的情况
                        printf("\n");
                    }
                    flush_stdout();
                } else {
                    printf("\n");
                    flush_stdout();
                }

                if (atl_comment) {
                    printf("(FORTH) ");
                } else if (state) {
                    printf("<FORTH> ");
                } else {
                    printf("[FORTH] ");
                }
                flush_stdout();
                idx = 0;
            } else if (!isprint(c)) {
                if (c == '\b') {
                    if (idx > 0) {
                        printf("\b \b");
                        flush_stdout();
                        idx--;
                    }
                }
            } else if (idx < sizeof(input_buffer) - 1) {
                input_buffer[idx++] = c;
                printf("%c", c);
                flush_stdout();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Atlast的键盘交互实现
extern "C" {
    int Keyhit_impl() {
        flush_stdout();

        if (Serial.available()) {
            return Serial.read();
        }

        taskYIELD();

        return 0;
    }
}

/* =========================================================
 * Setup & Loop
 * ========================================================= */
void setup() {
    Serial.begin(115200);

    INFO printf("\n[SYS] ESP32-C3 HR Monitor Starting...\n");

    // 加载配置
    LoadSettings();

    // 初始化蓝牙
    NimBLEDevice::init("C3_HR_MON");
    g_client = NimBLEDevice::createClient();
    g_client->setClientCallbacks(&g_ble_handler, false);

    // 配置扫描
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_ble_handler, false);
    scan->setInterval(150);
    scan->setWindow(100);
    scan->setDuplicateFilter(false);

    INFO printf("[SCAN] Initial scan started...\n");
    g_need_scan = true;
    g_last_disconnect_time = xTaskGetTickCount() * portTICK_PERIOD_MS - 1000;
    scan->start(0, false);

    // 创建 FreeRTOS 任务
    xTaskCreate(HrManagerTask, "hr_mgr", 4096, nullptr, 10, nullptr);
    xTaskCreate(DisplayTask,   "ds_mgr", 2048, nullptr,  5, nullptr);
    xTaskCreate(ForthTask,  "forth_cli", 4096, nullptr,  2, nullptr);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
