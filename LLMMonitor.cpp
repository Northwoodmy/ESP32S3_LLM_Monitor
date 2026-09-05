/*
 * LLMMonitor.cpp - 大模型用量监控器实现
 * ESP32S3_LLM_Monitor 项目
 *
 * 数据流:
 *   配置(NVS) -> loadConfig -> 槽位数组
 *   定时轮询 -> HTTPS 请求 -> JSON 解析 -> ProviderData
 *   DeepSeek 余额差分 -> cost_today/cost_total (NVS 持久化)
 *   汇总 LLMUsageData -> 回调 -> DisplayManager
 *
 * API 说明:
 * - DeepSeek: GET https://api.deepseek.com/user/balance
 *     Header: Authorization: Bearer sk-xxx
 *     响应: {"is_available":true,"balance_infos":[{"currency":"CNY",
 *           "total_balance":"10.00","granted_balance":"0.00","topped_up_balance":"10.00"}]}
 * - Kimi Code: GET https://api.kimi.com/coding/v1/usages (404 时回退 /usage)
 *     Header: Authorization: Bearer sk-kimi-xxx, User-Agent: KimiCLI/1.6
 *     响应形态A: {"data":[{"model_name":"all","used":96,"limit":100,"resetTime":...}, ...]}
 *     响应形态B: {"usage":{...},"limits":[{"detail":{...},"window":{"duration":5,"timeUnit":"HOUR"}}]}
 */

#include "LLMMonitor.h"
#include "PSRAMManager.h"
#include "ConfigStorage.h"
#include "Arduino.h"
#include <WiFiClientSecure.h>
#include <time.h>
#include <cstring>
#include <cstdlib>

// === NVS 配置键名(通过 ConfigStorage 通用 KV 接口,存于 system 命名空间) ===
static const char* KEY_POLL_SEC    = "llm_poll_sec";   // int: 轮询间隔(秒)
static const char* KEY_SLOT_TYPE   = "llm_s%d_type";   // int: 0=禁用 1=DeepSeek 2=KimiCode
static const char* KEY_SLOT_KEY    = "llm_s%d_key";    // string: API Key
static const char* KEY_SLOT_PRICE  = "llm_s%d_price";  // string: 估算单价 ¥/M tokens
static const char* KEY_DS_LAST_BAL = "ds_last_bal";    // string: 上次余额
static const char* KEY_DS_COST_DAY = "ds_cost_today";  // string: 今日累计消耗
static const char* KEY_DS_COST_ALL = "ds_cost_total";  // string: 历史累计消耗
static const char* KEY_DS_DATE     = "ds_cost_date";   // string: 消耗归属日期
static const char* KEY_DS_BASELINE = "ds_baseline";    // int: 基线是否已建立

// 状态字符串(屏幕字体仅支持 ASCII,故状态用英文)
static const char* STATE_OK        = "OK";
static const char* STATE_AUTH_ERR  = "AUTH ERR";
static const char* STATE_NET_ERR   = "NET ERR";
static const char* STATE_PARSE_ERR = "PARSE ERR";
static const char* STATE_NO_KEY    = "NO KEY";
static const char* STATE_DISABLED  = "DISABLED";

// HTTPS 超时
static const uint32_t HTTP_TIMEOUT_MS = 10000;

// === 文件本地辅助函数 ===

/**
 * @brief 从 JSON 对象按多个候选键名取整数值
 */
static bool jsonGetInt(JsonObject obj, const char* const* keys, size_t keyCount, int& out) {
    for (size_t i = 0; i < keyCount; i++) {
        if (!obj[keys[i]].isNull()) {
            out = obj[keys[i]].as<int>();
            return true;
        }
    }
    return false;
}

/**
 * @brief 由 UTC 年月日时分秒计算 unix 时间戳(days-from-civil 算法, 不依赖 timegm)
 */
