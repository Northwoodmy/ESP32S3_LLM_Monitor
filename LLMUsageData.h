#ifndef LLM_USAGE_DATA_H
#define LLM_USAGE_DATA_H

#include <stdint.h>

/**
 * @brief 大模型平台类型
 */
enum ProviderType {
    PROVIDER_NONE = 0,      ///< 未配置/禁用
    PROVIDER_DEEPSEEK,      ///< DeepSeek(余额型)
    PROVIDER_KIMICODE       ///< Kimi Code(配额型)
};

/**
 * @brief 单个平台用量数据
 *
 * 支持两种数据形态:
 * - 余额型(DeepSeek): balance/cost_today/cost_total/est_tokens_today
 * - 配额型(Kimi Code): weekly/win5h 的 used/limit 与重置时间
 */
struct ProviderData {
    ProviderType type;          ///< 平台类型
    char name[16];              ///< 平台名称 "DeepSeek" / "Kimi Code"
    bool enabled;               ///< 是否启用
    char state[16];             ///< 状态(屏幕字体仅支持ASCII): "OK"/"AUTH ERR"/"NET ERR"/"PARSE ERR"/"NO KEY"/"DISABLED"

    // === 余额型字段(DeepSeek) ===
    double balance;             ///< 总余额(CNY)
    double cash_balance;        ///< 充值/现金余额
    double voucher_balance;     ///< 赠送/代金券余额
    double cost_today;          ///< 今日消耗(差分累计,NV持久化)
    double cost_total;          ///< 累计消耗
    double unit_price;          ///< 估算单价(¥/M tokens)
    uint64_t est_tokens_today;  ///< 估算今日tokens

    // === 配额型字段(Kimi Code) ===
    int weekly_used;            ///< 7天周用量(已用)
    int weekly_limit;           ///< 7天周用量(上限)
    int win5h_used;             ///< 5小时窗口(已用)
    int win5h_limit;            ///< 5小时窗口(上限)
    char weekly_reset[32];      ///< 周重置倒计时(如 "3d 5h 20m")
    char win5h_reset[32];       ///< 5小时窗口重置倒计时

    char updated[24];           ///< 最近更新时间 "HH:MM:SS"
    bool valid;                 ///< 数据有效性
};

/**
 * @brief 本机系统数据
 */
struct LLMSystemData {
    unsigned long boot_time;    ///< 启动时间
    int reset_reason;           ///< 重置原因
    unsigned long free_heap;    ///< 可用内存
    bool valid;
};

/**
 * @brief 大模型用量监控数据(4个槽位,当前使用2个)
 */
struct LLMUsageData {
    ProviderData providers[4];  ///< 平台数据数组
    double total_cost_today;    ///< 今日总消耗(余额型平台合计)
    double total_balance;       ///< 余额合计(余额型平台)
    LLMSystemData system;       ///< 本机系统数据
    unsigned long timestamp;    ///< 数据时间戳
    bool valid;                 ///< 数据有效性
};

// 用量数据回调函数类型
typedef void (*LLMUsageCallback)(const LLMUsageData& data, void* userData);

#endif // LLM_USAGE_DATA_H
