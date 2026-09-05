/*
 * ESP32S3_LLM_Monitor - 大模型用量监控项目
 * 版本: 由Version.h统一管理
 *
 * 功能说明:
 * - DeepSeek 余额/消耗监控(官方API直连)
 * - Kimi Code 5小时/7天配额监控(官方API直连)
 * - WiFi自动配置和管理
 * - 现代化Web配置界面(大模型API Key配置)
 * - NVS存储配置信息
 * - FreeRTOS多任务架构
 * - LVGL显示驱动和触摸手势
 * - PSRAM内存管理和优化
 * - NTP网络时间同步功能
 * - WeatherManager天气管理系统(高德天气API)
 * - 定位管理系统(高德定位API)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

#include "Version.h"
#include "ConfigStorage.h"
#include "WiFiManager.h"
#include "WebServerManager.h"
#include "FileManager.h"
#include "LVGL_Driver.h"
#include "DisplayManager.h"
#include "PSRAMManager.h"
#include "TimeManager.h"
#include "I2CBusManager.h"
#include "AudioManager.h"
#include "WeatherManager.h"
#include "LocationManager.h"
#include "LLMUsageData.h"
#include "LLMMonitor.h"

// 外部变量声明
extern LVGLDriver* lvglDriver;

// LVGL驱动实例
LVGLDriver lvglDriverInstance;

// 全局实例
ConfigStorage configStorage;
WiFiManager wifiManager;
WebServerManager* webServerManager;
FileManager fileManager;
DisplayManager displayManager;
PSRAMManager psramManager;
TimeManager timeManager;
AudioManager audioManager;
WeatherManager weatherManager;
LocationManager locationManager;
LLMMonitor llmMonitor;

// 全局DisplayManager指针，供UI系统回调使用
DisplayManager* globalDisplayManager = &displayManager;

// 大模型用量数据回调函数
void llmUsageCallback(const LLMUsageData& data, void* userData) {
  DisplayManager* displayManager = (DisplayManager*)userData;
  if (displayManager) {
    displayManager->updateUsageData(data);
  }
}

void setup() {

  printf("=== ESP32S3 大模型用量监控启动 ===\n");
  printf("版本: %s\n", VERSION_STRING);
  printf("编译时间: %s %s\n", BUILD_DATE, BUILD_TIME);

  // 初始化PSRAM管理器（优先初始化）
  printf("\n初始化PSRAM管理器...\n");
  if (psramManager.init()) {
    psramManager.start();
    psramManager.setDebugMode(false); // 生产环境关闭调试模式
    printf("PSRAM管理器初始化成功\n");
    psramManager.printStatistics();
  } else {
    printf("PSRAM管理器初始化失败，继续使用内部RAM\n");
  }

  // 初始化配置存储
  printf("\n初始化系统组件...\n");
  configStorage.init();

  // 启动配置存储任务
  printf("启动配置存储任务...\n");
  if (!configStorage.startTask()) {
    printf("❌ 配置存储任务启动失败\n");
  } else {
    printf("✅ 配置存储任务启动成功\n");
  }

  // 初始化WiFi管理器
  wifiManager.setPSRAMManager(&psramManager);
  wifiManager.init(&configStorage);

  // 初始化I2C总线管理器（在所有I2C设备初始化之前）
  printf("初始化I2C总线管理器...\n");
  esp_err_t i2c_ret = I2CBus_Init();
  if (i2c_ret != ESP_OK) {
    printf("❌ I2C总线管理器初始化失败，错误码: 0x%x\n", i2c_ret);
  } else {
    printf("✅ I2C总线管理器初始化成功\n");
  }

  // 初始化LVGL驱动系统
  printf("开始LVGL驱动系统初始化...\n");
  lvglDriverInstance.init();
    // 设置全局LVGL驱动指针
    lvglDriver = &lvglDriverInstance;
  // 启动LVGL驱动任务
  printf("启动LVGL驱动任务...\n");
  lvglDriverInstance.start();

  printf("LVGL驱动系统初始化完成\n");

  // 初始化文件管理器
  fileManager.init();

  // 初始化天气管理器（需要在DisplayManager之前初始化）
  printf("开始初始化天气管理器...\n");
  if (weatherManager.init(&psramManager, &wifiManager, &configStorage)) {
    printf("✅ 天气管理器初始化成功\n");

    // 启动天气管理器
    if (weatherManager.start()) {
      printf("✅ 天气管理器启动成功\n");

      // 启用调试模式查看详细信息
      weatherManager.setDebugMode(true);

      printf("🌤️ 天气管理器配置完成\n");
      printf("💡 请在Web界面设置高德天气API密钥以启用天气功能\n");
      printf("🔗 高德开发者平台: https://console.amap.com/\n");

      // 打印天气配置信息
      weatherManager.printConfig();
    } else {
      printf("❌ 天气管理器启动失败\n");
    }
  } else {
    printf("❌ 天气管理器初始化失败\n");
  }
  printf("天气管理器初始化完成\n");

  // 初始化定位管理器
  printf("开始初始化定位管理器...\n");
  if (locationManager.init(&psramManager, &wifiManager, &configStorage)) {
    printf("✅ 定位管理器初始化成功\n");

    // 启动定位管理器
    if (locationManager.start()) {
      printf("✅ 定位管理器启动成功\n");

      // 启用调试模式查看详细信息
      locationManager.setDebugMode(true);

      printf("🌍 定位管理器配置完成\n");
      printf("💡 请在Web界面设置高德定位API密钥以启用定位功能\n");
      printf("🔗 高德开发者平台: https://console.amap.com/\n");

      // 打印定位配置信息
      locationManager.printConfig();
    } else {
      printf("❌ 定位管理器启动失败\n");
    }
  } else {
    printf("❌ 定位管理器初始化失败\n");
  }
  printf("定位管理器初始化完成\n");

  // 初始化显示管理器（现在包含WeatherManager）
  printf("开始初始化显示管理器...\n");
  displayManager.init(&lvglDriverInstance, &wifiManager, &configStorage, &psramManager, &weatherManager);

  // 启动显示管理器任务
  printf("启动显示管理器任务...\n");
  displayManager.start();

  // 设置全局DisplayManager指针，供UI系统回调使用
  globalDisplayManager = &displayManager;

  // 设置触摸活动回调，将触摸事件传递给DisplayManager
  printf("设置触摸活动回调...\n");
  lvglDriverInstance.setTouchActivityCallback([](void* userdata) {
    DisplayManager* dm = static_cast<DisplayManager*>(userdata);
    if (dm) {
      dm->notifyTouchActivity();
    }
  }, &displayManager);

  printf("显示管理器初始化完成\n");

  // 创建Web服务器管理器实例
  webServerManager = new WebServerManager(&wifiManager, &configStorage, &fileManager);

  // 初始化并启动Web服务器
  webServerManager->setPSRAMManager(&psramManager);
  webServerManager->setDisplayManager(&displayManager);
  webServerManager->setWeatherManager(&weatherManager);
  webServerManager->setLocationManager(&locationManager);
  webServerManager->setLLMMonitor(&llmMonitor);
  webServerManager->init();
  webServerManager->start();

  // 初始化大模型用量监控器
  printf("初始化大模型用量监控器...\n");
  llmMonitor.init(&psramManager, &configStorage);

  // 设置用量数据回调，将LLMMonitor的数据传递给DisplayManager
  llmMonitor.setUsageCallback(llmUsageCallback, &displayManager);
  printf("用量数据回调已设置\n");

  // 加载并应用屏幕设置配置
  printf("加载屏幕设置配置...\n");
  if (displayManager.loadScreenModeConfig()) {
    printf("屏幕设置配置加载成功\n");
  } else {
    printf("屏幕设置配置加载失败，将使用默认配置\n");
  }

  // 初始化时间管理器
  printf("开始初始化时间管理器...\n");
  timeManager.init(&psramManager, &wifiManager, &configStorage);

  // 启动时间管理器任务
  printf("启动时间管理器任务...\n");
  timeManager.start();

  // 启用时间管理器调试模式，显示详细同步信息
  timeManager.setDebugMode(true);

  printf("时间管理器初始化完成\n");

  // 显示当前状态
  vTaskDelay(pdMS_TO_TICKS(2000));
  displaySystemStatus();

  // 显示PSRAM详细使用分析
  printf("\n=== PSRAM详细使用分析 ===\n");
  if (psramManager.isPSRAMAvailable()) {
    psramManager.printMemoryMap();
  }

  // 传感器数据已集成到LVGL驱动的屏幕自动旋转功能中

  printf("=== 系统初始化完成 ===\n");
}

void loop() {
  // 根据规则，loop()不做任何任务处理
  // 所有功能通过FreeRTOS任务实现
  vTaskDelay(pdMS_TO_TICKS(1000));
}

void displaySystemStatus() {
  printf("\n=== 系统状态信息 ===\n");

  // WiFi状态
  if (wifiManager.isConnected()) {
    printf("WiFi状态: 已连接\n");
    printf("SSID: %s\n", WiFi.SSID().c_str());
    printf("IP地址: %s\n", wifiManager.getLocalIP().c_str());
    printf("信号强度: %d dBm\n", WiFi.RSSI());
  } else {
    printf("WiFi状态: AP配置模式\n");
    printf("AP SSID: ESP32S3-Config\n");
    printf("AP密码: 12345678\n");
    printf("配置IP: %s\n", wifiManager.getAPIP().c_str());
    printf("Web配置地址: http://%s\n", wifiManager.getAPIP().c_str());
  }

  // 系统信息
  printf("芯片型号: %s Rev.%d\n", ESP.getChipModel(), ESP.getChipRevision());
  printf("CPU频率: %d MHz\n", ESP.getCpuFreqMHz());
  printf("Flash大小: %d MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  printf("内部RAM: %d KB 可用, %d KB 总计\n", ESP.getFreeHeap() / 1024, ESP.getHeapSize() / 1024);

  // PSRAM信息
  if (psramManager.isPSRAMAvailable()) {
    printf("PSRAM大小: %d MB\n", psramManager.getTotalSize() / (1024 * 1024));
    printf("PSRAM可用: %d KB (%.1f%%)\n",
           psramManager.getFreeSize() / 1024,
           100.0f - psramManager.getUsagePercent());
    printf("PSRAM已用: %d KB (%.1f%%)\n",
           psramManager.getUsedSize() / 1024,
           psramManager.getUsagePercent());
    printf("PSRAM碎片率: %.1f%%\n", psramManager.getFragmentationRate());
    printf("PSRAM分配块: %u个\n", psramManager.getBlockCount());
  } else {
    printf("PSRAM: 未检测到或未启用\n");
  }

  // 时间信息
  printf("时间状态: ");
  if (timeManager.isTimeValid()) {
    printf("已同步\n");
    printf("当前时间: %s\n", timeManager.getDateTimeString().c_str());
    printf("时区: UTC%+.1f\n", timeManager.getTimezoneOffset());
    printf("同步状态: ");
    switch (timeManager.getSyncStatus()) {
      case TIME_SYNCED: printf("正常\n"); break;
      case TIME_SYNCING: printf("同步中\n"); break;
      case TIME_NOT_SYNCED: printf("未同步\n"); break;
      case TIME_SYNC_FAILED: printf("同步失败\n"); break;
    }
  } else {
    printf("未同步\n");
    printf("NTP服务器: %s\n", timeManager.getNTPConfig().primaryServer.c_str());
  }

  // 天气系统信息
  printf("天气状态: %s\n", weatherManager.getStateString().c_str());
  if (weatherManager.isWeatherDataValid()) {
    WeatherData weather = weatherManager.getCurrentWeather();
    printf("当前天气: %s\n", weather.city.c_str());
    printf("天气现象: %s\n", weather.weather.c_str());
    printf("温度: %s°C\n", weather.temperature.c_str());
    printf("湿度: %s%%\n", weather.humidity.c_str());
    printf("风向风力: %s %s级\n", weather.winddirection.c_str(), weather.windpower.c_str());
    printf("发布时间: %s\n", weather.reporttime.c_str());
  } else {
    printf("天气数据: 未获取\n");
    WeatherConfig config = weatherManager.getConfig();
    if (config.apiKey.isEmpty()) {
      printf("提示: 请设置高德API密钥\n");
    } else {
      printf("城市: %s (%s)\n", config.cityName.c_str(), config.cityCode.c_str());
    }
  }

  WeatherStatistics weatherStats = weatherManager.getStatistics();
  if (weatherStats.totalRequests > 0) {
    printf("天气统计: 总计%lu次, 成功%lu次, 失败%lu次\n",
           weatherStats.totalRequests, weatherStats.successRequests, weatherStats.failedRequests);
  }

  printf("==================\n");
}
