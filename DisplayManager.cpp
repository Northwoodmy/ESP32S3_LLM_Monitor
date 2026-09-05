/*
 * DisplayManager - 显示管理器实现
 * ESP32S3_LLM_Monitor 项目
 *
 * 该文件实现了显示管理功能，包括：
 * - 多页面UI管理和切换(UI1主题:待机/总览/DeepSeek/Kimi Code)
 * - 大模型用量数据实时显示
 * - 屏幕模式管理(常开/常关/定时/延时熄屏)
 * - 触摸活动检测与触摸唤醒
 * - 亮度渐变控制
 * - 天气和时间显示
 * - WiFi信息页面(三击手势)
 *
 * 技术特点：
 * - FreeRTOS任务调度
 * - 线程安全的消息队列
 * - LVGL UI组件管理
 * - SquareLine Studio生成的UI系统
 */

#include "DisplayManager.h"
#include "WiFiManager.h"
#include "ConfigStorage.h"
#include "PSRAMManager.h"
#include "TimeManager.h"
#include "WeatherManager.h"
#include "ui_helpers.h"
#include <WiFi.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 静态实例指针定义
DisplayManager* DisplayManager::s_instance = nullptr;

// === 文件本地辅助函数 ===

/**
 * @brief 在用量数据中按平台类型查找已启用的平台
 */
static const ProviderData* findProviderData(const LLMUsageData& data, ProviderType type) {
    for (int i = 0; i < 4; i++) {
        if (data.providers[i].type == type && data.providers[i].enabled) {
            return &data.providers[i];
        }
    }
    return nullptr;
}

/**
 * @brief 格式化token数量: 123 / 45.2K / 1.23M
 */
static void formatTokenCount(uint64_t tokens, char* buf, size_t len) {
    if (tokens >= 1000000ULL) {
        snprintf(buf, len, "%.2fM", (double)tokens / 1000000.0);
    } else if (tokens >= 1000ULL) {
        snprintf(buf, len, "%.1fK", (double)tokens / 1000.0);
    } else {
        snprintf(buf, len, "%llu", (unsigned long long)tokens);
    }
}

/**
 * @brief 计算配额使用百分比
 */