static time_t epochFromUtc(int y, int m, int d, int hh, int mm, int ss) {
    y -= (m <= 2) ? 1 : 0;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(m > 2 ? m - 3 : m + 9) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (time_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
}

/**
 * @brief 解析重置时间为 unix 时间戳(秒)
 *
 * 支持:
 * - 数字: >1e9 视为 unix 时间戳; 否则视为"剩余秒数"
 * - 字符串: ISO8601 "2026-09-12T08:00:00Z" 或纯数字字符串
 *
 * @param now 当前时间戳(用于剩余秒数换算)
 * @return unix 时间戳(秒), 失败返回 0
 */
static time_t parseResetTime(JsonVariant v, time_t now) {
    if (v.isNull()) return 0;

    if (v.is<double>() || v.is<long>() || v.is<unsigned long>()) {
        double d = v.as<double>();
        if (d > 1e9) return (time_t)d;              // unix 时间戳
        if (d > 0)   return now + (time_t)d;        // 剩余秒数
        return 0;
    }

    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (!s || !*s) return 0;

        // 纯数字字符串
        if (s[0] >= '0' && s[0] <= '9' && strspn(s, "0123456789") == strlen(s)) {
            double d = strtod(s, nullptr);
            if (d > 1e9) return (time_t)d;
            if (d > 0)   return now + (time_t)d;
            return 0;
        }

        // ISO8601: 2026-09-12T08:00:00Z / +08:00 等
        int year, mon, day, hh, mm, ss = 0;
        if (sscanf(s, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hh, &mm, &ss) >= 5) {
            return epochFromUtc(year, mon, day, hh, mm, ss);   // 按 UTC 解析
        }
    }
    return 0;
}

/**
 * @brief 把"目标时间戳 - 当前时间"格式化为倒计时字符串 "3d 5h 20m" / "2h 15m" / "45m"
 */
static void formatCountdown(time_t target, time_t now, char* buf, size_t len) {
    if (target <= 0 || now <= 0 || target <= now) {
        snprintf(buf, len, "--");
        return;
    }
    long diff = (long)(target - now);
    int days  = diff / 86400;
    int hours = (diff % 86400) / 3600;
    int mins  = (diff % 3600) / 60;

    if (days > 0) {
        snprintf(buf, len, "%dd %dh %dm", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buf, len, "%dh %dm", hours, mins);
    } else {
        snprintf(buf, len, "%dm", mins > 0 ? mins : 1);
    }
}

LLMMonitor::LLMMonitor()
    : m_taskHandle(nullptr),
      m_psramManager(nullptr),
      m_configStorage(nullptr),
      m_isRunning(false),
      m_configDirty(false),
      m_requestInterval(60000),  // 默认60秒轮询
      m_pollIntervalSec(60),
      m_usageCallback(nullptr),
      m_callbackUserData(nullptr),
      m_dsLastBalance(0),
      m_dsBalanceValid(false),
      m_dsCostToday(0),
      m_dsCostTotal(0) {
    memset(&m_currentData, 0, sizeof(m_currentData));
    for (int i = 0; i < 4; i++) {
        m_currentData.providers[i].type = PROVIDER_NONE;
        strcpy(m_currentData.providers[i].state, STATE_DISABLED);
    }
    m_currentData.valid = false;
    m_dsCostDate[0] = '\0';
}

LLMMonitor::~LLMMonitor() {
    stop();
}

void LLMMonitor::init() {
    init(nullptr);
}

void LLMMonitor::init(PSRAMManager* psramManager) {
    init(psramManager, nullptr);
}

