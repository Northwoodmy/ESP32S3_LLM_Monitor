/*
 * DisplayManager - 显示管理器
 * ESP32S3_LLM_Monitor 项目
 *
 * 功能说明：
 * - 管理LVGL显示界面和页面切换(UI1主题,6个页面)
 * - 大模型用量数据显示(总览/DeepSeek/Kimi Code)
 * - 屏幕模式管理（定时开关、延时熄屏等）
 * - 触摸活动检测和超时控制
 * - 亮度渐变控制
 * - 天气和时间显示
 * - WiFi信息页面(三击手势)
 *
 * 设计特点：
 * - 模块化C++面向对象设计
 * - FreeRTOS任务管理
 * - 线程安全的显示更新(消息队列)
 * - SquareLine Studio生成的UI系统
 */

#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "LVGL_Driver.h"
#include "LLMUsageData.h"
#include "ConfigStorage.h"

// UI系统头文件
#include "ui.h"

// 前向声明
class WiFiManager;
class ConfigStorage;
class PSRAMManager;
class WeatherManager;

/**
 * @brief 显示页面枚举
 */
enum DisplayPage {
    PAGE_HOME = 0,      ///< 待机页面(时钟+天气)
    PAGE_OVERVIEW,      ///< 总览页面(今日消耗+4槽位)
    PAGE_DS_MAIN,       ///< DeepSeek 主数据页面
    PAGE_DS_INFO,       ///< DeepSeek 详情页面
    PAGE_KIMI_MAIN,     ///< Kimi Code 主数据页面
    PAGE_KIMI_INFO,     ///< Kimi Code 详情页面
    PAGE_WIFI_STATUS,   ///< WiFi状态页面
    PAGE_COUNT          ///< 页面总数
};

/**
 * @brief 显示消息结构
 */
struct DisplayMessage {
    enum MessageType {
        MSG_UPDATE_WIFI_STATUS,     ///< 更新WiFi状态
        MSG_UPDATE_SYSTEM_INFO,     ///< 更新系统信息
        MSG_UPDATE_USAGE_DATA,      ///< 更新用量数据
        MSG_UPDATE_WEATHER_DATA,    ///< 更新天气数据
        MSG_SWITCH_PAGE,            ///< 切换页面
        MSG_SET_BRIGHTNESS,         ///< 设置亮度
        MSG_SHOW_NOTIFICATION,      ///< 显示通知
        MSG_SCREEN_MODE_CHANGED,    ///< 屏幕模式改变
        MSG_TOUCH_ACTIVITY,         ///< 触摸活动
        MSG_SCREEN_ON,              ///< 屏幕开启
        MSG_SCREEN_OFF,             ///< 屏幕关闭
        MSG_SHOW_WIFI_INFO,         ///< 显示WiFi信息页面
        MSG_RETURN_FROM_WIFI_INFO,  ///< 从WiFi信息页面返回
        MSG_DESTROY_WIFI_INFO       ///< 销毁WiFi信息页面
    } type;

    union {
        struct {
            bool connected;
            char ssid[32];
            char ip[16];
            int rssi;
        } wifi_status;

        struct {
            uint32_t free_heap;
            uint32_t uptime;
            float cpu_usage;
        } system_info;

        struct {
            LLMUsageData usage_data;
        } usage_monitor;

        struct {
            char temperature[16];
            char weather[32];
            bool valid;
        } weather_data;

        struct {
            DisplayPage page;
        } page_switch;

        struct {
            uint8_t brightness;
        } brightness;

        struct {
            char text[64];
            uint32_t duration_ms;
        } notification;

        struct {
            ScreenMode mode;
            int startHour;
            int startMinute;
            int endHour;
            int endMinute;
            int timeoutMinutes;
            bool autoRotationEnabled;
            int staticRotation;
        } screen_mode;
    } data;
};

/**
 * @brief 显示管理器类
 *
 * 负责管理ESP32S3_LLM_Monitor系统的所有显示功能，包括：
 * - 多页面UI管理(待机/总览/DeepSeek/Kimi)
 * - 实时用量数据显示
 * - 亮度控制与渐变
 * - 屏幕模式管理（定时开关、延时熄屏等）
 * - 触摸交互处理
 * - WiFi信息页面
 */
class DisplayManager {
public:
    /**
     * @brief 亮度渐变方向枚举
     */
    enum FadeDirection {
        FADE_TO_ON = 0,                 ///< 渐变到开启状态（亮起）
        FADE_TO_OFF                     ///< 渐变到关闭状态（变暗）
    };

    DisplayManager();
    ~DisplayManager();

    /**
     * @brief 初始化显示管理器
     */
    bool init(LVGLDriver* lvgl_driver, WiFiManager* wifi_manager, ConfigStorage* config_storage,
              PSRAMManager* psram_manager = nullptr, WeatherManager* weather_manager = nullptr);