static int quotaPercent(int used, int limit) {
    if (limit <= 0) return 0;
    int pct = used * 100 / limit;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/**
 * @brief 统一的屏幕切换实现(供消息处理和WiFi返回使用)
 */
static bool switchToScreen(DisplayPage page, lv_scr_load_anim_t anim, uint32_t duration) {
    lv_obj_t** target = nullptr;
    void (*initFn)(void) = nullptr;

    switch (page) {
        case PAGE_HOME:      target = &ui_standbySCREEN;   initFn = &ui_standbySCREEN_screen_init;   break;
        case PAGE_OVERVIEW:  target = &ui_overviewSCREEN;  initFn = &ui_overviewSCREEN_screen_init;  break;
        case PAGE_DS_MAIN:   target = &ui_dsMainSCREEN;    initFn = &ui_dsMainSCREEN_screen_init;    break;
        case PAGE_DS_INFO:   target = &ui_dsInfoSCREEN;    initFn = &ui_dsInfoSCREEN_screen_init;    break;
        case PAGE_KIMI_MAIN: target = &ui_kimiMainSCREEN;  initFn = &ui_kimiMainSCREEN_screen_init;  break;
        case PAGE_KIMI_INFO: target = &ui_kimiInfoSCREEN;  initFn = &ui_kimiInfoSCREEN_screen_init;  break;
        default: return false;
    }

    if (!target || !(*target)) return false;
    _ui_screen_change(target, anim, duration, 0, initFn);
    return true;
}

/**
 * @brief 构造函数
 */
DisplayManager::DisplayManager()
    : m_initialized(false)
    , m_running(false)
    , m_taskHandle(nullptr)
    , m_messageQueue(nullptr)
    , m_lvglDriver(nullptr)
    , m_wifiManager(nullptr)
    , m_configStorage(nullptr)
    , m_psramManager(nullptr)
    , m_weatherManager(nullptr)
    , m_currentPage(PAGE_HOME)
    , m_brightness(80)
    , m_screen(nullptr)
    , m_screenMode(SCREEN_MODE_ALWAYS_ON)
    , m_screenStartHour(8)
    , m_screenStartMinute(0)
    , m_screenEndHour(22)
    , m_screenEndMinute(0)
    , m_screenTimeoutMinutes(10)
    , m_screenOn(true)
    , m_lastTouchTime(0)
    , m_lastScreenModeCheck(0)
    , m_wifiInfoScreen(nullptr)
    , m_wifiStatusLabel(nullptr)
    , m_wifiSSIDLabel(nullptr)
    , m_wifiIPLabel(nullptr)
    , m_previousPageForWiFi(PAGE_HOME)
    , m_wifiInfoDisplayActive(false)
    , m_wifiInfoPendingDestroy(false)
    , m_lastWiFiSwitchTime(0)
    , m_firstSwipeTime(0)
    , m_swipeCount(0)
    , m_fadingEnabled(true)
    , m_currentFadingBrightness(80)
    , m_targetFadingBrightness(80)
    , m_fadeStartTime(0)
    , m_fadeDuration(1000)
    , m_isFading(false)
    , m_fadeDirection(FADE_TO_ON)
{
    // 设置全局实例指针
    s_instance = this;

    // 初始化用量数据
    memset(&m_usageData, 0, sizeof(m_usageData));
    for (int i = 0; i < 4; i++) {
        m_usageData.providers[i].type = PROVIDER_NONE;
        m_usageData.providers[i].enabled = false;
        m_usageData.providers[i].valid = false;
        strcpy(m_usageData.providers[i].state, "N/A");
    }
    m_usageData.valid = false;

    printf("[DisplayManager] 显示管理器已创建（大模型用量监控）\n");
}

/**
 * @brief 析构函数
 */
DisplayManager::~DisplayManager() {
    stop();

    // 清理WiFi信息页面资源
    m_wifiInfoPendingDestroy = false;
    destroyWiFiInfoPage();

    // 清理消息队列
    if (m_messageQueue) {
        vQueueDelete(m_messageQueue);
        m_messageQueue = nullptr;
    }

    printf("[DisplayManager] 显示管理器已销毁\n");
}

/**
 * @brief 初始化显示管理器
 */
bool DisplayManager::init(LVGLDriver* lvgl_driver, WiFiManager* wifi_manager, ConfigStorage* config_storage, PSRAMManager* psram_manager, WeatherManager* weather_manager) {
    if (m_initialized) {
        printf("[DisplayManager] 警告：重复初始化\n");
        return true;
    }

    if (!lvgl_driver || !wifi_manager || !config_storage) {
        printf("[DisplayManager] 错误：无效的依赖参数\n");
        return false;
    }

    printf("[DisplayManager] 开始初始化显示管理器...\n");

    // 保存依赖对象
    m_lvglDriver = lvgl_driver;
    m_wifiManager = wifi_manager;
    m_configStorage = config_storage;
    m_psramManager = psram_manager;
    m_weatherManager = weather_manager;

    // 创建消息队列
    m_messageQueue = xQueueCreate(MESSAGE_QUEUE_SIZE, sizeof(DisplayMessage));
    if (!m_messageQueue) {
        printf("[DisplayManager] 错误：创建消息队列失败\n");
        return false;
    }

    // 检查LVGL驱动是否已初始化
    if (!m_lvglDriver->isInitialized()) {
        printf("[DisplayManager] 错误：LVGL驱动未初始化\n");
        return false;
    }

    // 获取LVGL锁并初始化UI系统
    if (m_lvglDriver->lock(5000)) {
        // 初始化UI1系统
        ui_init();

        // 获取主屏幕
        m_screen = lv_scr_act();
        if (!m_screen) {
            printf("[DisplayManager] 错误：获取主屏幕失败\n");
            m_lvglDriver->unlock();
            return false;
        }

        // 显示默认页面（待机屏幕）
        if (ui_standbySCREEN) {
            lv_scr_load(ui_standbySCREEN);
            printf("[DisplayManager] 显示默认页面：待机屏幕\n");
        }

        // 初始化时间和日期显示
        updateTimeDisplay();

        m_lvglDriver->unlock();

        printf("[DisplayManager] UI系统初始化完成\n");
    } else {
        printf("[DisplayManager] 错误：获取LVGL锁超时\n");
        return false;
    }

    // 从NVS加载保存的亮度设置
    if (m_configStorage->hasBrightnessConfigAsync(3000)) {
        m_brightness = m_configStorage->loadBrightnessAsync(3000);
        printf("[DisplayManager] 加载保存的亮度: %d%%\n", m_brightness);

        // 立即应用加载的亮度到硬件（初始化时不需要通过消息队列）
        setBrightnessImmediate(m_brightness);
    } else {
        printf("[DisplayManager] 使用默认亮度: %d%%\n", m_brightness);
    }

    // 加载屏幕模式配置
    if (!loadScreenModeConfig()) {
        printf("[DisplayManager] 警告：加载屏幕模式配置失败，使用默认配置\n");
    }

    // 初始化触摸时间
    m_lastTouchTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    m_lastScreenModeCheck = m_lastTouchTime;

    m_initialized = true;
    printf("[DisplayManager] 显示管理器初始化完成\n");
    return true;
}

/**
 * @brief 启动显示管理器任务
 */
bool DisplayManager::start() {
    if (!m_initialized) {
        printf("[DisplayManager] 错误：未初始化，无法启动任务\n");
        return false;
    }

    if (m_running) {
        printf("[DisplayManager] 警告：任务已在运行\n");
        return true;
    }

    // 在创建任务之前设置m_running = true，避免竞态条件
    m_running = true;

    if (m_psramManager && m_psramManager->isPSRAMAvailable()) {
        // 使用PSRAM栈创建任务
        m_taskHandle = m_psramManager->createTaskWithPSRAMStack(
            displayTaskEntry,           // 任务函数
            "DisplayManager",           // 任务名称
            TASK_STACK_SIZE,            // 栈大小
            this,                       // 任务参数
            TASK_PRIORITY,              // 任务优先级
            TASK_CORE                   // 运行核心
        );

        if (m_taskHandle == nullptr) {
            printf("[DisplayManager] 错误：创建PSRAM栈任务失败\n");
            m_running = false;
            return false;
        }

        printf("[DisplayManager] 显示管理器任务(PSRAM栈)已启动\n");
    } else {
        // 回退到SRAM栈创建任务
        BaseType_t result = xTaskCreatePinnedToCore(
            displayTaskEntry,           // 任务函数
            "DisplayManager",           // 任务名称
            TASK_STACK_SIZE,            // 栈大小
            this,                       // 任务参数
            TASK_PRIORITY,              // 任务优先级
            &m_taskHandle,              // 任务句柄
            TASK_CORE                   // 运行核心
        );

        if (result != pdPASS) {
            printf("[DisplayManager] 错误：创建SRAM栈任务失败\n");
            m_running = false;
            return false;
        }

        printf("[DisplayManager] 显示管理器任务(SRAM栈)已启动\n");
    }
    return true;
}

/**
 * @brief 停止显示管理器任务
 */
void DisplayManager::stop() {
    if (!m_running) {
        return;
    }

    m_running = false;

    // 等待任务结束
    if (m_taskHandle) {
        vTaskDelete(m_taskHandle);
        m_taskHandle = nullptr;
        printf("[DisplayManager] 显示管理器任务已停止\n");
    }
}

/**
 * @brief 显示管理器任务静态入口
 */
void DisplayManager::displayTaskEntry(void* arg) {
    DisplayManager* manager = static_cast<DisplayManager*>(arg);
    if (manager) {
        manager->displayTask();
    }
    vTaskDelete(nullptr);
}

/**
 * @brief 显示管理器任务执行函数
 */
void DisplayManager::displayTask() {
    printf("[DisplayManager] 显示管理器任务开始运行\n");

    DisplayMessage msg;
    TickType_t lastUpdateTime = 0;
    const TickType_t updateInterval = pdMS_TO_TICKS(1000); // 1秒更新间隔

    while (m_running) {
        // 处理消息队列中的消息
        BaseType_t queueResult = xQueueReceive(m_messageQueue, &msg, pdMS_TO_TICKS(100));
        if (queueResult == pdTRUE) {
            processMessage(msg);
        }

        // 定期更新时间显示和屏幕模式检查
        TickType_t currentTime = xTaskGetTickCount();
        if (currentTime - lastUpdateTime >= updateInterval) {
            // 更新时间显示
            updateTimeDisplay();

            // 更新天气显示
            updateWeatherDisplay();

            // 处理屏幕模式管理逻辑
            processScreenModeLogic();

            // 检查三击手势超时
            checkTripleSwipeTimeout();

            // 处理WiFi信息页面的延迟销毁
            if (m_wifiInfoPendingDestroy) {
                // 确保WiFi信息页面对象存在且显示状态已清除
                if (m_wifiInfoScreen && !m_wifiInfoDisplayActive) {
                    printf("[DisplayManager] 通过消息队列安全销毁WiFi信息页面\n");

                    DisplayMessage destroyMsg;
                    destroyMsg.type = DisplayMessage::MSG_DESTROY_WIFI_INFO;

                    if (xQueueSend(m_messageQueue, &destroyMsg, pdMS_TO_TICKS(100)) == pdTRUE) {
                        m_wifiInfoPendingDestroy = false;
                    } else {
                        printf("[DisplayManager] 错误：发送WiFi信息页面销毁消息失败\n");
                    }
                } else if (!m_wifiInfoScreen) {
                    // 对象已经不存在，清除标志
                    m_wifiInfoPendingDestroy = false;
                }
            }

            lastUpdateTime = currentTime;
        }

        // 处理亮度渐变（更高频率更新以确保平滑渐变）
        if (m_isFading && m_fadingEnabled) {
            processFading();
        }

        // 渐变期间使用更短的延迟以获得更平滑的效果
        if (m_isFading && m_fadingEnabled) {
            vTaskDelay(pdMS_TO_TICKS(10)); // 渐变时10ms更新间隔
        } else {
            vTaskDelay(pdMS_TO_TICKS(50)); // 正常时50ms延迟
        }
    }

    printf("[DisplayManager] 显示管理器任务结束\n");
}

/**
 * @brief 处理显示消息
 */
void DisplayManager::processMessage(const DisplayMessage& msg) {
    // 特殊处理：亮度调整不需要LVGL锁，直接处理避免死锁
    if (msg.type == DisplayMessage::MSG_SET_BRIGHTNESS) {
        processBrightnessMessage(msg);
        return;
    }

    if (!m_lvglDriver->lock(1000)) {
        printf("[DisplayManager] 警告：处理消息时获取LVGL锁失败\n");
        return;
    }

    switch (msg.type) {
        case DisplayMessage::MSG_UPDATE_WIFI_STATUS:
            // 更新WiFi状态显示
            printf("[DisplayManager] 更新WiFi状态：%s\n",
                   msg.data.wifi_status.connected ? "已连接" : "未连接");
            break;

        case DisplayMessage::MSG_UPDATE_SYSTEM_INFO:
            // 更新系统信息显示
            printf("[DisplayManager] 更新系统信息：内存=%d KB，运行时间=%d秒\n",
                   msg.data.system_info.free_heap / 1024,
                   msg.data.system_info.uptime);
            break;

        case DisplayMessage::MSG_UPDATE_USAGE_DATA:
            // 用量数据已在updateUsageData中直接更新显示
            break;

        case DisplayMessage::MSG_UPDATE_WEATHER_DATA:
            // 天气数据已在updateWeatherDisplay中处理
            if (!msg.data.weather_data.valid) {
                printf("[DisplayManager] 天气数据无效\n");
            }
            break;

        case DisplayMessage::MSG_SWITCH_PAGE:
            // 切换页面
            if (msg.data.page_switch.page < PAGE_COUNT) {
                DisplayPage newPage = msg.data.page_switch.page;

                // 检查是否从WiFi信息页面切换出去，需要清理状态
                if (m_currentPage == PAGE_WIFI_STATUS && newPage != PAGE_WIFI_STATUS && m_wifiInfoDisplayActive) {
                    printf("[DisplayManager] 从WiFi信息页面自动切换到页面：%d，清理WiFi页面状态\n", newPage);
                    m_wifiInfoDisplayActive = false;
                    m_wifiInfoPendingDestroy = true;
                }

                // 当切换到待机页面时，重置三击手势状态
                if (newPage == PAGE_HOME && m_swipeCount > 0) {
                    printf("[DisplayManager] 切换到待机页面，重置三击手势状态\n");
                    m_swipeCount = 0;
                    m_firstSwipeTime = 0;
                }

                if (newPage == PAGE_WIFI_STATUS) {
                    // WiFi信息页面应该通过showWiFiInfoPage()调用，而不是switchPage()
                    printf("[DisplayManager] 警告：PAGE_WIFI_STATUS应该通过showWiFiInfoPage()调用\n");
                } else if (switchToScreen(newPage, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500)) {
                    m_currentPage = newPage;
                    printf("[DisplayManager] 切换到页面：%d\n", m_currentPage);
                } else {
                    printf("[DisplayManager] 警告：页面%d切换失败\n", newPage);
                }
            }
            break;

        case DisplayMessage::MSG_SHOW_NOTIFICATION:
            // 显示通知（暂时简化为控制台输出）
            printf("[DisplayManager] 通知：%s\n", msg.data.notification.text);
            break;

        case DisplayMessage::MSG_SCREEN_MODE_CHANGED:
            // 屏幕模式变更
            m_screenMode = msg.data.screen_mode.mode;
            m_screenStartHour = msg.data.screen_mode.startHour;
            m_screenStartMinute = msg.data.screen_mode.startMinute;
            m_screenEndHour = msg.data.screen_mode.endHour;
            m_screenEndMinute = msg.data.screen_mode.endMinute;
            m_screenTimeoutMinutes = msg.data.screen_mode.timeoutMinutes;

            // 设置自动旋转
            if (m_lvglDriver) {
                m_lvglDriver->setAutoRotationEnabled(msg.data.screen_mode.autoRotationEnabled);
                // 如果自动旋转被禁用，应用静态旋转角度
                if (!msg.data.screen_mode.autoRotationEnabled) {
                    m_lvglDriver->setScreenRotation((screen_rotation_t)msg.data.screen_mode.staticRotation);
                    printf("[DisplayManager] 应用静态旋转角度: %d度\n", msg.data.screen_mode.staticRotation * 90);
                }
                printf("[DisplayManager] 自动旋转已设置为: %s\n",
                       msg.data.screen_mode.autoRotationEnabled ? "启用" : "禁用");
            }

            printf("[DisplayManager] 屏幕模式已更改: 模式=%d, 时间=%02d:%02d-%02d:%02d, 延时=%d分钟\n",
                   m_screenMode, m_screenStartHour, m_screenStartMinute,
                   m_screenEndHour, m_screenEndMinute, m_screenTimeoutMinutes);

            // 重新评估屏幕状态
            processScreenModeLogic();
            break;

        case DisplayMessage::MSG_TOUCH_ACTIVITY:
            // 触摸活动
            m_lastTouchTime = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // 如果屏幕当前关闭，触摸活动应立即开启屏幕
            if (!m_screenOn) {
                performScreenOn();
            }
            break;

        case DisplayMessage::MSG_SCREEN_ON:
            // 强制开启屏幕
            performScreenOn();
            break;

        case DisplayMessage::MSG_SCREEN_OFF:
            // 强制关闭屏幕
            performScreenOff();
            break;

        case DisplayMessage::MSG_SHOW_WIFI_INFO:
            // 显示WiFi信息页面
            printf("[DisplayManager] 处理显示WiFi信息页面消息，当前页面：%d\n", m_currentPage);

            // 保存当前页面，用于返回（在修改m_currentPage之前保存）
            m_previousPageForWiFi = m_currentPage;
            m_wifiInfoDisplayActive = true;

            // 直接创建WiFi信息页面（此时已在正确的LVGL上下文中）
            createWiFiInfoPage();
            updateWiFiInfoDisplay();

            // 最后设置当前页面
            m_currentPage = PAGE_WIFI_STATUS;

            printf("[DisplayManager] WiFi信息页面已显示，保存的返回页面：%d\n", m_previousPageForWiFi);
            break;

        case DisplayMessage::MSG_RETURN_FROM_WIFI_INFO:
            // 从WiFi信息页面返回
            printf("[DisplayManager] 处理从WiFi信息页面返回消息\n");

            if (m_wifiInfoDisplayActive) {
                m_wifiInfoDisplayActive = false;

                printf("[DisplayManager] 恢复到WiFi信息页面前的页面：%d\n", m_previousPageForWiFi);

                DisplayPage targetPage = m_previousPageForWiFi;
                if (targetPage == PAGE_WIFI_STATUS || targetPage >= PAGE_COUNT) {
                    targetPage = PAGE_HOME;
                }

                if (switchToScreen(targetPage, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300)) {
                    m_currentPage = targetPage;
                    printf("[DisplayManager] 成功切换到页面：%d\n", m_currentPage);
                } else {
                    printf("[DisplayManager] 错误：页面切换失败\n");
                }

                // 标记为等待销毁，在主循环中延迟处理
                m_wifiInfoPendingDestroy = true;
            }
            break;

        case DisplayMessage::MSG_DESTROY_WIFI_INFO:
            // 安全销毁WiFi信息页面（已在LVGL锁保护下）
            destroyWiFiInfoPage();
            break;
    }

    m_lvglDriver->unlock();
}

/**
 * @brief 处理亮度调整消息（无锁版本）
 *
 * 亮度调整不需要LVGL锁保护，因为它直接操作硬件寄存器，
 * 将其独立处理可以避免与屏幕切换等需要长时间持有LVGL锁的操作产生死锁。
 */
void DisplayManager::processBrightnessMessage(const DisplayMessage& msg) {
    // 更新内部亮度值
    m_brightness = msg.data.brightness.brightness;

    // 异步保存亮度到NVS
    if (m_configStorage) {
        bool saveSuccess = m_configStorage->saveBrightnessAsync(m_brightness, 3000);
        if (saveSuccess) {
            printf("[DisplayManager] 亮度设置已保存: %d%%\n", m_brightness);
        } else {
            printf("[DisplayManager] 亮度设置保存失败\n");
        }
    }

    // 直接应用亮度到硬件（不需要LVGL锁）
    if (m_lvglDriver) {
        m_lvglDriver->setBrightness(m_brightness);
    } else {
        printf("[DisplayManager] 错误：LVGL驱动未初始化，无法设置亮度\n");
    }
}

/**
 * @brief 立即设置亮度（不通过消息队列，用于内部调用）
 */
void DisplayManager::setBrightnessImmediate(uint8_t brightness) {
    // 直接应用亮度到硬件（不需要LVGL锁）
    if (m_lvglDriver) {
        m_lvglDriver->setBrightness(brightness);
    } else {
        printf("[DisplayManager] 错误：LVGL驱动未初始化，无法设置亮度\n");
    }
}

// === 公共接口实现 ===

void DisplayManager::switchPage(DisplayPage page) {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_SWITCH_PAGE;
    msg.data.page_switch.page = page;

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

/**
 * @brief 手动切换页面
 */
void DisplayManager::manualSwitchPage(DisplayPage page) {
    printf("[DisplayManager] 手动切换到页面：%d\n", page);
    switchPage(page);
}

void DisplayManager::updateWiFiStatus(bool connected, const char* ssid, const char* ip, int rssi) {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_UPDATE_WIFI_STATUS;
    msg.data.wifi_status.connected = connected;
    msg.data.wifi_status.rssi = rssi;

    if (ssid) {
        strncpy(msg.data.wifi_status.ssid, ssid, sizeof(msg.data.wifi_status.ssid) - 1);
        msg.data.wifi_status.ssid[sizeof(msg.data.wifi_status.ssid) - 1] = '\0';
    }

    if (ip) {
        strncpy(msg.data.wifi_status.ip, ip, sizeof(msg.data.wifi_status.ip) - 1);
        msg.data.wifi_status.ip[sizeof(msg.data.wifi_status.ip) - 1] = '\0';
    }

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

void DisplayManager::updateSystemInfo(uint32_t free_heap, uint32_t uptime, float cpu_usage) {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_UPDATE_SYSTEM_INFO;
    msg.data.system_info.free_heap = free_heap;
    msg.data.system_info.uptime = uptime;
    msg.data.system_info.cpu_usage = cpu_usage;

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

void DisplayManager::setBrightness(uint8_t brightness) {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_SET_BRIGHTNESS;
    msg.data.brightness.brightness = brightness;

    if (m_messageQueue) {
        BaseType_t result = xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
        if (result != pdTRUE) {
            printf("[DisplayManager] 亮度设置消息发送失败\n");
        }
    }
}

void DisplayManager::showNotification(const char* text, uint32_t duration_ms) {
    if (!text) return;

    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_SHOW_NOTIFICATION;
    msg.data.notification.duration_ms = duration_ms;

    strncpy(msg.data.notification.text, text, sizeof(msg.data.notification.text) - 1);
    msg.data.notification.text[sizeof(msg.data.notification.text) - 1] = '\0';

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

// 获取器实现
uint8_t DisplayManager::getBrightness() const {
    return m_brightness;
}

DisplayPage DisplayManager::getCurrentPage() const {
    return m_currentPage;
}

bool DisplayManager::isInitialized() const {
    return m_initialized;
}

bool DisplayManager::isRunning() const {
    return m_running;
}

/**
 * @brief 更新大模型用量数据
 */
void DisplayManager::updateUsageData(const LLMUsageData& usage_data) {
    // 直接更新内部数据
    m_usageData = usage_data;

    // 立即更新显示
    updateUsageDataDisplay();

    // 同时发送消息到队列进行后续处理
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_UPDATE_USAGE_DATA;
    msg.data.usage_monitor.usage_data = usage_data;

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

void DisplayManager::updateWeatherData(const char* temperature, const char* weather) {
    if (!temperature || !weather) {
        return;
    }

    // 立即更新UI显示
    updateWeatherDisplay();

    // 发送消息到队列进行后续处理
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_UPDATE_WEATHER_DATA;

    strncpy(msg.data.weather_data.temperature, temperature, sizeof(msg.data.weather_data.temperature) - 1);
    msg.data.weather_data.temperature[sizeof(msg.data.weather_data.temperature) - 1] = '\0';

    strncpy(msg.data.weather_data.weather, weather, sizeof(msg.data.weather_data.weather) - 1);
    msg.data.weather_data.weather[sizeof(msg.data.weather_data.weather) - 1] = '\0';

    msg.data.weather_data.valid = true;

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

const LLMUsageData& DisplayManager::getCurrentUsageData() const {
    return m_usageData;
}

/**
 * @brief 更新时间显示
 */
void DisplayManager::updateTimeDisplay() {
    if (!m_lvglDriver || !m_lvglDriver->lock(100)) {
        return;
    }

    // 获取当前时间
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);

    if (ui_timeLabel) {
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
        lv_label_set_text(ui_timeLabel, time_str);
    }

    if (ui_dataLabel) {
        char date_str[16];
        strftime(date_str, sizeof(date_str), "%m-%d", timeinfo);
        lv_label_set_text(ui_dataLabel, date_str);
    }

    if (ui_weekLabel) {
        const char* weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        lv_label_set_text(ui_weekLabel, weekdays[timeinfo->tm_wday]);
    }

    m_lvglDriver->unlock();
}

/**
 * @brief 更新用量数据显示（调用前需已持有LVGL锁）
 *
 * 页面布局:
 * - 总览页: 总消耗 + 4槽位(DS余额/DS今日消耗/Kimi 5h/Kimi 7d)
 * - DeepSeek主数据页: 余额大数字 + 今日消耗/估算Tokens/累计消耗
 * - DeepSeek详情页: 状态/套餐/现金/代金券/单价/更新时间
 * - Kimi主数据页: 7天用量%大数字 + 5h%/7d重置/5h重置
 * - Kimi详情页: 状态/套餐/7d原始值/5h原始值/7d重置/更新时间
 */
void DisplayManager::updateUsageDataDisplay() {
    if (!m_lvglDriver || !m_lvglDriver->lock(100)) {
        return;
    }

    char buf[40];

    // === 待机页: 今日总消耗 ===
    if (ui_costLabel) {
        if (m_usageData.valid) {
            snprintf(buf, sizeof(buf), "Cost: %.2f CNY", m_usageData.total_cost_today);
        } else {
            snprintf(buf, sizeof(buf), "Cost: -- CNY");
        }
        lv_label_set_text(ui_costLabel, buf);
    }

    // === 总览页: 今日总消耗 ===
    if (ui_totalcostlabel) {
        if (m_usageData.valid) {
            snprintf(buf, sizeof(buf), "CNY %.2f", m_usageData.total_cost_today);
        } else {
            snprintf(buf, sizeof(buf), "CNY --");
        }
        lv_label_set_text(ui_totalcostlabel, buf);
    }

    // === 总览页: 4个槽位 ===
    const ProviderData* ds = findProviderData(m_usageData, PROVIDER_DEEPSEEK);
    const ProviderData* kimi = findProviderData(m_usageData, PROVIDER_KIMICODE);

    // 槽位1: DeepSeek 余额
    if (ui_slot1value) {
        if (ds && ds->valid) {
            snprintf(buf, sizeof(buf), "%.2f", ds->balance);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        lv_label_set_text(ui_slot1value, buf);
    }

    // 槽位2: DeepSeek 今日消耗
    if (ui_slot2value) {
        if (ds && ds->valid) {
            snprintf(buf, sizeof(buf), "%.2f", ds->cost_today);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        lv_label_set_text(ui_slot2value, buf);
    }

    // 槽位3: Kimi Code 5小时窗口用量百分比
    if (ui_slot3value) {
        if (kimi && kimi->valid) {
            snprintf(buf, sizeof(buf), "%d%%", quotaPercent(kimi->win5h_used, kimi->win5h_limit));
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        lv_label_set_text(ui_slot3value, buf);
    }

    // 槽位4: Kimi Code 7天用量百分比
    if (ui_slot4value) {
        if (kimi && kimi->valid) {
            snprintf(buf, sizeof(buf), "%d%%", quotaPercent(kimi->weekly_used, kimi->weekly_limit));
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        lv_label_set_text(ui_slot4value, buf);
    }

    // 进度条
    updateUsageBars();

    // 详情页面
    updateDeepSeekPages();
    updateKimiPages();

    m_lvglDriver->unlock();
}

/**
 * @brief 更新总览页槽位进度条（调用前需已持有LVGL锁）
 *
 * 进度条刻度:
 * - 槽位1(余额): 100 CNY 满刻度
 * - 槽位2(今日消耗): 10 CNY/天 满刻度
 * - 槽位3/4(配额): 使用百分比
 */
void DisplayManager::updateUsageBars() {
    const ProviderData* ds = findProviderData(m_usageData, PROVIDER_DEEPSEEK);
    const ProviderData* kimi = findProviderData(m_usageData, PROVIDER_KIMICODE);

    if (ui_slot1bar) {
        int v = 0;
        if (ds && ds->valid) {
            v = (int)ds->balance;   // 100 CNY = 满格
            if (v > 100) v = 100;
            if (v < 0) v = 0;
        }
        lv_bar_set_value(ui_slot1bar, v, LV_ANIM_OFF);
    }

    if (ui_slot2bar) {
        int v = 0;
        if (ds && ds->valid) {
            v = (int)(ds->cost_today / 10.0 * 100.0);   // 10 CNY/天 = 满格
            if (v > 100) v = 100;
            if (v < 0) v = 0;
        }
        lv_bar_set_value(ui_slot2bar, v, LV_ANIM_OFF);
    }

    if (ui_slot3bar) {
        int v = (kimi && kimi->valid) ? quotaPercent(kimi->win5h_used, kimi->win5h_limit) : 0;
        lv_bar_set_value(ui_slot3bar, v, LV_ANIM_OFF);
    }

    if (ui_slot4bar) {
        int v = (kimi && kimi->valid) ? quotaPercent(kimi->weekly_used, kimi->weekly_limit) : 0;
        lv_bar_set_value(ui_slot4bar, v, LV_ANIM_OFF);
    }
}

/**
 * @brief 更新DeepSeek页面显示（调用前需已持有LVGL锁）
 */
void DisplayManager::updateDeepSeekPages() {
    const ProviderData* ds = findProviderData(m_usageData, PROVIDER_DEEPSEEK);
    char buf[40];
    bool ok = (ds && ds->valid);

    // === 主数据页 ===
    if (ui_dsMainValue) {
        if (ok) snprintf(buf, sizeof(buf), "%.2f", ds->balance);
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_dsMainValue, buf);
    }
    if (ui_dsCostToday) {
        if (ok) snprintf(buf, sizeof(buf), "CNY %.2f", ds->cost_today);
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_dsCostToday, buf);
    }
    if (ui_dsTokens) {
        if (ok) formatTokenCount(ds->est_tokens_today, buf, sizeof(buf));
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_dsTokens, buf);
    }
    if (ui_dsCostTotal) {
        if (ok) snprintf(buf, sizeof(buf), "CNY %.2f", ds->cost_total);
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_dsCostTotal, buf);
    }

    // === 详情页 ===
    if (ui_dsState) {
        if (ds) snprintf(buf, sizeof(buf), "%s", ds->state);
        else    snprintf(buf, sizeof(buf), "N/A");
        lv_label_set_text(ui_dsState, buf);
    }
    if (ui_dsPlan) {
        lv_label_set_text(ui_dsPlan, ds ? "Pay-as-you-go" : "N/A");
    }
    if (ui_dsCash) {
        if (ok) snprintf(buf, sizeof(buf), "%.2f", ds->cash_balance);
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_dsCash, buf);
    }
    if (ui_dsVoucher) {
        if (ok) snprintf(buf, sizeof(buf), "%.2f", ds->voucher_balance);
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_dsVoucher, buf);
    }
    if (ui_dsUnitPrice) {
        if (ds && ds->unit_price > 0) snprintf(buf, sizeof(buf), "%.1f/M", ds->unit_price);
        else                          snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_dsUnitPrice, buf);
    }
    if (ui_dsUpdated) {
        if (ok && ds->updated[0]) snprintf(buf, sizeof(buf), "%s", ds->updated);
        else                      snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_dsUpdated, buf);
    }
}

/**
 * @brief 更新Kimi Code页面显示（调用前需已持有LVGL锁）
 */
void DisplayManager::updateKimiPages() {
    const ProviderData* kimi = findProviderData(m_usageData, PROVIDER_KIMICODE);
    char buf[40];
    bool ok = (kimi && kimi->valid);

    // === 主数据页 ===
    if (ui_kimiWeeklyValue) {
        if (ok) snprintf(buf, sizeof(buf), "%d%%", quotaPercent(kimi->weekly_used, kimi->weekly_limit));
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_kimiWeeklyValue, buf);
    }
    if (ui_kimiWin5h) {
        if (ok) snprintf(buf, sizeof(buf), "%d%%", quotaPercent(kimi->win5h_used, kimi->win5h_limit));
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_kimiWin5h, buf);
    }
    if (ui_kimiWeeklyReset) {
        if (ok && kimi->weekly_reset[0]) snprintf(buf, sizeof(buf), "%s", kimi->weekly_reset);
        else                             snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_kimiWeeklyReset, buf);
    }
    if (ui_kimiWin5hReset) {
        if (ok && kimi->win5h_reset[0]) snprintf(buf, sizeof(buf), "%s", kimi->win5h_reset);
        else                            snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_kimiWin5hReset, buf);
    }

    // === 详情页 ===
    if (ui_kimiState) {
        if (kimi) snprintf(buf, sizeof(buf), "%s", kimi->state);
        else      snprintf(buf, sizeof(buf), "N/A");
        lv_label_set_text(ui_kimiState, buf);
    }
    if (ui_kimiPlan) {
        lv_label_set_text(ui_kimiPlan, kimi ? "Coding Plan" : "N/A");
    }
    if (ui_kimiWeeklyRaw) {
        if (ok) snprintf(buf, sizeof(buf), "%d/%d", kimi->weekly_used, kimi->weekly_limit);
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_kimiWeeklyRaw, buf);
    }
    if (ui_kimiWin5hRaw) {
        if (ok) snprintf(buf, sizeof(buf), "%d/%d", kimi->win5h_used, kimi->win5h_limit);
        else    snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_kimiWin5hRaw, buf);
    }
    if (ui_kimiWeeklyResetAt) {
        if (ok && kimi->weekly_reset[0]) snprintf(buf, sizeof(buf), "%s", kimi->weekly_reset);
        else                             snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_kimiWeeklyResetAt, buf);
    }
    if (ui_kimiUpdated) {
        if (ok && kimi->updated[0]) snprintf(buf, sizeof(buf), "%s", kimi->updated);
        else                        snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(ui_kimiUpdated, buf);
    }
}