void LLMMonitor::init(PSRAMManager* psramManager, ConfigStorage* configStorage) {
    if (m_isRunning) {
        printf("LLM监控器已经在运行中\n");
        return;
    }

    m_psramManager = psramManager;
    m_configStorage = configStorage;

    printf("正在启动LLM用量监控任务...\n");

    // 加载配置(含 NVS 中持久化的消耗状态)
    loadConfig();

    m_isRunning = true;

    // 网络任务必须使用内部SRAM栈(lwip要求); HTTPS + JSON 需要较大栈
    BaseType_t result = xTaskCreatePinnedToCore(
        monitorTask,
        "LLMMonitorTask",
        10240,
        this,
        3,
        &m_taskHandle,
        0
    );

    if (result == pdPASS) {
        printf("LLM用量监控任务(SRAM栈)创建成功, 轮询间隔: %u 秒\n", m_pollIntervalSec);
    } else {
        m_isRunning = false;
        printf("LLM用量监控任务创建失败\n");
    }
}

void LLMMonitor::stop() {
    if (!m_isRunning) {
        return;
    }
    printf("正在停止LLM监控任务...\n");
    if (m_taskHandle != nullptr) {
        vTaskDelete(m_taskHandle);
        m_taskHandle = nullptr;
    }
    m_isRunning = false;
    printf("LLM监控任务已停止\n");
}

void LLMMonitor::setUsageCallback(LLMUsageCallback callback, void* userData) {
    m_usageCallback = callback;
    m_callbackUserData = userData;
}

const LLMUsageData& LLMMonitor::getCurrentUsageData() const {
    return m_currentData;
}

void LLMMonitor::reloadConfig() {
    m_configDirty = true;
}

/**
 * @brief 测试指定平台连接(供 Web 测试接口使用)
 *
 * 在调用者(Web服务器)任务上下文中执行一次性 HTTPS 请求,
 * 使用临时配置, 不影响正常轮询和消耗追踪状态。
 */
bool LLMMonitor::testProvider(ProviderType type, const String& apiKey, double unitPrice, String& message) {
    if (!isWiFiConnected()) {
        message = "WiFi 未连接, 无法测试";
        return false;
    }

    ProviderSlot slot;
    slot.type = type;
    slot.apiKey = apiKey;
    slot.unitPrice = unitPrice;

    ProviderData data;
    memset(&data, 0, sizeof(data));

    bool ok = false;
    if (type == PROVIDER_DEEPSEEK) {
        ok = fetchDeepSeek(slot, data, true);   // testOnly: 不做差分追踪
        if (ok) {
            char buf[80];
            snprintf(buf, sizeof(buf), "连接成功! 余额 %.2f CNY (现金 %.2f + 赠送 %.2f)",
                     data.balance, data.cash_balance, data.voucher_balance);
            message = buf;
        }
    } else if (type == PROVIDER_KIMICODE) {
        ok = fetchKimiCode(slot, data);
        if (ok) {
            char buf[96];
            snprintf(buf, sizeof(buf), "连接成功! 7天 %d/%d, 5小时 %d/%d",
                     data.weekly_used, data.weekly_limit, data.win5h_used, data.win5h_limit);
            message = buf;
        }
    } else {
        message = "未选择平台类型";
        return false;
    }

    if (!ok) {
        message = String("连接失败: ") + data.state;
        if (strcmp(data.state, "AUTH ERR") == 0 && type == PROVIDER_KIMICODE) {
            message += " (Kimi Code 请使用 coding 控制台的 sk-kimi- 密钥)";
        }
    }
    return ok;
}

bool LLMMonitor::isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void LLMMonitor::currentDateString(char* buf, size_t len) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    if (t && (t->tm_year + 1900) >= 2020) {
        strftime(buf, len, "%Y-%m-%d", t);
    } else {
        buf[0] = '\0';   // 时间未同步
    }
}

void LLMMonitor::currentTimeString(char* buf, size_t len) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    if (t && (t->tm_year + 1900) >= 2020) {
        strftime(buf, len, "%H:%M:%S", t);
    } else {
        snprintf(buf, len, "--:--:--");
    }
}

/**
 * @brief 从 NVS 加载配置与 DeepSeek 消耗状态
 */
