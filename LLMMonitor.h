/*
 * LLMMonitor.h - 大模型用量监控器头文件
 * ESP32S3_LLM_Monitor 项目
 *
 * 功能:
 * - 定时轮询 DeepSeek / Kimi Code 官方 API(HTTPS 直连)
 * - DeepSeek: 余额查询 + 差分消耗追踪(无官方用量API,通过余额差值累计消耗)
 * - Kimi Code: 5小时/7天配额查询
 * - 消耗状态 NVS 持久化(断电不丢失,跨零点自动清零)
 * - 数据通过回调传递给 DisplayManager
 */

#ifndef LLM_MONITOR_H
#define LLM_MONITOR_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "LLMUsageData.h"

// 前向声明
class PSRAMManager;
class ConfigStorage;

/**
 * @brief 单个平台槽位的配置
 */
struct ProviderSlot {
    ProviderType type;      ///< 平台类型(PROVIDER_NONE/DEEPSEEK/KIMICODE)
    String apiKey;          ///< API Key(DeepSeek: sk-xxx; Kimi Code: sk-kimi-xxx)
    double unitPrice;       ///< 估算单价 ¥/M tokens(仅 DeepSeek 用于换算 tokens)

    ProviderSlot() : type(PROVIDER_NONE), apiKey(""), unitPrice(2.0) {}
};

class LLMMonitor {
public:
    LLMMonitor();
    ~LLMMonitor();

    // 初始化监控器
    void init();
    void init(PSRAMManager* psramManager);
    void init(PSRAMManager* psramManager, ConfigStorage* configStorage);

    // 停止监控器
    void stop();

    // 设置数据回调
    void setUsageCallback(LLMUsageCallback callback, void* userData);

    // 获取当前用量数据
    const LLMUsageData& getCurrentUsageData() const;

    // 重新加载配置(API Key 等修改后调用)
    void reloadConfig();

    // 测试指定平台连接(一次性请求, 不影响消耗追踪状态, 供 Web 测试接口使用)
    bool testProvider(ProviderType type, const String& apiKey, double unitPrice, String& message);

    // 获取当前轮询间隔(秒)
    uint32_t getPollIntervalSec() const { return m_pollIntervalSec; }

private:
    // 任务句柄
    TaskHandle_t m_taskHandle;

    // PSRAM管理器指针
    PSRAMManager* m_psramManager;

    // 配置存储管理器指针
    ConfigStorage* m_configStorage;

    // 任务运行标志
    bool m_isRunning;

    // 配置变更标志(触发重新加载)
    volatile bool m_configDirty;

    // 轮询间隔(ms)
    uint32_t m_requestInterval;

    // 轮询间隔(秒,配置值)
    uint32_t m_pollIntervalSec;

    // 槽位配置
    ProviderSlot m_slots[4];

    // 数据回调
    LLMUsageCallback m_usageCallback;
    void* m_callbackUserData;

    // 当前用量数据
    LLMUsageData m_currentData;

    // === DeepSeek 差分消耗追踪状态 ===
    double m_dsLastBalance;     ///< 上次查询到的余额
    bool m_dsBalanceValid;      ///< 余额基线是否已建立
    double m_dsCostToday;       ///< 今日累计消耗
    double m_dsCostTotal;       ///< 历史累计消耗
    char m_dsCostDate[12];      ///< 今日消耗归属日期 "YYYY-MM-DD"

    // 静态任务函数
    static void monitorTask(void* parameter);

    // 私有方法
    bool isWiFiConnected();
    void loadConfig();
    void fetchAllProviders();

    // 平台数据获取(testOnly=true 时不做差分追踪和状态持久化)
    bool fetchDeepSeek(ProviderSlot& slot, ProviderData& out, bool testOnly = false);
    bool fetchKimiCode(ProviderSlot& slot, ProviderData& out);

    // DeepSeek 差分消耗追踪
    void trackDeepSeekCost(double balance, ProviderData& out);
    void persistDSState();
    void checkDayRollover();

    // 时间辅助
    void currentDateString(char* buf, size_t len);
    void currentTimeString(char* buf, size_t len);
};

#endif // LLM_MONITOR_H