/**
 * @brief 更新天气显示
 */
void DisplayManager::updateWeatherDisplay() {
    if (!m_lvglDriver || !m_lvglDriver->lock(100)) {
        return;
    }

    // 如果没有天气管理器，跳过更新
    if (!m_weatherManager) {
        m_lvglDriver->unlock();
        return;
    }

    // 获取当前天气数据
    auto currentWeather = m_weatherManager->getCurrentWeather();

    if (currentWeather.isValid) {
        if (ui_temperatureLabel) {
            char temp_str[16];
            snprintf(temp_str, sizeof(temp_str), "%s度", currentWeather.temperature.c_str());
            lv_label_set_text(ui_temperatureLabel, temp_str);
        }

        if (ui_weatherLabel) {
            lv_label_set_text(ui_weatherLabel, currentWeather.weather.c_str());
        }
    } else {
        // 天气数据无效时显示默认值
        if (ui_temperatureLabel) {
            lv_label_set_text(ui_temperatureLabel, "--度");
        }

        if (ui_weatherLabel) {
            lv_label_set_text(ui_weatherLabel, "--");
        }
    }

    m_lvglDriver->unlock();
}

// === 屏幕模式管理功能实现 ===

/**
 * @brief 加载屏幕模式配置
 */
bool DisplayManager::loadScreenModeConfig() {
    if (!m_configStorage) {
        printf("[DisplayManager] 错误：配置存储未初始化\n");
        return false;
    }

    if (!m_configStorage->hasScreenConfigAsync(3000)) {
        printf("[DisplayManager] 未找到屏幕模式配置，使用默认配置\n");
        return false;
    }

    ScreenMode mode;
    int startHour, startMinute, endHour, endMinute, timeoutMinutes;
    bool autoRotationEnabled;
    int staticRotation;

    if (m_configStorage->loadScreenConfigAsync(mode, startHour, startMinute,
                                             endHour, endMinute, timeoutMinutes, autoRotationEnabled, staticRotation, 3000)) {
        m_screenMode = mode;
        m_screenStartHour = startHour;
        m_screenStartMinute = startMinute;
        m_screenEndHour = endHour;
        m_screenEndMinute = endMinute;
        m_screenTimeoutMinutes = timeoutMinutes;

        // 设置自动旋转功能
        if (m_lvglDriver) {
            m_lvglDriver->setAutoRotationEnabled(autoRotationEnabled);
            // 如果自动旋转被禁用，应用静态旋转角度
            if (!autoRotationEnabled) {
                m_lvglDriver->setScreenRotation((screen_rotation_t)staticRotation);
                printf("[DisplayManager] 应用静态旋转角度: %d度\n", staticRotation * 90);
            }
            printf("[DisplayManager] 自动旋转配置已加载并应用: %s\n",
                   autoRotationEnabled ? "启用" : "禁用");
        }

        printf("[DisplayManager] 屏幕模式配置加载成功：模式=%d, 时间=%02d:%02d-%02d:%02d, 延时=%d分钟, 自动旋转=%s, 静态旋转=%d度\n",
               m_screenMode, m_screenStartHour, m_screenStartMinute,
               m_screenEndHour, m_screenEndMinute, m_screenTimeoutMinutes,
               autoRotationEnabled ? "启用" : "禁用", staticRotation * 90);

        return true;
    } else {
        printf("[DisplayManager] 屏幕模式配置加载失败\n");
        return false;
    }
}