void LLMMonitor::loadConfig() {
    printf("[LLMMonitor] 加载配置...\n");

    if (!m_configStorage) {
        printf("[LLMMonitor] 警告: 配置存储未初始化, 使用默认配置\n");
        return;
    }

    // 轮询间隔(30s - 600s, 默认60s)
    int pollSec = m_configStorage->getIntAsync(KEY_POLL_SEC, 60, 3000);
    if (pollSec < 30) pollSec = 30;
    if (pollSec > 600) pollSec = 600;
    m_pollIntervalSec = (uint32_t)pollSec;
    m_requestInterval = m_pollIntervalSec * 1000;

    // 槽位配置
    char keyBuf[24];
    for (int i = 0; i < 4; i++) {
        snprintf(keyBuf, sizeof(keyBuf), KEY_SLOT_TYPE, i);
        int type = m_configStorage->getIntAsync(keyBuf, 0, 3000);
        m_slots[i].type = (type >= PROVIDER_NONE && type <= PROVIDER_KIMICODE)
                          ? (ProviderType)type : PROVIDER_NONE;

        snprintf(keyBuf, sizeof(keyBuf), KEY_SLOT_KEY, i);
        m_slots[i].apiKey = m_configStorage->getStringAsync(keyBuf, "", 3000);

        snprintf(keyBuf, sizeof(keyBuf), KEY_SLOT_PRICE, i);
        String price = m_configStorage->getStringAsync(keyBuf, "2.0", 3000);
        m_slots[i].unitPrice = atof(price.c_str());
        if (m_slots[i].unitPrice <= 0) m_slots[i].unitPrice = 2.0;

        if (m_slots[i].type != PROVIDER_NONE) {
            printf("[LLMMonitor] 槽位%d: 类型=%d, Key%s配置, 单价=%.1f\n",
                   i, m_slots[i].type,
                   m_slots[i].apiKey.length() > 0 ? "已" : "未",
                   m_slots[i].unitPrice);
        }
    }

    // DeepSeek 消耗状态(NVS 持久化)
    m_dsBalanceValid = m_configStorage->getIntAsync(KEY_DS_BASELINE, 0, 3000) == 1;
    m_dsLastBalance  = atof(m_configStorage->getStringAsync(KEY_DS_LAST_BAL, "0", 3000).c_str());
    m_dsCostToday    = atof(m_configStorage->getStringAsync(KEY_DS_COST_DAY, "0", 3000).c_str());
    m_dsCostTotal    = atof(m_configStorage->getStringAsync(KEY_DS_COST_ALL, "0", 3000).c_str());
    String date      = m_configStorage->getStringAsync(KEY_DS_DATE, "", 3000);
    strncpy(m_dsCostDate, date.c_str(), sizeof(m_dsCostDate) - 1);
    m_dsCostDate[sizeof(m_dsCostDate) - 1] = '\0';

    printf("[LLMMonitor] 配置加载完成: 轮询=%us, DeepSeek基线=%s, 今日消耗=%.4f, 累计消耗=%.4f\n",
           m_pollIntervalSec, m_dsBalanceValid ? "已建立" : "未建立",
           m_dsCostToday, m_dsCostTotal);

    // 跨零点检查
    checkDayRollover();
}

/**
 * @brief 持久化 DeepSeek 消耗状态到 NVS
 */
void LLMMonitor::persistDSState() {
    if (!m_configStorage) return;

    char valBuf[24];
    snprintf(valBuf, sizeof(valBuf), "%.4f", m_dsLastBalance);
    m_configStorage->putStringAsync(KEY_DS_LAST_BAL, valBuf, 3000);
    snprintf(valBuf, sizeof(valBuf), "%.4f", m_dsCostToday);
    m_configStorage->putStringAsync(KEY_DS_COST_DAY, valBuf, 3000);
    snprintf(valBuf, sizeof(valBuf), "%.4f", m_dsCostTotal);
    m_configStorage->putStringAsync(KEY_DS_COST_ALL, valBuf, 3000);
    m_configStorage->putStringAsync(KEY_DS_DATE, m_dsCostDate, 3000);
    m_configStorage->putIntAsync(KEY_DS_BASELINE, m_dsBalanceValid ? 1 : 0, 3000);
}