    /**
     * @brief 启动显示管理器任务
     */
    bool start();

    /**
     * @brief 停止显示管理器任务
     */
    void stop();

    /**
     * @brief 切换显示页面
     */
    void switchPage(DisplayPage page);

    /**
     * @brief 手动切换页面
     */
    void manualSwitchPage(DisplayPage page);

    /**
     * @brief 更新WiFi状态显示
     */
    void updateWiFiStatus(bool connected, const char* ssid, const char* ip, int rssi);

    /**
     * @brief 更新系统信息显示
     */
    void updateSystemInfo(uint32_t free_heap, uint32_t uptime, float cpu_usage);

    /**
     * @brief 设置显示亮度
     */
    void setBrightness(uint8_t brightness);

    /**
     * @brief 获取当前亮度
     */
    uint8_t getBrightness() const;

    /**
     * @brief 显示通知消息
     */
    void showNotification(const char* text, uint32_t duration_ms = 3000);

    /**
     * @brief 更新大模型用量数据
     */
    void updateUsageData(const LLMUsageData& usage_data);

    /**
     * @brief 更新天气数据显示
     */
    void updateWeatherData(const char* temperature, const char* weather);

    /**
     * @brief 获取当前用量数据
     */
    const LLMUsageData& getCurrentUsageData() const;

    /**
     * @brief 获取当前页面
     */
    DisplayPage getCurrentPage() const;

    /**
     * @brief 检查是否已初始化
     */
    bool isInitialized() const;

    /**
     * @brief 检查任务是否在运行
     */
    bool isRunning() const;

    /**
     * @brief 更新时间显示
     */
    void updateTimeDisplay();

    /**
     * @brief 更新用量数据显示
     */
    void updateUsageDataDisplay();

    /**
     * @brief 更新天气显示
     */
    void updateWeatherDisplay();

    // === 屏幕模式管理功能 ===

    bool loadScreenModeConfig();
    void setScreenMode(ScreenMode mode, int startHour = 8, int startMinute = 0,
                       int endHour = 22, int endMinute = 0, int timeoutMinutes = 10, bool autoRotationEnabled = true);
    ScreenMode getScreenMode() const;
    bool shouldScreenBeOn() const;

    // === 屏幕开关控制 ===

    void forceScreenOn();
    void forceScreenOff();
    void notifyTouchActivity();
    bool isScreenOn() const;
    bool isTouchWakeupEnabled() const;
    void getTouchWakeupStatus(uint32_t& lastTouchTime, uint32_t& timeSinceLastTouch, bool& isInLowPower) const;

    // === 亮度渐变控制功能 ===

    void setFadingEnabled(bool enabled, uint32_t fadeDurationMs = 1000);
    bool isFadingEnabled() const;
    bool isFading() const;
    void setFadeDuration(uint32_t fadeDurationMs);
    uint8_t getCurrentFadingBrightness() const;

    // === WiFi信息页面功能 ===

    void showWiFiInfoPage();
    void requestShowWiFiInfoPage();
    void handleStandbyRightSwipe();
    void updateWiFiInfoDisplay();

    // === 全局实例(供UI系统回调) ===

    static void setInstance(DisplayManager* instance);
    static DisplayManager* getInstance();
    void updateCurrentPage(DisplayPage page);
    void updateCurrentPageByScreen(lv_obj_t* screen);
    LVGLDriver* getLVGLDriver() const;

private:
    /**
     * @brief 显示管理器任务静态入口
     */
    static void displayTaskEntry(void* arg);

    /**
     * @brief 显示管理器任务执行函数
     */
    void displayTask();

    /**
     * @brief 处理显示消息
     */
    void processMessage(const DisplayMessage& msg);

    /**
     * @brief 处理亮度调整消息（无锁版本,避免死锁）
     */
    void processBrightnessMessage(const DisplayMessage& msg);

    /**
     * @brief 立即设置亮度（不通过消息队列）
     */
    void setBrightnessImmediate(uint8_t brightness);

    /**
     * @brief 更新总览页槽位进度条
     */
    void updateUsageBars();

    /**
     * @brief 更新DeepSeek页面显示
     */
    void updateDeepSeekPages();

    /**
     * @brief 更新Kimi Code页面显示
     */
    void updateKimiPages();

    // === 屏幕模式管理私有方法 ===

    void processScreenModeLogic();
    bool isInScheduledTime() const;
    bool isTimeoutExpired() const;
    void performScreenOn();
    void performScreenOff();
    void resetTimeoutTimer();

    // === 三击手势检测 ===

    void checkTripleSwipeTimeout();

    // === WiFi信息页面私有方法 ===

