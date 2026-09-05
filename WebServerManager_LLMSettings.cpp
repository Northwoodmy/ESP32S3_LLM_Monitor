/*
 * WebServerManager_LLMSettings.cpp - 大模型用量设置页面实现
 * ESP32S3_LLM_Monitor项目 - 大模型平台(API Key)配置模块
 *
 * 页面功能:
 * - 4个平台槽位配置(禁用/DeepSeek/Kimi Code)
 * - API Key 密码框配置(掩码显示, 留空保持不变, 可勾选清除)
 * - DeepSeek 估算单价配置(用于换算 tokens)
 * - 全局轮询间隔配置(30-600秒)
 * - 每槽位连接测试
 * - 实时用量状态预览
 */

#include "WebServerManager.h"

String WebServerManager::getLLMSettingsHTML() {
    String html = "<!DOCTYPE html>\n";
    html += "<html lang=\"zh-CN\">\n";
    html += "<head>\n";
    html += "    <meta charset=\"UTF-8\">\n";
    html += "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    html += "    <title>大模型设置 - ESP32S3 LLM Monitor</title>\n";
    html += "    <style>\n";
    html += getLLMSettingsCSS();
    html += "    </style>\n";
    html += "</head>\n";
    html += "<body>\n";
    html += "    <div class=\"container\">\n";
    html += "        <header class=\"header\">\n";
    html += "            <h1>大模型用量设置</h1>\n";
    html += "            <div class=\"subtitle\">配置 DeepSeek / Kimi Code 平台 API Key</div>\n";
    html += "        </header>\n";
    html += "\n";
    html += "        <button onclick=\"window.location.href='/'\" class=\"back-btn\">返回首页</button>\n";
    html += "\n";
    // === 实时状态预览 ===
    html += "        <div class=\"card\">\n";
    html += "            <h2>当前状态</h2>\n";
    html += "            <div id=\"statusArea\" class=\"status-area\">\n";
    html += "                <span class=\"muted\">加载中...</span>\n";
    html += "            </div>\n";
    html += "        </div>\n";
    html += "\n";
    // === 全局设置 ===
    html += "        <div class=\"card\">\n";
    html += "            <h2>全局设置</h2>\n";
    html += "            <div class=\"form-row\">\n";
    html += "                <label>轮询间隔(秒)</label>\n";
    html += "                <input type=\"number\" id=\"pollSec\" min=\"30\" max=\"600\" step=\"10\" value=\"60\">\n";
    html += "            </div>\n";
    html += "            <div class=\"hint\">范围 30-600 秒, 建议 60 秒以上, 避免频繁请求 API</div>\n";
    html += "        </div>\n";
    html += "\n";
    // === 4个槽位(由 JS 生成) ===
    html += "        <div id=\"slotsArea\"></div>\n";
    html += "\n";
    // === 保存按钮 ===
    html += "        <div class=\"card\">\n";
    html += "            <button onclick=\"saveConfig()\" class=\"save-btn\" id=\"saveBtn\">保存全部配置</button>\n";
    html += "            <div class=\"hint\" style=\"margin-top:10px;\">保存后立即生效, 无需重启</div>\n";
    html += "        </div>\n";
    html += "    </div>\n";
    html += "\n";
    html += "    <div id=\"toast\" class=\"toast hidden\"><span id=\"toastMsg\"></span></div>\n";
    html += "\n";
    html += "    <script>\n";
    html += getLLMSettingsJavaScript();
    html += "    </script>\n";
    html += "</body>\n";
    html += "</html>\n";
    return html;
}