/**
 * @brief 跨零点检查: 日期变化时清零今日消耗
 */
void LLMMonitor::checkDayRollover() {
    char today[12];
    currentDateString(today, sizeof(today));
    if (today[0] == '\0') return;   // 时间未同步, 不处理

    if (strcmp(today, m_dsCostDate) != 0) {
        printf("[LLMMonitor] 跨天检测: %s -> %s, 今日消耗清零(昨日: %.4f)\n",
               m_dsCostDate[0] ? m_dsCostDate : "无", today, m_dsCostToday);
        strncpy(m_dsCostDate, today, sizeof(m_dsCostDate) - 1);
        m_dsCostDate[sizeof(m_dsCostDate) - 1] = '\0';
        m_dsCostToday = 0;
        persistDSState();
    }
}

/**
 * @brief DeepSeek 差分消耗追踪
 *
 * 余额下降 -> 消耗, 累加到 cost_today/cost_total
 * 余额上升 -> 充值事件, 仅更新基线不计入
 * 首次运行 -> 仅记录基线
 */
void LLMMonitor::trackDeepSeekCost(double balance, ProviderData& out) {
    checkDayRollover();

    if (!m_dsBalanceValid) {
        // 首次运行: 建立基线
        m_dsLastBalance = balance;
        m_dsBalanceValid = true;
        printf("[LLMMonitor] DeepSeek 余额基线建立: %.4f\n", balance);
        persistDSState();
    } else {
        double delta = m_dsLastBalance - balance;
        if (delta > 0.0001) {
            // 余额下降: 消耗
            m_dsCostToday += delta;
            m_dsCostTotal += delta;
            printf("[LLMMonitor] DeepSeek 消耗: -%.4f (今日累计 %.4f)\n", delta, m_dsCostToday);
        } else if (delta < -0.0001) {
            // 余额上升: 充值/赠送
            printf("[LLMMonitor] DeepSeek 充值事件: +%.4f (新余额 %.4f)\n", -delta, balance);
        }
        m_dsLastBalance = balance;
        persistDSState();
    }

    // 填充到输出
    out.cost_today = m_dsCostToday;
    out.cost_total = m_dsCostTotal;

    // 估算今日 tokens: 消耗(¥) / 单价(¥/M) * 1M
    double unitPrice = out.unit_price > 0 ? out.unit_price : 2.0;
    out.est_tokens_today = (uint64_t)(m_dsCostToday / unitPrice * 1000000.0);
}

/**
 * @brief 获取 DeepSeek 余额
 */