/**
 * @brief 设置屏幕模式
 */
void DisplayManager::setScreenMode(ScreenMode mode, int startHour, int startMinute,
                                 int endHour, int endMinute, int timeoutMinutes, bool autoRotationEnabled) {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_SCREEN_MODE_CHANGED;
    msg.data.screen_mode.mode = mode;
    msg.data.screen_mode.startHour = startHour;
    msg.data.screen_mode.startMinute = startMinute;
    msg.data.screen_mode.endHour = endHour;
    msg.data.screen_mode.endMinute = endMinute;
    msg.data.screen_mode.timeoutMinutes = timeoutMinutes;
    msg.data.screen_mode.autoRotationEnabled = autoRotationEnabled;

    // 如果自动旋转被禁用，需要获取当前旋转角度并保存
    if (!autoRotationEnabled && m_lvglDriver) {
        msg.data.screen_mode.staticRotation = (int)m_lvglDriver->getScreenRotation();
        printf("[DisplayManager] 保存当前旋转角度: %d度\n", msg.data.screen_mode.staticRotation * 90);
    } else {
        msg.data.screen_mode.staticRotation = 0; // 默认为0度
    }

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

/**
 * @brief 获取当前屏幕模式
 */
ScreenMode DisplayManager::getScreenMode() const {
    return m_screenMode;
}

/**
 * @brief 检查屏幕是否应该开启
 */
bool DisplayManager::shouldScreenBeOn() const {
    switch (m_screenMode) {
        case SCREEN_MODE_ALWAYS_ON:
            return true;

        case SCREEN_MODE_ALWAYS_OFF:
            return false;

        case SCREEN_MODE_SCHEDULED:
            return isInScheduledTime();

        case SCREEN_MODE_TIMEOUT:
            return !isTimeoutExpired();

        default:
            return true;
    }
}

/**
 * @brief 强制开启屏幕
 */
void DisplayManager::forceScreenOn() {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_SCREEN_ON;

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

/**
 * @brief 强制关闭屏幕
 */
void DisplayManager::forceScreenOff() {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_SCREEN_OFF;

    if (m_messageQueue) {
        xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
    }
}

/**
 * @brief 通知触摸活动
 */
void DisplayManager::notifyTouchActivity() {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_TOUCH_ACTIVITY;

    if (m_messageQueue) {
        BaseType_t result = xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100));
        if (result != pdTRUE) {
            printf("[DisplayManager] 警告：触摸活动消息发送失败\n");
        }
    } else {
        printf("[DisplayManager] 错误：消息队列为空\n");
    }
}