String WebServerManager::getLLMSettingsCSS() {
    String css = "";
    css += "* { margin: 0; padding: 0; box-sizing: border-box; }\n";
    css += "body { font-family: -apple-system, 'PingFang SC', 'Microsoft YaHei', sans-serif; background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%); color: #e0e0e0; min-height: 100vh; padding: 16px; }\n";
    css += ".container { max-width: 720px; margin: 0 auto; }\n";
    css += ".header { text-align: center; margin-bottom: 16px; }\n";
    css += ".header h1 { font-size: 24px; color: #4fc3f7; }\n";
    css += ".subtitle { color: #888; font-size: 13px; margin-top: 4px; }\n";
    css += ".card { background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.1); border-radius: 12px; padding: 16px; margin-bottom: 14px; }\n";
    css += ".card h2 { font-size: 16px; color: #4fc3f7; margin-bottom: 12px; }\n";
    css += ".slot-head { display: flex; align-items: center; justify-content: space-between; }\n";
    css += ".slot-badge { font-size: 12px; padding: 2px 10px; border-radius: 10px; background: #333; color: #999; }\n";
    css += ".slot-badge.on { background: #1b5e20; color: #a5d6a7; }\n";
    css += ".form-row { display: flex; align-items: center; margin-bottom: 10px; gap: 10px; }\n";
    css += ".form-row label { width: 110px; flex-shrink: 0; font-size: 14px; color: #bbb; }\n";
    css += ".form-row input, .form-row select { flex: 1; padding: 9px 10px; border-radius: 8px; border: 1px solid rgba(255,255,255,0.2); background: rgba(0,0,0,0.3); color: #fff; font-size: 14px; }\n";
    css += ".form-row input:focus, .form-row select:focus { outline: none; border-color: #4fc3f7; }\n";
    css += ".check-row { display: flex; align-items: center; gap: 8px; font-size: 13px; color: #bbb; margin: 6px 0 6px 120px; }\n";
    css += ".hint { font-size: 12px; color: #777; margin: 4px 0 0 120px; line-height: 1.5; }\n";
    css += ".hint a { color: #4fc3f7; }\n";
    css += ".back-btn, .save-btn, .test-btn { border: none; border-radius: 8px; cursor: pointer; font-size: 14px; padding: 10px 18px; }\n";
    css += ".back-btn { background: rgba(255,255,255,0.1); color: #ccc; margin-bottom: 14px; }\n";
    css += ".save-btn { width: 100%; background: #1976d2; color: #fff; font-size: 16px; padding: 13px; }\n";
    css += ".save-btn:hover { background: #1e88e5; }\n";
    css += ".save-btn:disabled { background: #555; cursor: not-allowed; }\n";
    css += ".test-btn { background: #37474f; color: #b0bec5; padding: 8px 14px; font-size: 13px; }\n";
    css += ".test-btn:hover { background: #455a64; }\n";
    css += ".test-btn:disabled { opacity: 0.5; cursor: wait; }\n";
    css += ".test-result { font-size: 13px; margin-top: 8px; margin-left: 120px; word-break: break-all; }\n";
    css += ".test-result.ok { color: #81c784; }\n";
    css += ".test-result.err { color: #e57373; }\n";
    css += ".status-area { display: flex; flex-direction: column; gap: 8px; }\n";
    css += ".status-item { background: rgba(0,0,0,0.25); border-radius: 8px; padding: 10px 12px; font-size: 13px; line-height: 1.7; }\n";
    css += ".status-item .pname { color: #4fc3f7; font-weight: bold; margin-right: 8px; }\n";
    css += ".status-item .ok { color: #81c784; }\n";
    css += ".status-item .err { color: #e57373; }\n";
    css += ".muted { color: #777; font-size: 13px; }\n";
    css += ".hidden-row { display: none !important; }\n";
    css += ".toast { position: fixed; bottom: 30px; left: 50%; transform: translateX(-50%); background: #323232; color: #fff; padding: 12px 24px; border-radius: 8px; font-size: 14px; z-index: 999; box-shadow: 0 4px 12px rgba(0,0,0,0.4); }\n";
    css += ".toast.hidden { display: none; }\n";
    return css;
}