bool LLMMonitor::fetchDeepSeek(ProviderSlot& slot, ProviderData& out, bool testOnly) {
    out.type = PROVIDER_DEEPSEEK;
    strncpy(out.name, "DeepSeek", sizeof(out.name) - 1);
    out.unit_price = slot.unitPrice;

    if (slot.apiKey.length() == 0) {
        strcpy(out.state, STATE_NO_KEY);
        out.valid = false;
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();   // 跳过证书校验, 避免证书轮换问题
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, "https://api.deepseek.com/user/balance")) {
        strcpy(out.state, STATE_NET_ERR);
        out.valid = false;
        return false;
    }
    http.addHeader("Authorization", "Bearer " + slot.apiKey);
    http.addHeader("Accept", "application/json");

    int httpCode = http.GET();
    if (httpCode == 401 || httpCode == 403) {
        printf("[LLMMonitor] DeepSeek 认证失败(HTTP %d)\n", httpCode);
        strcpy(out.state, STATE_AUTH_ERR);
        out.valid = false;
        http.end();
        return false;
    }
    if (httpCode != HTTP_CODE_OK) {
        printf("[LLMMonitor] DeepSeek 请求失败(HTTP %d)\n", httpCode);
        strcpy(out.state, STATE_NET_ERR);
        out.valid = false;
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(1536);
    if (deserializeJson(doc, payload) != DeserializationError::Ok) {
        printf("[LLMMonitor] DeepSeek 响应解析失败\n");
        strcpy(out.state, STATE_PARSE_ERR);
        out.valid = false;
        return false;
    }

    JsonArray infos = doc["balance_infos"].as<JsonArray>();
    if (infos.isNull() || infos.size() == 0) {
        strcpy(out.state, STATE_PARSE_ERR);
        out.valid = false;
        return false;
    }

    // 取第一个货币条目(通常 CNY)
    JsonObject info = infos[0];
    out.balance         = info["total_balance"].as<String>().toDouble();
    out.voucher_balance = info["granted_balance"].as<String>().toDouble();
    out.cash_balance    = info["topped_up_balance"].as<String>().toDouble();

    strcpy(out.state, STATE_OK);
    out.valid = true;
    currentTimeString(out.updated, sizeof(out.updated));

    // 差分消耗追踪(测试模式下跳过)
    if (!testOnly) {
        trackDeepSeekCost(out.balance, out);
    }

    printf("[LLMMonitor] DeepSeek: 余额=%.2f (现金 %.2f + 赠送 %.2f), 今日消耗=%.4f\n",
           out.balance, out.cash_balance, out.voucher_balance, out.cost_today);
    return true;
}

/**
 * @brief 获取 Kimi Code 配额用量(5小时/7天窗口)
 */
