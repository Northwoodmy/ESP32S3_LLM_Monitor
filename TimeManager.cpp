/*
 * TimeManager.cpp - 时间管理器类实现文件
 * ESP32S3_LLM_Monitor项目 - NTP网络时间同步模块
 */

#include "TimeManager.h"
#include "PSRAMManager.h"
#include "WiFiManager.h"
#include "ConfigStorage.h"
#include <ArduinoJson.h>

TimeManager::TimeManager() :
    m_psramManager(nullptr),
    m_wifiManager(nullptr),
    m_configStorage(nullptr),
    m_taskHandle(nullptr),
    m_mutex(nullptr),
    m_initialized(false),
    m_running(false),
    m_debugMode(false),
    m_syncStatus(TIME_NOT_SYNCED),
    m_lastSyncTime(0),
    m_syncFailCount(0),
    m_updateCallback(nullptr),
    m_timeValid(false),
    m_timezoneOffset(8.0f) {  // 默认中国时区 UTC+8
    
    memset(&m_timeinfo, 0, sizeof(m_timeinfo));
    printf("[TimeManager] 时间管理器已创建\n");
}

TimeManager::~TimeManager() {
    stop();
    
    // 销毁互斥锁
    if (m_mutex) {
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }
    
    printf("[TimeManager] 时间管理器已销毁\n");
}

bool TimeManager::init(PSRAMManager* psramManager, WiFiManager* wifiManager, ConfigStorage* configStorage) {
    if (m_initialized) {
        printf("[TimeManager] 时间管理器已经初始化\n");
        return true;
    }
    
    printf("[TimeManager] 正在初始化时间管理器...\n");
    
    m_psramManager = psramManager;
    m_wifiManager = wifiManager;
    m_configStorage = configStorage;
    
    // 创建互斥锁
    m_mutex = xSemaphoreCreateMutex();
    if (!m_mutex) {
        printf("[TimeManager] 互斥锁创建失败\n");
        return false;
    }
    
    // 加载配置
    if (m_configStorage) {
        loadConfig();
    }
    
    // 配置NTP
    if (!configureNTP()) {
        printf("[TimeManager] NTP配置失败\n");
        return false;
    }
    
    m_initialized = true;
    printf("[TimeManager] 时间管理器初始化完成\n");
    printf("[TimeManager] NTP服务器: %s\n", m_ntpConfig.primaryServer.c_str());
    printf("[TimeManager] 时区: %s (UTC%+.1f)\n", m_ntpConfig.timezone.c_str(), m_timezoneOffset);
    printf("[TimeManager] 同步间隔: %d分钟\n", m_ntpConfig.syncInterval);
    
    return true;
}

bool TimeManager::start() {
    if (!m_initialized) {
        printf("[TimeManager] 请先初始化时间管理器\n");
        return false;
    }
    
    if (m_running) {
        printf("[TimeManager] 时间同步任务已在运行\n");
        return true;
    }
    
    printf("[TimeManager] 启动时间同步任务...\n");
    
    m_running = true;
    
    if (m_psramManager && m_psramManager->isPSRAMAvailable()) {
        // 使用PSRAM栈创建任务
        printf("[TimeManager] 使用PSRAM栈创建时间同步任务\n");
        m_taskHandle = m_psramManager->createTaskWithPSRAMStack(
            timeTaskEntry,              // 任务函数
            "TimeManager",              // 任务名称
            TASK_STACK_SIZE,           // 栈大小
            this,                      // 任务参数
            TASK_PRIORITY,             // 任务优先级
            TASK_CORE                  // 运行核心
        );
        
        if (m_taskHandle != nullptr) {
            printf("[TimeManager] ✓ 时间同步任务(PSRAM栈)创建成功\n");
        } else {
            m_running = false;
            printf("[TimeManager] ✗ 时间同步任务(PSRAM栈)创建失败\n");
            return false;
        }
    } else {
        // 回退到SRAM栈创建任务
        printf("[TimeManager] 使用SRAM栈创建时间同步任务\n");
        BaseType_t result = xTaskCreatePinnedToCore(
            timeTaskEntry,              // 任务函数
            "TimeManager",              // 任务名称
            TASK_STACK_SIZE,           // 栈大小
            this,                      // 任务参数
            TASK_PRIORITY,             // 任务优先级
            &m_taskHandle,             // 任务句柄
            TASK_CORE                  // 运行核心
        );
        
        if (result == pdPASS) {
            printf("[TimeManager] ✓ 时间同步任务(SRAM栈)创建成功\n");
        } else {
            m_running = false;
            printf("[TimeManager] ✗ 时间同步任务(SRAM栈)创建失败\n");
            return false;
        }
    }
    
    return true;
}