String WebServerManager::getLLMSettingsJavaScript() {
    String js = "";
    // === 槽位卡片生成(4个) ===
    js += "const SLOT_COUNT = 4;\n";
    js += "function buildSlots() {\n";
    js += "  let area = document.getElementById('slotsArea');\n";
    js += "  let out = '';\n";
    js += "  for (let i = 0; i < SLOT_COUNT; i++) {\n";
    js += "    out += `<div class=\"card\" id=\"slotCard${i}\">`\n";
    js += "      + `<div class=\"slot-head\"><h2>槽位 ${i+1}</h2><span class=\"slot-badge\" id=\"slotBadge${i}\">禁用</span></div>`\n";
    js += "      + `<div class=\"form-row\"><label>平台</label><select id=\"slotType${i}\" onchange=\"onTypeChange(${i})\">`\n";
    js += "      + `<option value=\"0\">禁用</option><option value=\"1\">DeepSeek</option><option value=\"2\">Kimi Code</option>`\n";
    js += "      + `</select></div>`\n";
    js += "      + `<div class=\"form-row\" id=\"keyRow${i}\"><label>API Key</label><input type=\"password\" id=\"slotKey${i}\" placeholder=\"未配置\" autocomplete=\"off\"></div>`\n";
    js += "      + `<div class=\"check-row\" id=\"clearRow${i}\"><input type=\"checkbox\" id=\"slotClear${i}\"><label for=\"slotClear${i}\" style=\"width:auto;\">清除已保存的密钥</label></div>`\n";
    js += "      + `<div class=\"hint\" id=\"keyHint${i}\"></div>`\n";
    js += "      + `<div class=\"form-row\" id=\"priceRow${i}\"><label>估算单价</label><input type=\"number\" id=\"slotPrice${i}\" min=\"0.1\" step=\"0.1\" value=\"2.0\"></div>`\n";
    js += "      + `<div class=\"hint\" id=\"priceHint${i}\">单位: 元/百万tokens, 用于把余额消耗换算成估算 token 数(参考官网模型价格)</div>`\n";
    js += "      + `<div style=\"margin-top:10px; margin-left:120px;\"><button class=\"test-btn\" id=\"testBtn${i}\" onclick=\"testSlot(${i})\">测试连接</button></div>`\n";
    js += "      + `<div class=\"test-result\" id=\"testResult${i}\"></div>`\n";
    js += "      + `</div>`;\n";
    js += "  }\n";
    js += "  area.innerHTML = out;\n";
    js += "}\n";
    // === 平台类型切换 ===
    js += "function onTypeChange(i) {\n";
    js += "  const t = parseInt(document.getElementById('slotType' + i).value);\n";
    js += "  const badge = document.getElementById('slotBadge' + i);\n";
    js += "  const show = (t !== 0);\n";
    js += "  document.getElementById('keyRow' + i).classList.toggle('hidden-row', !show);\n";
    js += "  document.getElementById('clearRow' + i).classList.toggle('hidden-row', !show);\n";
    js += "  document.getElementById('priceRow' + i).classList.toggle('hidden-row', t !== 1);\n";
    js += "  document.getElementById('priceHint' + i).classList.toggle('hidden-row', t !== 1);\n";
    js += "  document.getElementById('keyHint' + i).classList.toggle('hidden-row', !show);\n";
    js += "  const hint = document.getElementById('keyHint' + i);\n";
    js += "  if (t === 1) hint.innerHTML = 'DeepSeek 平台密钥, 格式 sk-xxx, 获取: <a href=\"https://platform.deepseek.com/api_keys\" target=\"_blank\">platform.deepseek.com</a>';\n";
    js += "  else if (t === 2) hint.innerHTML = 'Kimi Code 密钥, 格式 sk-kimi-xxx(注意: 不是开放平台的 sk-xxx), 在 Kimi Code 控制台获取';\n";
    js += "  else hint.textContent = '';\n";
    js += "  badge.textContent = (t === 0 ? '禁用' : (t === 1 ? 'DeepSeek' : 'Kimi Code'));\n";
    js += "  badge.classList.toggle('on', show);\n";
    js += "}\n";
    // === 提示框 ===
    js += "function toast(msg) {\n";
    js += "  const t = document.getElementById('toast');\n";
    js += "  document.getElementById('toastMsg').textContent = msg;\n";
    js += "  t.classList.remove('hidden');\n";
    js += "  setTimeout(() => t.classList.add('hidden'), 3000);\n";
    js += "}\n";
    // === 加载配置 ===
    js += "async function loadConfig() {\n";
    js += "  try {\n";
    js += "    const resp = await fetch('/api/llm/config');\n";
    js += "    const data = await resp.json();\n";
    js += "    if (!data.success) { toast('配置加载失败'); return; }\n";
    js += "    document.getElementById('pollSec').value = data.pollSec;\n";
    js += "    for (let i = 0; i < SLOT_COUNT; i++) {\n";
    js += "      const s = data.slots[i];\n";
    js += "      document.getElementById('slotType' + i).value = s.type;\n";
    js += "      if (s.hasKey) document.getElementById('slotKey' + i).placeholder = '已配置: ' + s.keyMasked;\n";
    js += "      document.getElementById('slotPrice' + i).value = s.unitPrice;\n";
    js += "      onTypeChange(i);\n";
    js += "    }\n";
    js += "  } catch (e) { toast('配置加载失败: ' + e); }\n";
    js += "}\n";
    // === 保存配置 ===
    js += "async function saveConfig() {\n";
    js += "  const btn = document.getElementById('saveBtn');\n";
    js += "  btn.disabled = true;\n";
    js += "  const params = new URLSearchParams();\n";
    js += "  params.append('pollSec', document.getElementById('pollSec').value);\n";
    js += "  for (let i = 0; i < SLOT_COUNT; i++) {\n";
    js += "    params.append('slot' + i + '_type', document.getElementById('slotType' + i).value);\n";
    js += "    if (document.getElementById('slotClear' + i).checked) {\n";
    js += "      params.append('slot' + i + '_key', 'CLEAR');\n";
    js += "    } else {\n";
    js += "      const k = document.getElementById('slotKey' + i).value.trim();\n";
    js += "      if (k.length > 0) params.append('slot' + i + '_key', k);\n";
    js += "    }\n";
    js += "    params.append('slot' + i + '_price', document.getElementById('slotPrice' + i).value);\n";
    js += "  }\n";
    js += "  try {\n";
    js += "    const resp = await fetch('/api/llm/config', { method: 'POST', body: params });\n";
    js += "    const data = await resp.json();\n";
    js += "    toast(data.message || (data.success ? '已保存' : '保存失败'));\n";
    js += "    if (data.success) setTimeout(loadStatus, 1500);\n";
    js += "  } catch (e) { toast('保存失败: ' + e); }\n";
    js += "  btn.disabled = false;\n";
    js += "}\n";
    // === 测试连接 ===
    js += "async function testSlot(i) {\n";
    js += "  const btn = document.getElementById('testBtn' + i);\n";
    js += "  const result = document.getElementById('testResult' + i);\n";
    js += "  const t = parseInt(document.getElementById('slotType' + i).value);\n";
    js += "  if (t === 0) { result.className = 'test-result err'; result.textContent = '请先选择平台并保存配置'; return; }\n";
    js += "  btn.disabled = true;\n";
    js += "  result.className = 'test-result';\n";
    js += "  result.textContent = '测试中...';\n";
    js += "  try {\n";
    js += "    const params = new URLSearchParams();\n";
    js += "    params.append('slot', i);\n";
    js += "    const resp = await fetch('/api/llm/test', { method: 'POST', body: params });\n";
    js += "    const data = await resp.json();\n";
    js += "    result.className = 'test-result ' + (data.success ? 'ok' : 'err');\n";
    js += "    result.textContent = data.message;\n";
    js += "  } catch (e) {\n";
    js += "    result.className = 'test-result err';\n";
    js += "    result.textContent = '测试请求失败: ' + e;\n";
    js += "  }\n";
    js += "  btn.disabled = false;\n";
    js += "}\n";
    // === 实时状态 ===
    js += "async function loadStatus() {\n";
    js += "  const area = document.getElementById('statusArea');\n";
    js += "  try {\n";
    js += "    const resp = await fetch('/api/llm/status');\n";
    js += "    const data = await resp.json();\n";
    js += "    if (!data.success) { area.innerHTML = '<span class=\"muted\">状态不可用</span>'; return; }\n";
    js += "    let out = '';\n";
    js += "    let any = false;\n";
    js += "    for (const p of data.providers) {\n";
    js += "      if (!p.enabled) continue;\n";
    js += "      any = true;\n";
    js += "      out += '<div class=\"status-item\"><span class=\"pname\">' + (p.name || '平台') + '</span>';\n";
    js += "      out += p.valid ? '<span class=\"ok\">[' + p.state + ']</span> ' : '<span class=\"err\">[' + p.state + ']</span> ';\n";
    js += "      if (p.type === 1 && p.valid) {\n";
    js += "        out += '余额 ' + p.balance.toFixed(2) + ' 元, 今日消耗 ' + p.costToday.toFixed(4) + ' 元, 估算tokens ' + p.estTokensToday;\n";
    js += "      } else if (p.type === 2 && p.valid) {\n";
    js += "        out += '7天 ' + p.weeklyUsed + '/' + p.weeklyLimit + ', 5小时 ' + p.win5hUsed + '/' + p.win5hLimit + ', 7d重置 ' + p.weeklyReset + ', 5h重置 ' + p.win5hReset;\n";
    js += "      }\n";
    js += "      out += '<br><span class=\"muted\">更新于 ' + (p.updated || '--') + '</span></div>';\n";
    js += "    }\n";
    js += "    if (!any) out = '<span class=\"muted\">尚未配置任何平台, 请在下方槽位中配置 API Key</span>';\n";
    js += "    area.innerHTML = out;\n";
    js += "  } catch (e) { area.innerHTML = '<span class=\"muted\">状态加载失败</span>'; }\n";
    js += "}\n";
    // === 初始化 ===
    js += "buildSlots();\n";
    js += "for (let i = 0; i < SLOT_COUNT; i++) onTypeChange(i);\n";
    js += "loadConfig();\n";
    js += "loadStatus();\n";
    js += "setInterval(loadStatus, 15000);\n";
    return js;
}