bool LLMMonitor::fetchKimiCode(ProviderSlot& slot, ProviderData& out) {
    out.type = PROVIDER_KIMICODE;
    strncpy(out.name, "Kimi Code", sizeof(out.name) - 1);

    if (slot.apiKey.length() == 0) {
        strcpy(out.state, STATE_NO_KEY);
        out.valid = false;
        return false;
    }

    String payload;
    bool gotResponse = false;

    // 优先 /usages, 404 时回退 /usage
    const char* paths[] = { "/coding/v1/usages", "/coding/v1/usage" };
    for (int attempt = 0; attempt < 2 && !gotResponse; attempt++) {
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(HTTP_TIMEOUT_MS / 1000);

        HTTPClient http;
        http.setTimeout(HTTP_TIMEOUT_MS);
        String url = String("https://api.kimi.com") + paths[attempt];
        if (!http.begin(client, url)) {
            continue;
        }
        http.addHeader("Authorization", "Bearer " + slot.apiKey);
        http.addHeader("User-Agent", "KimiCLI/1.6");
        http.addHeader("Accept", "application/json");

        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            payload = http.getString();
            gotResponse = true;
        } else if (httpCode == 401 || httpCode == 403) {
            // 认证失败: 多半用了 platform 的 sk-xxx 而非 Kimi Code 的 sk-kimi-xxx
            printf("[LLMMonitor] Kimi Code 认证失败(HTTP %d), 请确认使用 Kimi Code 控制台的 sk-kimi- 密钥\n", httpCode);
            strcpy(out.state, STATE_AUTH_ERR);
            out.valid = false;
            http.end();
            return false;
        } else if (httpCode == 404 && attempt == 0) {
            printf("[LLMMonitor] Kimi /usages 不存在, 尝试 /usage\n");
        } else {
            printf("[LLMMonitor] Kimi Code 请求失败(HTTP %d)\n", httpCode);
            strcpy(out.state, STATE_NET_ERR);
            out.valid = false;
            http.end();
            return false;
        }
        http.end();
    }

    if (!gotResponse) {
        strcpy(out.state, STATE_NET_ERR);
        out.valid = false;
        return false;
    }

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, payload) != DeserializationError::Ok) {
        printf("[LLMMonitor] Kimi Code 响应解析失败\n");
        strcpy(out.state, STATE_PARSE_ERR);
        out.valid = false;
        return false;
    }

    time_t now = time(nullptr);
    const char* limitKeys[] = { "limit", "limit_amount", "quota" };
    const char* usedKeys[]  = { "used", "used_amount" };
    const char* resetKeys[] = { "resetTime", "reset_at", "reset_time", "resetIn", "reset_in" };

    time_t weeklyResetAt = 0, win5hResetAt = 0;
    bool foundWeekly = false, foundWin5h = false;

    // === 形态A: {"data":[{"model_name":"all",...}, ...]} ===
    JsonArray dataArr = doc["data"].as<JsonArray>();
    if (!dataArr.isNull()) {
        for (JsonObject entry : dataArr) {
            int limit = 0, used = 0;
            jsonGetInt(entry, limitKeys, 3, limit);
            jsonGetInt(entry, usedKeys, 2, used);
            // 没有 used 但有 remaining 时反推
            if (used == 0 && !entry["remaining"].isNull() && limit > 0) {
                used = limit - entry["remaining"].as<int>();
                if (used < 0) used = 0;
            }

            time_t resetAt = 0;
            for (size_t k = 0; k < 5 && resetAt == 0; k++) {
                resetAt = parseResetTime(entry[resetKeys[k]], now);
            }

            String modelName = entry["model_name"] | "";
            if (modelName == "all") {
                // "all" 条目为 7天汇总
                out.weekly_used = used;
                out.weekly_limit = limit;
                weeklyResetAt = resetAt;
                foundWeekly = true;
            } else {
                // 其他条目视为 5小时窗口
                out.win5h_used = used;
                out.win5h_limit = limit;
                win5hResetAt = resetAt;
                foundWin5h = true;
            }
        }
    }

    // === 形态B: {"usage":{...},"limits":[{"detail":{...},"window":{...}}]} ===
    if (!foundWeekly && !foundWin5h) {
        JsonArray limits = doc["limits"].as<JsonArray>();
        if (!limits.isNull()) {
            for (JsonObject entry : limits) {
                JsonObject detail = entry["detail"].as<JsonObject>();
                JsonObject window = entry["window"].as<JsonObject>();
                if (detail.isNull()) detail = entry;

                int limit = 0, used = 0;
                jsonGetInt(detail, limitKeys, 3, limit);
                jsonGetInt(detail, usedKeys, 2, used);
                if (used == 0 && !detail["remaining"].isNull() && limit > 0) {
                    used = limit - detail["remaining"].as<int>();
                    if (used < 0) used = 0;
                }

                time_t resetAt = 0;
                for (size_t k = 0; k < 5 && resetAt == 0; k++) {
                    resetAt = parseResetTime(detail[resetKeys[k]], now);
                }

                // 依据 window 判断窗口类型
                int duration = window.isNull() ? 0 : window["duration"].as<int>();
                String unit = "";
                if (!window.isNull()) {
                    if (!window["timeUnit"].isNull()) unit = window["timeUnit"].as<String>();
                    else if (!window["time_unit"].isNull()) unit = window["time_unit"].as<String>();
                }

                bool isWeekly = (unit == "DAY" && duration >= 7) ||
                                (unit == "WEEK") ||
                                (unit == "" && duration >= 24 * 7);
                bool is5h = (unit == "HOUR" && duration <= 5) ||
                            (unit == "" && duration > 0 && duration <= 5);

                if (isWeekly) {
                    out.weekly_used = used;
                    out.weekly_limit = limit;
                    weeklyResetAt = resetAt;
                    foundWeekly = true;
                } else if (is5h) {
                    out.win5h_used = used;
                    out.win5h_limit = limit;
                    win5hResetAt = resetAt;
                    foundWin5h = true;
                } else if (!foundWeekly && limit > 0) {
                    // 无法识别窗口: 第一个未知条目按周用量处理
                    out.weekly_used = used;
                    out.weekly_limit = limit;
                    weeklyResetAt = resetAt;
                    foundWeekly = true;
                }
            }
        }
    }

    if (!foundWeekly && !foundWin5h) {
        printf("[LLMMonitor] Kimi Code 响应中未找到配额数据: %.200s\n", payload.c_str());
        strcpy(out.state, STATE_PARSE_ERR);
        out.valid = false;
        return false;
    }

    formatCountdown(weeklyResetAt, now, out.weekly_reset, sizeof(out.weekly_reset));
    formatCountdown(win5hResetAt, now, out.win5h_reset, sizeof(out.win5h_reset));

    strcpy(out.state, STATE_OK);
    out.valid = true;
    currentTimeString(out.updated, sizeof(out.updated));

    printf("[LLMMonitor] Kimi Code: 7天=%d/%d, 5小时=%d/%d, 7d重置=%s, 5h重置=%s\n",
           out.weekly_used, out.weekly_limit, out.win5h_used, out.win5h_limit,
           out.weekly_reset, out.win5h_reset);
    return true;
}