/**
 * @brief 检查屏幕是否开启
 */
bool DisplayManager::isScreenOn() const {
    return m_screenOn;
}

/**
 * @brief 处理屏幕模式逻辑
 */
void DisplayManager::processScreenModeLogic() {
    uint32_t currentTime = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // 检查是否到了处理时间
    if (currentTime - m_lastScreenModeCheck < SCREEN_MODE_CHECK_INTERVAL) {
        return;
    }

    m_lastScreenModeCheck = currentTime;

    // 根据屏幕模式决定屏幕状态
    bool shouldBeOn = shouldScreenBeOn();

    if (shouldBeOn && !m_screenOn) {
        performScreenOn();
    } else if (!shouldBeOn && m_screenOn) {
        performScreenOff();
    }
}

/**
 * @brief 检查定时模式是否应该开启屏幕
 */
bool DisplayManager::isInScheduledTime() const {
    // 获取当前时间
    time_t now;
    time(&now);
    struct tm* timeinfo = localtime(&now);

    int currentHour = timeinfo->tm_hour;
    int currentMinute = timeinfo->tm_min;

    // 计算当前时间的分钟数（从00:00开始）
    int currentMinutes = currentHour * 60 + currentMinute;
    int startMinutes = m_screenStartHour * 60 + m_screenStartMinute;
    int endMinutes = m_screenEndHour * 60 + m_screenEndMinute;

    // 处理跨天的情况
    if (startMinutes <= endMinutes) {
        // 同一天内的时间段
        return currentMinutes >= startMinutes && currentMinutes <= endMinutes;
    } else {
        // 跨天的时间段（例如：22:00 - 08:00）
        return currentMinutes >= startMinutes || currentMinutes <= endMinutes;
    }
}

/**
 * @brief 检查延时模式是否应该关闭屏幕(基于触摸活动)
 */
bool DisplayManager::isTimeoutExpired() const {
    uint32_t currentTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t timeoutMs = m_screenTimeoutMinutes * 60 * 1000;
    uint32_t timeSinceLastTouch = currentTime - m_lastTouchTime;

    return timeSinceLastTouch > timeoutMs;
}

/**
 * @brief 执行屏幕开启操作
 */
