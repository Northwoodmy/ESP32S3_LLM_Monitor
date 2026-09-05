# ESP32S3_LLM_Monitor - 大模型用量监控器

## 项目简介

基于 ESP32-S3 (SH8601 AMOLED 圆形触摸屏) 的大模型用量监控设备，直连官方 API 实时显示 DeepSeek 账户余额/消耗与 Kimi Code 订阅配额使用情况。使用 Arduino IDE 开发，FreeRTOS 多任务架构，LVGL 图形界面。

## 监控平台

### DeepSeek（余额型）
- 数据源：`GET https://api.deepseek.com/user/balance`
- 显示：总余额、现金/赠送余额、今日消耗、累计消耗、估算 tokens
- 消耗追踪：官方无用量查询 API，设备通过**余额差分**累计消耗（余额下降计入消耗，上升识别为充值），状态 NVS 持久化，跨零点自动清零今日消耗，断电不丢失
- 估算 tokens = 今日消耗 ÷ 可配置单价（元/百万 tokens）

### Kimi Code（配额型）
- 数据源：`GET https://api.kimi.com/coding/v1/usages`（兼容 `/usage` 回退与两种响应格式）
- 显示：5 小时窗口用量/上限/重置倒计时、7 天（周）用量/上限/重置倒计时
- 注意：需使用 **Kimi Code 控制台**的 `sk-kimi-` 密钥，开放平台的 `sk-` 密钥会返回 401

## 屏幕页面（手势导航）

| 页面 | 内容 | 手势 |
|---|---|---|
| 待机页 | 时间/日期/星期/天气/今日消耗 | 左滑→总览，右滑×3→WiFi信息 |
| 总览页 | 今日总消耗 + 4 槽位(DS余额/DS消耗/Kimi 5h/Kimi 7d) | 左滑→DS主页，右滑→待机，点击槽位→对应主页 |
| DS 主数据页 | 余额大数字 + 今日消耗/估算Tokens/累计 | 左滑→Kimi，右滑→总览，上滑→DS详情 |
| DS 详情页 | 状态/套餐/现金/赠送/单价/更新时间 | 下滑→返回DS主页 |
| Kimi 主数据页 | 7天用量%大数字 + 5h%/重置倒计时 | 左滑→待机，右滑→DS主页，上滑→Kimi详情 |
| Kimi 详情页 | 状态/套餐/7d原始值/5h原始值/重置时间 | 下滑→返回Kimi主页 |

> 屏幕标签使用英文（界面字体为 ASCII + 少量中文子集）。

## Web 配置界面

连接设备 WiFi（或同一局域网访问设备 IP）后浏览器打开：

- `/` — 首页：WiFi 配置、设备信息、系统管理
- `/llm-settings` — **大模型设置**：4 个平台槽位（禁用/DeepSeek/Kimi Code）、API Key 配置（掩码显示、留空保持不变）、DeepSeek 估算单价、全局轮询间隔（30-600 秒）、每槽位连接测试、实时状态预览
- `/weather-settings` — 天气设置（高德天气 API）
- `/screen-settings` — 屏幕设置（亮度/屏幕模式/旋转）
- `/settings` — 时间设置（NTP）
- `/files` — 文件管理器（SPIFFS）

## 硬件平台

- ESP32-S3 (8MB OPI PSRAM, 16MB Flash)
- SH8601 AMOLED 圆形显示屏 + 触摸
- QMI8658 IMU（屏幕自动旋转）
- ES8311 音频（预留）

## 编译

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CPUFreq=240" .
```

分区表：app0/app1 各 6MB（保留 OTA 分区以便将来扩展），SPIFFS 约 4MB。

## 版本

v1.0.0 — 由功率监控项目重构为大模型用量监控：移除功率监控与 OTA 功能，仅保留 UI1 主题，新增 DeepSeek/Kimi Code 官方 API 直连监控。