/**
 * @brief 逐个平台获取数据并汇总
 */
void LLMMonitor::fetchAllProviders() {
    bool anyValid = false;

    for (int i = 0; i < 4; i++) {
        ProviderData& out = m_currentData.providers[i];
        ProviderSlot& slot = m_slots[i];

        if (slot.type == PROVIDER_NONE) {
            // 未配置槽位: 保持禁用状态
            out.type = PROVIDER_NONE;
            out.enabled = false;
            out.valid = false;
            strcpy(out.state, STATE_DISABLED);
            out.name[0] = '\0';
            continue;
        }

        out.enabled = true;
        bool ok = false;

        // 顺序请求, 避免并发 TLS 占用过多堆内存
        if (slot.type == PROVIDER_DEEPSEEK) {
            ok = fetchDeepSeek(slot, out);
        } else if (slot.type == PROVIDER_KIMICODE) {
            ok = fetchKimiCode(slot, out);
        }

        if (ok) anyValid = true;
    }

    // 汇总(仅余额型平台参与金额合计)
    double totalCost = 0, totalBalance = 0;
    for (int i = 0; i < 4; i++) {
        const ProviderData& p = m_currentData.providers[i];
        if (p.enabled && p.valid && p.type == PROVIDER_DEEPSEEK) {
            totalCost += p.cost_today;
            totalBalance += p.balance;
        }
    }
    m_currentData.total_cost_today = totalCost;
    m_currentData.total_balance = totalBalance;
    m_currentData.timestamp = millis();
    m_currentData.valid = anyValid;

    // 回调通知(显示更新)
    if (m_usageCallback) {
        m_usageCallback(m_currentData, m_callbackUserData);
    }
}

void LLMMonitor::monitorTask(void* parameter) {
    LLMMonitor* self = static_cast<LLMMonitor*>(parameter);

    printf("LLM用量监控任务开始运行\n");

    while (self->m_isRunning) {
        if (self->m_configDirty) {
            self->m_configDirty = false;
            self->loadConfig();
        }

        if (self->isWiFiConnected()) {
            self->fetchAllProviders();
        } else {
            // WiFi 断开: 将所有已启用槽位标记为网络错误
            for (int i = 0; i < 4; i++) {
                ProviderData& p = self->m_currentData.providers[i];
                if (p.enabled) {
                    strcpy(p.state, STATE_NET_ERR);
                    p.valid = false;
                }
            }
            if (self->m_usageCallback) {
                self->m_usageCallback(self->m_currentData, self->m_callbackUserData);
            }
        }

        // 分段延时, 便于及时响应配置变更和停止
        uint32_t waited = 0;
        while (self->m_isRunning && !self->m_configDirty && waited < self->m_requestInterval) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited += 1000;
        }
    }

    printf("LLM用量监控任务结束\n");
    vTaskDelete(nullptr);
}