    void createWiFiInfoPage();
    void destroyWiFiInfoPage();
    static void wifiInfoScreenEventHandler(lv_event_t* e);
    void returnFromWiFiInfoPage();

    // === 亮度渐变私有方法 ===

    void startFading(uint8_t targetBrightness, FadeDirection direction);
    void processFading();
    void stopFading();
    void performScreenOnImmediate();
    void performScreenOffImmediate();

private:
    // 成员变量
    bool m_initialized;                 ///< 初始化状态
    bool m_running;                     ///< 运行状态
    TaskHandle_t m_taskHandle;          ///< 任务句柄
    QueueHandle_t m_messageQueue;       ///< 消息队列

    // 静态全局实例指针，用于UI系统回调
    static DisplayManager* s_instance;

    // 外部依赖
    LVGLDriver* m_lvglDriver;           ///< LVGL驱动指针
    WiFiManager* m_wifiManager;         ///< WiFi管理器指针
    ConfigStorage* m_configStorage;     ///< 配置存储指针
    PSRAMManager* m_psramManager;       ///< PSRAM管理器指针
    WeatherManager* m_weatherManager;   ///< 天气管理器指针

    // 显示状态
    DisplayPage m_currentPage;          ///< 当前页面
    uint8_t m_brightness;               ///< 当前亮度

    // LVGL对象
    lv_obj_t* m_screen;                 ///< 主屏幕对象

    // 用量监控数据
    LLMUsageData m_usageData;           ///< 大模型用量数据

    // === 屏幕模式管理成员变量 ===
    ScreenMode m_screenMode;            ///< 当前屏幕模式
    int m_screenStartHour;              ///< 定时开始小时
    int m_screenStartMinute;            ///< 定时开始分钟
    int m_screenEndHour;                ///< 定时结束小时
    int m_screenEndMinute;              ///< 定时结束分钟
    int m_screenTimeoutMinutes;         ///< 延时分钟数

    bool m_screenOn;                    ///< 屏幕开启状态
    uint32_t m_lastTouchTime;           ///< 最后触摸时间
    uint32_t m_lastScreenModeCheck;     ///< 最后屏幕模式检查时间

    // === WiFi信息页面成员变量 ===
    lv_obj_t* m_wifiInfoScreen;         ///< WiFi信息屏幕对象
    lv_obj_t* m_wifiStatusLabel;        ///< WiFi连接状态标签对象
    lv_obj_t* m_wifiSSIDLabel;          ///< WiFi网络名称标签对象
    lv_obj_t* m_wifiIPLabel;            ///< WiFi IP地址标签对象
    DisplayPage m_previousPageForWiFi;  ///< WiFi信息页面开始前的页面，用于恢复
    bool m_wifiInfoDisplayActive;       ///< WiFi信息显示是否激活
    bool m_wifiInfoPendingDestroy;      ///< WiFi信息页面是否等待销毁
    TickType_t m_lastWiFiSwitchTime;    ///< 上次WiFi页面切换时间，用于5秒冷却

    // === 三击手势检测成员变量 ===
    TickType_t m_firstSwipeTime;        ///< 第一次右滑时间
    uint8_t m_swipeCount;               ///< 当前滑动次数计数
    static const uint32_t TRIPLE_SWIPE_TIMEOUT_MS = 2000; ///< 三击超时时间（2秒）
    static const uint8_t REQUIRED_SWIPE_COUNT = 3;         ///< 需要的滑动次数

    // === 亮度渐变成员变量 ===
    bool m_fadingEnabled;               ///< 是否启用亮度渐变
    uint8_t m_currentFadingBrightness;  ///< 当前渐变亮度值
    uint8_t m_targetFadingBrightness;   ///< 目标渐变亮度值
    uint32_t m_fadeStartTime;           ///< 渐变开始时间
    uint32_t m_fadeDuration;            ///< 渐变持续时间（毫秒）
    bool m_isFading;                    ///< 是否正在渐变中
    FadeDirection m_fadeDirection;      ///< 渐变方向

    // 任务配置
    static const uint32_t TASK_STACK_SIZE = 9 * 1024;    ///< 任务栈大小
    static const UBaseType_t TASK_PRIORITY = 3;          ///< 任务优先级
    static const BaseType_t TASK_CORE = 0;               ///< 任务运行核心
    static const uint32_t MESSAGE_QUEUE_SIZE = 10;       ///< 消息队列大小

    // 屏幕模式相关常量
    static const uint32_t SCREEN_MODE_CHECK_INTERVAL = 1000;  ///< 屏幕模式检查间隔（毫秒）
};

// C风格包装函数声明（供UI系统调用）
extern "C" void updateDisplayManagerCurrentPage(void* screen);

// UI系统调用WiFi信息页面的桥接函数声明
extern "C" void showWiFiInfoPageFromUI();

#endif // DISPLAY_MANAGER_H