void DisplayManager::performScreenOn() {
    if (m_screenOn && (!m_isFading || m_fadeDirection == FADE_TO_ON)) {
        return; // 已经开启且不在关闭渐变中
    }

    // 如果正在渐变关闭，立即停止并开始开启渐变
    if (m_isFading && m_fadeDirection == FADE_TO_OFF) {
        stopFading();
    }

    printf("[DisplayManager] 开启屏幕\n");

    // 如果启用了渐变功能，使用渐变开启
    if (m_fadingEnabled) {
        printf("[DisplayManager] 使用亮度渐变开启屏幕（目标亮度：%d%%）\n", m_brightness);
        startFading(m_brightness, FADE_TO_ON);
    } else {
        // 不使用渐变，直接开启
        performScreenOnImmediate();
    }
}

/**
 * @brief 执行屏幕关闭操作
 */
void DisplayManager::performScreenOff() {
    if (!m_screenOn) {
        return; // 已经关闭
    }

    // 如果已经在渐变关闭过程中，不要重复启动
    if (m_isFading && m_fadeDirection == FADE_TO_OFF) {
        return;
    }

    printf("[DisplayManager] 关闭屏幕（延时模式生效）\n");
    printf("[DisplayManager] 屏幕已进入省电模式，触摸屏幕可立即唤醒\n");

    // 如果启用了渐变功能，使用渐变关闭
    if (m_fadingEnabled) {
        printf("[DisplayManager] 使用亮度渐变关闭屏幕\n");
        startFading(0, FADE_TO_OFF);
    } else {
        // 不使用渐变，直接关闭
        performScreenOffImmediate();
    }
}

/**
 * @brief 重置延时计时器
 */
void DisplayManager::resetTimeoutTimer() {
    m_lastTouchTime = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // 如果屏幕关闭，立即开启
    if (!m_screenOn) {
        performScreenOn();
    }
}

/**
 * @brief 检查触摸唤醒功能是否可用
 */
bool DisplayManager::isTouchWakeupEnabled() const {
    // 触摸唤醒功能在延时模式下可用
    return (m_screenMode == SCREEN_MODE_TIMEOUT);
}

/**
 * @brief 获取触摸活动状态信息
 */
void DisplayManager::getTouchWakeupStatus(uint32_t& lastTouchTime, uint32_t& timeSinceLastTouch, bool& isInLowPower) const {
    lastTouchTime = m_lastTouchTime;
    timeSinceLastTouch = (xTaskGetTickCount() * portTICK_PERIOD_MS) - m_lastTouchTime;
    isInLowPower = false;
}

/**
 * @brief 检查三击手势超时
 */
void DisplayManager::checkTripleSwipeTimeout() {
    if (m_swipeCount == 0) {
        return;
    }

    TickType_t currentTime = xTaskGetTickCount();
    TickType_t timeSinceFirstSwipe = currentTime - m_firstSwipeTime;
    uint32_t timeSinceFirstSwipeMs = pdTICKS_TO_MS(timeSinceFirstSwipe);

    if (timeSinceFirstSwipeMs > TRIPLE_SWIPE_TIMEOUT_MS) {
        // 三击超时，重置状态
        printf("[DisplayManager] 三击手势超时（%lu毫秒），重置状态\n", timeSinceFirstSwipeMs);
        m_swipeCount = 0;
        m_firstSwipeTime = 0;
    }
}

/**
 * @brief 设置全局实例指针（供UI系统回调使用）
 */
void DisplayManager::setInstance(DisplayManager* instance) {
    s_instance = instance;
}

/**
 * @brief 获取全局实例指针
 */
DisplayManager* DisplayManager::getInstance() {
    return s_instance;
}

/**
 * @brief 更新当前页面状态（供UI系统回调使用）
 */
void DisplayManager::updateCurrentPage(DisplayPage page) {
    if (m_currentPage != page) {
        printf("[DisplayManager] 页面状态更新：%d -> %d\n", m_currentPage, page);
        m_currentPage = page;
    }
}

/**
 * @brief 根据屏幕对象更新当前页面状态
 */
void DisplayManager::updateCurrentPageByScreen(lv_obj_t* screen) {
    if (!screen) return;

    if (screen == ui_standbySCREEN) {
        updateCurrentPage(PAGE_HOME);
    } else if (screen == ui_overviewSCREEN) {
        updateCurrentPage(PAGE_OVERVIEW);
    } else if (screen == ui_dsMainSCREEN) {
        updateCurrentPage(PAGE_DS_MAIN);
    } else if (screen == ui_dsInfoSCREEN) {
        updateCurrentPage(PAGE_DS_INFO);
    } else if (screen == ui_kimiMainSCREEN) {
        updateCurrentPage(PAGE_KIMI_MAIN);
    } else if (screen == ui_kimiInfoSCREEN) {
        updateCurrentPage(PAGE_KIMI_INFO);
    }
}

// C风格包装函数，供UI系统调用
extern "C" void updateDisplayManagerCurrentPage(void* screen) {
    DisplayManager* instance = DisplayManager::getInstance();
    if (instance) {
        instance->updateCurrentPageByScreen((lv_obj_t*)screen);
    }
}

// UI系统调用WiFi信息页面的桥接函数（三击检测）
extern "C" void showWiFiInfoPageFromUI() {
    DisplayManager* instance = DisplayManager::getInstance();
    if (instance) {
        printf("[DisplayManager] UI待机页面右滑手势，当前页面：%d\n", instance->getCurrentPage());
        instance->handleStandbyRightSwipe();
    } else {
        printf("[DisplayManager] 错误：DisplayManager实例未找到\n");
    }
}

// === 亮度渐变功能实现 ===

/**
 * @brief 启用或禁用亮度渐变功能
 */
void DisplayManager::setFadingEnabled(bool enabled, uint32_t fadeDurationMs) {
    m_fadingEnabled = enabled;
    m_fadeDuration = fadeDurationMs;

    // 如果禁用渐变且当前正在渐变，停止渐变
    if (!enabled && m_isFading) {
        stopFading();
    }

    printf("[DisplayManager] 亮度渐变功能已%s，渐变时长: %d毫秒\n",
           enabled ? "启用" : "禁用", fadeDurationMs);
}

/**
 * @brief 获取亮度渐变功能状态
 */
bool DisplayManager::isFadingEnabled() const {
    return m_fadingEnabled;
}

/**
 * @brief 检查是否正在执行亮度渐变
 */
bool DisplayManager::isFading() const {
    return m_isFading;
}

/**
 * @brief 设置渐变持续时间
 */
void DisplayManager::setFadeDuration(uint32_t fadeDurationMs) {
    m_fadeDuration = fadeDurationMs;
    printf("[DisplayManager] 渐变持续时间已设置为: %d毫秒\n", fadeDurationMs);
}

/**
 * @brief 获取当前渐变亮度值
 */
uint8_t DisplayManager::getCurrentFadingBrightness() const {
    return m_currentFadingBrightness;
}

/**
 * @brief 启动亮度渐变
 */
void DisplayManager::startFading(uint8_t targetBrightness, FadeDirection direction) {
    // 如果渐变功能未启用，直接设置目标亮度
    if (!m_fadingEnabled) {
        if (direction == FADE_TO_ON) {
            performScreenOnImmediate();
        } else {
            performScreenOffImmediate();
        }
        return;
    }

    // 停止当前渐变（如果正在进行）
    if (m_isFading) {
        stopFading();
    }

    // 获取当前实际亮度作为起始亮度
    if (direction == FADE_TO_ON) {
        // 开启时从0开始渐变
        m_currentFadingBrightness = 0;
        setBrightnessImmediate(0);
        // 立即标记屏幕为开启状态，但亮度从0开始
        m_screenOn = true;
    } else {
        // 关闭时从当前亮度开始渐变
        m_currentFadingBrightness = m_brightness;
    }

    // 设置渐变参数
    m_targetFadingBrightness = targetBrightness;
    m_fadeDirection = direction;
    m_fadeStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    m_isFading = true;

    printf("[DisplayManager] 开始亮度渐变: %d%% -> %d%%, 方向: %s, 持续时间: %d毫秒\n",
           m_currentFadingBrightness,
           m_targetFadingBrightness,
           direction == FADE_TO_ON ? "亮起" : "变暗",
           m_fadeDuration);
}

/**
 * @brief 处理亮度渐变逻辑
 */