void TimeManager::stop() {
    if (!m_running) {
        return;
    }
    
    printf("[TimeManager] 停止时间同步任务...\n");
    
    m_running = false;
    
    if (m_taskHandle) {
        vTaskDelete(m_taskHandle);
        m_taskHandle = nullptr;
    }
    
    printf("[TimeManager] 时间同步任务已停止\n");
}

void TimeManager::timeTaskEntry(void* parameter) {
    TimeManager* manager = static_cast<TimeManager*>(parameter);
    manager->timeTask();
}

void TimeManager::timeTask() {
    printf("[TimeManager] 时间同步任务开始运行\n");
    
    unsigned long lastSyncAttempt = 0;
    unsigned long syncIntervalMs = m_ntpConfig.syncInterval * 60 * 1000;  // 转换为毫秒
    
    while (m_running) {
        unsigned long currentTime = millis();
        
        // 检查是否需要同步时间
        if (isWiFiConnected() && 
            (currentTime - lastSyncAttempt >= syncIntervalMs || lastSyncAttempt == 0)) {
            
            if (lastSyncAttempt == 0) {
                printf("[TimeManager] 启动后首次同步时间...\n");
            } else if (m_debugMode) {
                printf("[TimeManager] 定时同步时间...\n");
            }
            
            if (performNTPSync()) {
                lastSyncAttempt = currentTime;
                m_syncFailCount = 0;
            } else {
                m_syncFailCount++;
                printf("[TimeManager] 时间同步失败，失败次数: %d\n", m_syncFailCount);
                
                // 如果连续失败多次，延长重试间隔
                if (m_syncFailCount >= MAX_SYNC_RETRY) {
                    lastSyncAttempt = currentTime;  // 重置，等下一个周期
                }
            }
        }
        
        // 更新本地时间显示
        updateLocalTime();
        
        // 每秒更新一次
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    printf("[TimeManager] 时间同步任务结束\n");
    vTaskDelete(nullptr);
}

bool TimeManager::syncTimeNow() {
    if (!isWiFiConnected()) {
        printf("[TimeManager] WiFi未连接，无法同步时间\n");
        return false;
    }
    
    printf("[TimeManager] 开始手动同步时间...\n");
    return performNTPSync();
}

bool TimeManager::performNTPSync() {
    if (!takeMutex()) {
        return false;
    }
    
    m_syncStatus = TIME_SYNCING;
    giveMutex();
    
    // 记录同步开始时间
    unsigned long syncStartTime = millis();
    
    printf("\n🔄 ==========================================\n");
    printf("[TimeManager] 🚀 开始NTP时间同步...\n");
    printf("🌐 主NTP服务器: %s\n", m_ntpConfig.primaryServer.c_str());
    printf("🌐 备用NTP服务器: %s\n", m_ntpConfig.secondaryServer.c_str());
    printf("🌍 目标时区: %s (UTC%+.1f)\n", m_ntpConfig.timezone.c_str(), m_timezoneOffset);
    printf("📡 WiFi状态: %s\n", isWiFiConnected() ? "已连接" : "未连接");
    printf("==========================================🔄\n");
    
    bool success = false;
    
    // 尝试主服务器
    if (configureNTP()) {
        // 配置时区
        configTime(m_timezoneOffset * 3600, 0, 
                   m_ntpConfig.primaryServer.c_str(), 
                   m_ntpConfig.secondaryServer.c_str());
        
        // 等待时间同步
        int attempts = 0;
        const int maxAttempts = 10;
        
        while (attempts < maxAttempts && !success) {
            if (getLocalTime(&m_timeinfo)) {
                success = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            attempts++;
        }
    }
    
    if (takeMutex()) {
        if (success) {
            m_syncStatus = TIME_SYNCED;
            m_lastSyncTime = millis();
            m_timeValid = true;
            
            // 总是输出时间同步成功的详细信息
            printf("\n🕐 ==========================================\n");
            printf("[TimeManager] ✅ NTP时间同步成功！\n");
            printf("🌐 NTP服务器: %s\n", m_ntpConfig.primaryServer.c_str());
            printf("🌍 时区设置: %s (UTC%+.1f)\n", m_ntpConfig.timezone.c_str(), m_timezoneOffset);
            printf("📅 同步时间: %04d年%02d月%02d日 %02d:%02d:%02d\n",
                   m_timeinfo.tm_year + 1900, m_timeinfo.tm_mon + 1, m_timeinfo.tm_mday,
                   m_timeinfo.tm_hour, m_timeinfo.tm_min, m_timeinfo.tm_sec);
            printf("📊 Unix时间戳: %lu\n", (unsigned long)mktime(&m_timeinfo));
            printf("⏰ 下次同步: %d分钟后\n", m_ntpConfig.syncInterval);
            printf("🔄 同步耗时: %lu毫秒\n", millis() - syncStartTime);
            
            // 显示星期信息
            const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
            if (m_timeinfo.tm_wday >= 0 && m_timeinfo.tm_wday <= 6) {
                printf("📆 星期: %s\n", weekdays[m_timeinfo.tm_wday]);
            }
            
            printf("==========================================🕐\n\n");
            
            // 通知时间更新
            notifyTimeUpdate();
        } else {
            m_syncStatus = TIME_SYNC_FAILED;
            m_syncFailCount++;
            
            printf("\n❌ ==========================================\n");
            printf("[TimeManager] ❌ NTP时间同步失败！\n");
            printf("🌐 尝试的NTP服务器: %s\n", m_ntpConfig.primaryServer.c_str());
            printf("🌐 备用NTP服务器: %s\n", m_ntpConfig.secondaryServer.c_str());
            printf("📡 WiFi连接状态: %s\n", isWiFiConnected() ? "已连接" : "未连接");
            printf("🔄 失败次数: %d\n", m_syncFailCount);
            printf("⏱️ 同步耗时: %lu毫秒\n", millis() - syncStartTime);
            printf("⏰ 下次重试: %d分钟后\n", m_ntpConfig.syncInterval);
            printf("==========================================❌\n\n");
        }
        
        giveMutex();
    }
    
    return success;
}

bool TimeManager::configureNTP() {
    // 验证时区格式
    if (!isValidTimeZone(m_ntpConfig.timezone)) {
        printf("[TimeManager] 无效的时区格式: %s\n", m_ntpConfig.timezone.c_str());
        return false;
    }
    
    return true;
}

void TimeManager::updateLocalTime() {
    if (m_timeValid && takeMutex()) {
        getLocalTime(&m_timeinfo);
        giveMutex();
    }
}

void TimeManager::notifyTimeUpdate() {
    if (m_updateCallback) {
        TimeInfo timeInfo = getCurrentTime();
        m_updateCallback(timeInfo);
    }
}

TimeInfo TimeManager::getCurrentTime() {
    TimeInfo timeInfo;
    
    if (takeMutex()) {
        if (m_timeValid && getLocalTime(&m_timeinfo)) {
            timeInfo.year = m_timeinfo.tm_year + 1900;
            timeInfo.month = m_timeinfo.tm_mon + 1;
            timeInfo.day = m_timeinfo.tm_mday;
            timeInfo.hour = m_timeinfo.tm_hour;
            timeInfo.minute = m_timeinfo.tm_min;
            timeInfo.second = m_timeinfo.tm_sec;
            timeInfo.weekday = m_timeinfo.tm_wday;
            timeInfo.timestamp = mktime(&m_timeinfo);
            timeInfo.valid = true;
        }
        giveMutex();
    }
    
    return timeInfo;
}

String TimeManager::getFormattedTime(const String& format) const {
    if (!m_timeValid || !takeMutex()) {
        return "Invalid Time";
    }
    
    String result = formatTime(&m_timeinfo, format);
    giveMutex();
    return result;
}

String TimeManager::getDateString() const {
    return getFormattedTime("%Y-%m-%d");
}

String TimeManager::getTimeString() const {
    return getFormattedTime("%H:%M:%S");
}

String TimeManager::getDateTimeString() const {
    return getFormattedTime("%Y-%m-%d %H:%M:%S");
}

unsigned long TimeManager::getTimestamp() {
    if (!m_timeValid) {
        return 0;
    }
    
    if (takeMutex()) {
        time_t now;
        time(&now);
        giveMutex();
        return (unsigned long)now;
    }
    
    return 0;
}

void TimeManager::setNTPConfig(const NTPServerConfig& config) {
    if (takeMutex()) {
        m_ntpConfig = config;
        
        // 解析时区偏移
        if (config.timezone.startsWith("CST")) {
            m_timezoneOffset = 8.0f;  // 中国标准时间
        } else if (config.timezone.startsWith("UTC")) {
            String offsetStr = config.timezone.substring(3);
            m_timezoneOffset = offsetStr.toFloat();
        }
        
        if (m_configStorage) {
            saveConfig();
        }
        
        printf("[TimeManager] NTP配置已更新\n");
        giveMutex();
    }
}

NTPServerConfig TimeManager::getNTPConfig() const {
    return m_ntpConfig;
}

void TimeManager::setTimezone(const String& timezone) {
    if (takeMutex()) {
        m_ntpConfig.timezone = timezone;
        
        if (m_configStorage) {
            saveConfig();
        }
        
        giveMutex();
    }
}

void TimeManager::setTimezone(float utcOffset) {
    if (takeMutex()) {
        m_timezoneOffset = utcOffset;
        m_ntpConfig.timezone = "UTC" + String(utcOffset, 1);
        
        if (m_configStorage) {
            saveConfig();
        }
        
        giveMutex();
    }
}

void TimeManager::setSyncInterval(int minutes) {
    if (minutes >= 1 && takeMutex()) {
        m_ntpConfig.syncInterval = minutes;
        
        if (m_configStorage) {
            saveConfig();
        }
        
        printf("[TimeManager] 同步间隔设置为 %d 分钟\n", minutes);
        giveMutex();
    }
}

TimeSyncStatus TimeManager::getSyncStatus() const {
    return m_syncStatus;
}

unsigned long TimeManager::getLastSyncTime() const {
    return m_lastSyncTime;
}

int TimeManager::getSyncFailCount() const {
    return m_syncFailCount;
}

float TimeManager::getTimezoneOffset() const {
    return m_timezoneOffset;
}

bool TimeManager::isTimeValid() const {
    return m_timeValid;
}

bool TimeManager::isWiFiConnected() const {
    return m_wifiManager ? m_wifiManager->isConnected() : WiFi.status() == WL_CONNECTED;
}

String TimeManager::getStatusString() const {
    String status = "时间管理器状态:\n";
    status += "- 初始化: " + String(m_initialized ? "是" : "否") + "\n";
    status += "- 运行中: " + String(m_running ? "是" : "否") + "\n";
    status += "- WiFi连接: " + String(isWiFiConnected() ? "是" : "否") + "\n";
    status += "- 时间有效: " + String(m_timeValid ? "是" : "否") + "\n";
    
    switch (m_syncStatus) {
        case TIME_NOT_SYNCED: status += "- 同步状态: 未同步\n"; break;
        case TIME_SYNCING: status += "- 同步状态: 同步中\n"; break;
        case TIME_SYNCED: status += "- 同步状态: 已同步\n"; break;
        case TIME_SYNC_FAILED: status += "- 同步状态: 同步失败\n"; break;
    }
    
    if (m_timeValid) {
        status += "- 当前时间: " + getDateTimeString() + "\n";
    }
    
    status += "- 时区: UTC" + String(m_timezoneOffset, 1) + "\n";
    status += "- 同步间隔: " + String(m_ntpConfig.syncInterval) + "分钟\n";
    status += "- 失败次数: " + String(m_syncFailCount) + "\n";
    
    return status;
}

String TimeManager::getStatusJSON() const {
    DynamicJsonDocument doc(1024);
    
    doc["initialized"] = m_initialized;
    doc["running"] = m_running;
    doc["wifiConnected"] = isWiFiConnected();
    doc["timeValid"] = m_timeValid;
    doc["syncStatus"] = (int)m_syncStatus;
    doc["lastSyncTime"] = m_lastSyncTime;
    doc["syncFailCount"] = m_syncFailCount;
    doc["timezoneOffset"] = m_timezoneOffset;
    doc["syncInterval"] = m_ntpConfig.syncInterval;
    
    if (m_timeValid) {
        TimeInfo currentTime = const_cast<TimeManager*>(this)->getCurrentTime();
        doc["currentTime"] = getDateTimeString();
        doc["timestamp"] = currentTime.timestamp;
    }
    
    JsonObject ntpConfig = doc.createNestedObject("ntpConfig");
    ntpConfig["primaryServer"] = m_ntpConfig.primaryServer;
    ntpConfig["secondaryServer"] = m_ntpConfig.secondaryServer;
    ntpConfig["timezone"] = m_ntpConfig.timezone;
    
    String result;
    serializeJson(doc, result);
    return result;
}

void TimeManager::printTimeInfo() {
    printf("\n=== 时间信息 ===\n");
    printf("时间有效: %s\n", m_timeValid ? "是" : "否");
    
    if (m_timeValid) {
        TimeInfo timeInfo = getCurrentTime();
        printf("当前时间: %04d-%02d-%02d %02d:%02d:%02d\n",
               timeInfo.year, timeInfo.month, timeInfo.day,
               timeInfo.hour, timeInfo.minute, timeInfo.second);
        printf("星期: %d (0=周日)\n", timeInfo.weekday);
        printf("时间戳: %lu\n", timeInfo.timestamp);
    }
    
    printf("时区: UTC%+.1f\n", m_timezoneOffset);
    printf("同步状态: ");
    switch (m_syncStatus) {
        case TIME_NOT_SYNCED: printf("未同步\n"); break;
        case TIME_SYNCING: printf("同步中\n"); break;
        case TIME_SYNCED: printf("已同步\n"); break;
        case TIME_SYNC_FAILED: printf("同步失败\n"); break;
    }
    
    if (m_lastSyncTime > 0) {
        printf("最后同步: %lu毫秒前\n", millis() - m_lastSyncTime);
    }
    
    printf("NTP服务器: %s\n", m_ntpConfig.primaryServer.c_str());
    printf("同步间隔: %d分钟\n", m_ntpConfig.syncInterval);
    printf("===============\n\n");
}

void TimeManager::setDebugMode(bool enabled) {
    m_debugMode = enabled;
    printf("[TimeManager] 调试模式: %s\n", enabled ? "开启" : "关闭");
}

void TimeManager::setTimeUpdateCallback(TimeUpdateCallback callback) {
    m_updateCallback = callback;
}

void TimeManager::loadConfig() {
    if (!m_configStorage) {
        printf("[TimeManager] 配置存储未初始化，使用默认配置\n");
        return;
    }
    
    // 从配置存储异步加载NTP设置
    String primaryServer, secondaryServer, timezone;
    int syncInterval;
    
    bool success = m_configStorage->loadTimeConfigAsync(primaryServer, secondaryServer, 
                                                       timezone, syncInterval, 3000);
    
    if (success) {
        m_ntpConfig.primaryServer = primaryServer;
        m_ntpConfig.secondaryServer = secondaryServer;
        m_ntpConfig.timezone = timezone;
        m_ntpConfig.syncInterval = syncInterval;
        
        // 解析时区偏移
        if (timezone.startsWith("CST")) {
            m_timezoneOffset = 8.0f;
        } else if (timezone.startsWith("UTC")) {
            String offsetStr = timezone.substring(3);
            m_timezoneOffset = offsetStr.toFloat();
        }
        
        printf("[TimeManager] 配置已从NVS加载\n");
    } else {
        printf("[TimeManager] 配置加载失败，使用默认配置\n");
    }
}

void TimeManager::saveConfig() {
    if (!m_configStorage) {
        printf("[TimeManager] 配置存储未初始化，无法保存配置\n");
        return;
    }
    
    // 异步保存时间配置到NVS
    bool success = m_configStorage->saveTimeConfigAsync(m_ntpConfig.primaryServer, 
                                                       m_ntpConfig.secondaryServer,
                                                       m_ntpConfig.timezone, 
                                                       m_ntpConfig.syncInterval, 3000);
    
    if (success) {
        printf("[TimeManager] 配置已保存到NVS\n");
    } else {
        printf("[TimeManager] 配置保存失败\n");
    }
}

String TimeManager::formatTime(const struct tm* timeinfo, const String& format) const {
    if (!timeinfo) {
        return "Invalid Time";
    }
    
    char buffer[100];
    strftime(buffer, sizeof(buffer), format.c_str(), timeinfo);
    return String(buffer);
}

bool TimeManager::isValidTimeZone(const String& timezone) {
    // 简单的时区格式验证
    return timezone.startsWith("UTC") || timezone.startsWith("CST") || 
           timezone.startsWith("EST") || timezone.startsWith("PST") ||
           timezone.startsWith("GMT");
}

bool TimeManager::takeMutex(TickType_t timeout) const {
    if (!m_mutex) return false;
    return xSemaphoreTake(m_mutex, timeout) == pdTRUE;
}

void TimeManager::giveMutex() const {
    if (m_mutex) {
        xSemaphoreGive(m_mutex);
    }
} 