void DisplayManager::processFading() {
    if (!m_isFading || !m_fadingEnabled) {
        return;
    }

    uint32_t currentTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t elapsedTime = currentTime - m_fadeStartTime;

    // 检查渐变是否完成
    if (elapsedTime >= m_fadeDuration) {
        // 渐变完成，设置最终亮度
        m_currentFadingBrightness = m_targetFadingBrightness;
        setBrightnessImmediate(m_currentFadingBrightness);

        // 更新屏幕状态
        if (m_fadeDirection == FADE_TO_OFF) {
            m_screenOn = false;
            printf("[DisplayManager] 渐变关闭完成，屏幕背光已关闭，触摸系统保持活跃状态\n");
        } else {
            m_screenOn = true;
            printf("[DisplayManager] 渐变开启完成，屏幕亮度已恢复至 %d%%\n", m_currentFadingBrightness);
        }

        // 停止渐变
        m_isFading = false;
        return;
    }

    // 计算渐变进度 (0.0 到 1.0)
    float linearProgress = (float)elapsedTime / (float)m_fadeDuration;

    // 组合缓动：前半段使用正弦波，后半段使用三次贝塞尔，过渡更自然
    float smoothProgress;
    if (linearProgress < 0.5f) {
        // 前半段：使用修正的正弦波缓动，起步更平缓
        float t = linearProgress * 2.0f;
        smoothProgress = 0.5f * (1.0f - cos(t * M_PI * 0.5f));
    } else {
        // 后半段：使用修正的三次贝塞尔曲线，结束更平缓
        float t = (linearProgress - 0.5f) * 2.0f;
        float cubicPart = t * t * (3.0f - 2.0f * t);
        smoothProgress = 0.5f + 0.5f * cubicPart;
    }

    // 进一步平滑处理：使用五次多项式进行最终平滑
    smoothProgress = smoothProgress * smoothProgress * smoothProgress *
                    (smoothProgress * (smoothProgress * 6.0f - 15.0f) + 10.0f);

    uint8_t startBrightness, endBrightness;
    if (m_fadeDirection == FADE_TO_ON) {
        startBrightness = 0;
        endBrightness = m_targetFadingBrightness;
    } else {
        startBrightness = m_brightness; // 从设定亮度开始
        endBrightness = 0;
    }

    // 计算亮度范围和总步数
    int brightnessRange = abs(endBrightness - startBrightness);

    // 使用更精确的浮点计算，支持小数点后一位精度
    float currentBrightnessFloat = (float)startBrightness +
                                  ((float)(endBrightness - startBrightness) * smoothProgress);

    // 将亮度值精确到0.1%，然后四舍五入到整数
    float precisionBrightness = roundf(currentBrightnessFloat * 10.0f) / 10.0f;
    uint8_t newBrightness = (uint8_t)(precisionBrightness + 0.5f);

    // 确保亮度值在有效范围内
    if (newBrightness > 100) newBrightness = 100;

    bool shouldUpdate = false;

    // 如果亮度值发生任何变化就更新（提高平滑度）
    if (newBrightness != m_currentFadingBrightness) {
        shouldUpdate = true;
    }

    // 在渐变接近完成时强制更新，确保到达目标值
    if (elapsedTime >= m_fadeDuration - 50) {
        shouldUpdate = true;
    }

    // 更新硬件亮度
    if (shouldUpdate) {
        m_currentFadingBrightness = newBrightness;
        setBrightnessImmediate(m_currentFadingBrightness);
    }
}

/**
 * @brief 停止当前的亮度渐变
 */
void DisplayManager::stopFading() {
    if (!m_isFading) {
        return;
    }

    printf("[DisplayManager] 停止亮度渐变\n");
    m_isFading = false;

    // 设置为目标亮度（完成渐变）
    m_currentFadingBrightness = m_targetFadingBrightness;
    setBrightnessImmediate(m_currentFadingBrightness);

    // 更新屏幕状态
    if (m_fadeDirection == FADE_TO_OFF) {
        m_screenOn = false;
    } else {
        m_screenOn = true;
    }
}

/**
 * @brief 执行即时屏幕开启操作（不使用渐变）
 */
void DisplayManager::performScreenOnImmediate() {
    if (m_screenOn) {
        return; // 已经开启
    }

    printf("[DisplayManager] 即时开启屏幕（无渐变）\n");

    // 恢复屏幕亮度（使用安全的亮度设置方法）
    setBrightnessImmediate(m_brightness);

    // 更新当前渐变亮度值
    m_currentFadingBrightness = m_brightness;

    m_screenOn = true;
    printf("[DisplayManager] 屏幕已开启，亮度已恢复至 %d%%\n", m_brightness);
}

/**
 * @brief 执行即时屏幕关闭操作（不使用渐变）
 */
void DisplayManager::performScreenOffImmediate() {
    if (!m_screenOn) {
        return; // 已经关闭
    }

    printf("[DisplayManager] 即时关闭屏幕（无渐变）\n");

    // 设置屏幕亮度为0（关闭背光）（使用安全的亮度设置方法）
    setBrightnessImmediate(0);

    // 更新当前渐变亮度值
    m_currentFadingBrightness = 0;

    // LVGL任务继续运行，触摸检测继续工作
    m_screenOn = false;
    printf("[DisplayManager] 屏幕背光已关闭，触摸系统保持活跃状态\n");
}

/**
 * @brief 获取LVGL驱动实例
 */
LVGLDriver* DisplayManager::getLVGLDriver() const {
    return m_lvglDriver;
}

// === WiFi信息页面功能实现 ===

/**
 * @brief 显示WiFi信息页面（通过消息队列方式处理，避免死锁）
 */
void DisplayManager::showWiFiInfoPage() {
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_SHOW_WIFI_INFO;

    if (m_messageQueue) {
        if (xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            printf("[DisplayManager] WiFi信息页面显示消息已发送到队列\n");
        } else {
            printf("[DisplayManager] 错误：发送WiFi信息页面显示消息失败\n");
        }
    } else {
        printf("[DisplayManager] 错误：消息队列未初始化\n");
    }
}

/**
 * @brief 处理待机页面右滑手势（三击检测）
 */
void DisplayManager::handleStandbyRightSwipe() {
    TickType_t currentTime = xTaskGetTickCount();

    printf("[DisplayManager] 检测到待机页面右滑手势\n");

    if (m_swipeCount == 0) {
        // 第一次右滑
        m_swipeCount = 1;
        m_firstSwipeTime = currentTime;
        printf("[DisplayManager] 第1次右滑已记录，请在2秒内再滑动2次以打开WiFi信息页面（1/%d）\n", REQUIRED_SWIPE_COUNT);
        return;
    }

    // 检查是否在2秒内进行后续右滑
    TickType_t timeSinceFirstSwipe = currentTime - m_firstSwipeTime;
    uint32_t timeSinceFirstSwipeMs = pdTICKS_TO_MS(timeSinceFirstSwipe);

    if (timeSinceFirstSwipeMs <= TRIPLE_SWIPE_TIMEOUT_MS) {
        // 在时间窗口内，增加滑动计数
        m_swipeCount++;
        printf("[DisplayManager] 第%d次右滑已记录（%d/%d）\n", m_swipeCount, m_swipeCount, REQUIRED_SWIPE_COUNT);

        if (m_swipeCount >= REQUIRED_SWIPE_COUNT) {
            // 三击成功，执行WiFi页面切换
            printf("[DisplayManager] 三击手势成功，总间隔时间：%lu毫秒\n", timeSinceFirstSwipeMs);
            m_swipeCount = 0; // 重置状态
            m_firstSwipeTime = 0;
            requestShowWiFiInfoPage();
        } else {
            uint8_t remaining = REQUIRED_SWIPE_COUNT - m_swipeCount;
            printf("[DisplayManager] 还需要%d次滑动，剩余时间：%lu毫秒\n", remaining, TRIPLE_SWIPE_TIMEOUT_MS - timeSinceFirstSwipeMs);
        }
    } else {
        // 超时，重新开始计时
        printf("[DisplayManager] 三击超时（间隔%lu毫秒），重新开始计时\n", timeSinceFirstSwipeMs);
        m_swipeCount = 1;
        m_firstSwipeTime = currentTime;
    }
}

/**
 * @brief 请求显示WiFi信息页面（通过消息队列）
 */
void DisplayManager::requestShowWiFiInfoPage() {
    printf("[DisplayManager] 请求显示WiFi信息页面\n");

    // 检查5秒冷却时间
    TickType_t currentTime = xTaskGetTickCount();
    TickType_t timeSinceLastSwitch = currentTime - m_lastWiFiSwitchTime;
    const TickType_t cooldownTime = pdMS_TO_TICKS(5000); // 5秒冷却时间

    if (timeSinceLastSwitch < cooldownTime) {
        uint32_t remainingTime = pdTICKS_TO_MS(cooldownTime - timeSinceLastSwitch);
        printf("[DisplayManager] WiFi页面切换冷却中，剩余 %lu 毫秒\n", remainingTime);
        return;
    }

    // 如果已经在WiFi信息页面，不需要重复切换
    if (m_wifiInfoDisplayActive) {
        printf("[DisplayManager] WiFi信息页面已经激活，跳过切换\n");
        return;
    }

    // 通过消息队列发送显示WiFi信息页面的消息，避免死锁
    DisplayMessage msg;
    msg.type = DisplayMessage::MSG_SHOW_WIFI_INFO;

    if (m_messageQueue) {
        if (xQueueSend(m_messageQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            printf("[DisplayManager] WiFi信息页面显示消息已发送到队列\n");
            m_lastWiFiSwitchTime = currentTime;
        } else {
            printf("[DisplayManager] 错误：发送WiFi信息页面显示消息失败\n");
        }
    } else {
        printf("[DisplayManager] 错误：消息队列未初始化\n");
    }
}

/**
 * @brief 创建或重用WiFi信息显示页面
 */
void DisplayManager::createWiFiInfoPage() {
    // 检查是否可以重用现有对象
    if (m_wifiInfoScreen) {
        printf("[DisplayManager] 重用现有的WiFi信息页面对象\n");

        // 恢复对象的可见性
        lv_obj_clear_flag(m_wifiInfoScreen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(m_wifiInfoScreen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_pos(m_wifiInfoScreen, 0, 0);

        // 重新添加事件处理器
        lv_obj_add_event_cb(m_wifiInfoScreen, wifiInfoScreenEventHandler, LV_EVENT_ALL, NULL);

        // 显示页面
        lv_scr_load_anim(m_wifiInfoScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, false);

        return;
    }

    // 检查是否正在等待销毁
    if (m_wifiInfoPendingDestroy) {
        printf("[DisplayManager] 警告：WiFi信息页面正在等待销毁，取消创建\n");
        return;
    }

    // 创建WiFi信息屏幕
    m_wifiInfoScreen = lv_obj_create(NULL);
    if (!m_wifiInfoScreen) {
        printf("[DisplayManager] 错误：创建WiFi信息屏幕失败\n");
        return;
    }
    lv_obj_set_style_bg_color(m_wifiInfoScreen, lv_color_hex(0x000000), LV_PART_MAIN);  // 黑色背景
    lv_obj_set_style_pad_all(m_wifiInfoScreen, 20, LV_PART_MAIN);

    // 创建主容器，使用flex布局
    lv_obj_t* mainContainer = lv_obj_create(m_wifiInfoScreen);
    lv_obj_set_size(mainContainer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(mainContainer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mainContainer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mainContainer, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(mainContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mainContainer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(mainContainer);

    // 创建主标题
    lv_obj_t* titleLabel = lv_label_create(mainContainer);
    lv_label_set_text(titleLabel, "WiFi Information");
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(titleLabel, 40, LV_PART_MAIN);

    // 创建信息容器
    lv_obj_t* infoContainer = lv_obj_create(mainContainer);
    lv_obj_set_size(infoContainer, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(infoContainer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(infoContainer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(infoContainer, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(infoContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(infoContainer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 创建连接状态标签
    m_wifiStatusLabel = lv_label_create(infoContainer);
    lv_label_set_text(m_wifiStatusLabel, "WiFi Status: Checking...");
    lv_obj_set_style_text_font(m_wifiStatusLabel, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(m_wifiStatusLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(m_wifiStatusLabel, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(m_wifiStatusLabel, 20, LV_PART_MAIN);
    lv_obj_set_width(m_wifiStatusLabel, lv_pct(100));

    // 创建网络名称标签
    m_wifiSSIDLabel = lv_label_create(infoContainer);
    lv_label_set_text(m_wifiSSIDLabel, "WiFi Name: N/A");
    lv_obj_set_style_text_font(m_wifiSSIDLabel, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(m_wifiSSIDLabel, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_align(m_wifiSSIDLabel, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(m_wifiSSIDLabel, 15, LV_PART_MAIN);
    lv_obj_set_width(m_wifiSSIDLabel, lv_pct(100));
    lv_label_set_long_mode(m_wifiSSIDLabel, LV_LABEL_LONG_WRAP);

    // 创建IP地址标签
    m_wifiIPLabel = lv_label_create(infoContainer);
    lv_label_set_text(m_wifiIPLabel, "IP Address: N/A");
    lv_obj_set_style_text_font(m_wifiIPLabel, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(m_wifiIPLabel, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_align(m_wifiIPLabel, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(m_wifiIPLabel, lv_pct(100));
    lv_label_set_long_mode(m_wifiIPLabel, LV_LABEL_LONG_WRAP);

    // 为WiFi信息屏幕添加事件处理
    lv_obj_add_event_cb(m_wifiInfoScreen, wifiInfoScreenEventHandler, LV_EVENT_ALL, NULL);

    // 切换到WiFi信息屏幕
    lv_scr_load_anim(m_wifiInfoScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0, false);
}

/**
 * @brief 隐藏WiFi信息显示页面（重用策略，不删除对象）
 */
void DisplayManager::destroyWiFiInfoPage() {
    if (m_wifiInfoScreen) {
        // 检查是否是当前活动屏幕，如果是则不能操作
        lv_obj_t* active_screen = lv_scr_act();
        if (active_screen == m_wifiInfoScreen) {
            printf("[DisplayManager] 警告：WiFi信息页面仍是活动屏幕，跳过隐藏\n");
            return;
        }

        // 强制完成当前的渲染
        lv_refr_now(NULL);

        // 清理事件回调函数
        lv_obj_remove_event_cb(m_wifiInfoScreen, wifiInfoScreenEventHandler);

        // 隐藏对象而不删除，避免内存问题
        lv_obj_add_flag(m_wifiInfoScreen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(m_wifiInfoScreen, LV_OPA_TRANSP, LV_PART_MAIN);

        // 将对象移到屏幕外，确保不可见
        lv_obj_set_pos(m_wifiInfoScreen, -1000, -1000);

        // 强制渲染确保更新
        lv_refr_now(NULL);

        printf("[DisplayManager] WiFi信息页面已隐藏（对象保留以重用）\n");
    }
}

/**
 * @brief 更新WiFi信息显示
 */
void DisplayManager::updateWiFiInfoDisplay() {
    if (!m_wifiStatusLabel || !m_wifiSSIDLabel || !m_wifiIPLabel || !m_wifiManager) {
        return;
    }

    // 更新连接状态
    bool isConnected = m_wifiManager->isConnected();
    if (isConnected) {
        lv_label_set_text(m_wifiStatusLabel, "Status: Connected");
        lv_obj_set_style_text_color(m_wifiStatusLabel, lv_color_hex(0x00DD00), LV_PART_MAIN);  // 绿色

        // 更新网络名称
        String ssid = WiFi.SSID();
        char ssidText[128];
        snprintf(ssidText, sizeof(ssidText), "WiFi: %s", ssid.c_str());
        lv_label_set_text(m_wifiSSIDLabel, ssidText);

        // 更新IP地址
        String ip = m_wifiManager->getLocalIP();
        char ipText[64];
        snprintf(ipText, sizeof(ipText), "IP Address: %s", ip.c_str());
        lv_label_set_text(m_wifiIPLabel, ipText);
    } else {
        lv_label_set_text(m_wifiStatusLabel, "Connection Status: Disconnected");
        lv_obj_set_style_text_color(m_wifiStatusLabel, lv_color_hex(0xFF4444), LV_PART_MAIN);  // 红色

        lv_label_set_text(m_wifiSSIDLabel, "Network Name: N/A");
        lv_label_set_text(m_wifiIPLabel, "IP Address: N/A");
    }
}

/**
 * @brief WiFi信息页面事件处理函数
 */
void DisplayManager::wifiInfoScreenEventHandler(lv_event_t* e) {
    if (!e) {
        return;
    }

    lv_event_code_t event_code = lv_event_get_code(e);

    if (event_code == LV_EVENT_GESTURE) {
        lv_dir_t gesture_dir = lv_indev_get_gesture_dir(lv_indev_get_act());

        // 向左滑动或向下滑动返回到之前页面
        if (gesture_dir == LV_DIR_LEFT || gesture_dir == LV_DIR_BOTTOM) {
            lv_indev_wait_release(lv_indev_get_act());

            DisplayManager* instance = DisplayManager::getInstance();

            // 多重安全检查：实例存在、WiFi信息显示激活、对象存在、不在销毁状态
            if (instance &&
                instance->m_wifiInfoDisplayActive &&
                instance->m_wifiInfoScreen &&
                !instance->m_wifiInfoPendingDestroy) {

                // 检查5秒冷却时间
                TickType_t currentTime = xTaskGetTickCount();
                TickType_t timeSinceLastSwitch = currentTime - instance->m_lastWiFiSwitchTime;
                const TickType_t cooldownTime = pdMS_TO_TICKS(5000); // 5秒冷却时间

                if (timeSinceLastSwitch < cooldownTime) {
                    uint32_t remainingTime = pdTICKS_TO_MS(cooldownTime - timeSinceLastSwitch);
                    printf("[DisplayManager] WiFi页面返回冷却中，剩余 %lu 毫秒\n", remainingTime);
                    return;
                }

                printf("[DisplayManager] WiFi信息页面手势返回\n");

                // 通过消息队列处理返回操作，避免在事件回调中直接操作屏幕
                DisplayMessage msg;
                msg.type = DisplayMessage::MSG_RETURN_FROM_WIFI_INFO;

                if (instance->m_messageQueue) {
                    if (xQueueSend(instance->m_messageQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                        instance->m_lastWiFiSwitchTime = currentTime;
                    } else {
                        printf("[DisplayManager] 错误：发送WiFi信息页面返回消息失败\n");
                    }
                }
            }
        }
    }
}

/**
 * @brief 从WiFi信息页面返回到之前页面
 */
void DisplayManager::returnFromWiFiInfoPage() {
    if (!m_wifiInfoDisplayActive) {
        return;
    }

    printf("[DisplayManager] 开始从WiFi信息页面返回\n");

    m_wifiInfoDisplayActive = false;

    // 先切换到目标页面，再销毁WiFi信息页面
    switchPage(m_previousPageForWiFi);

    // 延迟一点再销毁，确保页面切换完成
    vTaskDelay(pdMS_TO_TICKS(100));

    // 销毁WiFi信息页面
    destroyWiFiInfoPage();
}
