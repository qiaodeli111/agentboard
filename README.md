# AgentBoard

把微雪 **ESP32-S3-RLCD-4.2**（4.2" 全反射屏，ST7305，300×400 单色）变成桌面 Agent 状态板：一块屏看本机所有 AI agent 的会话与额度。

**连接方式：USB 线直连推送**（不需要 WiFi，不需要加入任何网络——适合办公室场景）。Mac 上常驻服务每 10 秒把数据快照写到板子的 USB 串口（`/dev/cu.usbmodem*`），固件收到后渲染上屏；固件也保留 WiFi 轮询模式可切换。

**屏幕有三个页面**（板侧 KEY 按键 GPIO18：**短按翻页，长按清除通知**）：

1. **通知页（首页）**——只显示"跑完等你输入"的提醒（反白醒目块：agent、项目、标题、多久前跑完）。agent 安静超过 60 秒即视为一轮跑完，产生通知；长按 KEY 或浏览器点"全部清除"即消失
2. **会话页**——全部 agent 会话列表 + 额度区（ChatGPT 5h/每周条、CLAUDE/PI 今日 token）。状态语义：● = 60 秒内有文件写入（真在跑）；◐ = 等输入/闲；○ = 1 小时以上没动
3. **模型页**——今日各 agent × 模型的 token 用量排行（含比例条）

```
┌─ 页1: 通知 ────────────────────────────┐
│ AgentBoard                主机名 14:30 │
│ ██████████████████████████████████████ │
│ ██ CLAUDE  phizpay   跑完等你    3m ██ │
│ ██ 帮我更新看板：1. 首页这些总览…    ██ │
│ ██████████████████████████████████████ │
│ [1 通知]  2 会话   3 模型        c1 x3 │
└────────────────────────────────────────┘
```

## 架构

```
┌─ Mac ────────────────────────────┐   USB CDC 串口    ┌─ ESP32-S3 ─────────────┐
│ agentboard.py  (launchd 常驻)    │ ───────────────> │ 收 JSON 行 → 解析渲染   │
│  读 ~/.claude/projects (会话)    │  /dev/cu.usbmodem*│ U8g2 → ST7305 整屏刷新  │
│  读 ~/.codex/sessions (会话+额度) │   每 10s 一行    │ 内容没变就不重绘         │
│  读 ~/.pi/agent/sessions (会话)  │                  └────────────────────────┘
│  读 ~/.dsh/sessions (会话+余额)  │  ← DeepSeek Harness，余额 5min 一查
│  ps+lsof 识别活进程 → 项目映射    │
│  GET /api/status  GET /(浏览器)  │   ← 同一份数据，浏览器预览用
└──────────────────────────────────┘
```

- **会话状态**：`running` = 会话文件 60 秒内有写入（agent 真在干活）；`idle` = 安静 1 小时内（等输入/闲）；`done` = 更久（24h 窗口内）。**通知** = 状态从 running 翻转为 idle（一轮干完，等你）
- **通知确认**：屏上长按 KEY（发 `ACK` 回 Mac）或浏览器"全部清除"按钮（`POST /api/ack`）；通知持久化在 `server/notifs.json`，服务重启不丢
- **ChatGPT 额度**：从 Codex 会话文件里解析的 ChatGPT 账号官方额度窗口（5 小时 + 每周，含重置时间）。ChatGPT 网页版普通聊天额度无官方 API，不覆盖
- **DSH（DeepSeek Harness）**：会话（zstd jsonl）+ 账户余额（platform.deepseek.com 钱包接口，x-dsh-auth-token 认证，5 分钟刷新）+ 分模型今日用量
- **Claude 统计**：本地解析 transcript 的今日 token / 费用估算（外部 API 模型如 GLM 没有价格表，费用为 0 只计数）
- **ChatGPT 桌面版会话**：Codex Desktop originator 的线程，自动归类到 `chatgpt` 名下
- **数据不出机器**：全部本地解析，串口直连，不经过任何网络

## 文件

| 路径 | 说明 |
|---|---|
| `server/agentboard.py` | 采集服务（纯 Python 标准库，无依赖） |
| `server/config.json` | 可选配置（端口、阈值、窗口） |
| `firmware/agentboard/` | Arduino 固件（`USE_WIFI 0`=USB 推送默认，`1`=WiFi 轮询） |
| `launchd/com.qiaodeli.agentboard.plist` | 开机自启 |

> 克隆后先执行：`cp firmware/agentboard/secrets.h.example firmware/agentboard/secrets.h`（WiFi 凭据不入库，默认 USB 模式可不填 WiFi）

## 日常使用

```bash
# 服务状态 / 日志
launchctl list | grep agentboard
tail ~/Library/Logs/agentboard.log

# 手动重启服务
launchctl kickstart -k gui/$(id -u)/com.qiaodeli.agentboard

# 浏览器预览（跟屏上一样的数据）
open http://localhost:8899

# 改固件后重新编译烧录
cd ~/agentboard/firmware/agentboard
arduino-cli compile -b "esp32:esp32:esp32s3:CDCOnBoot=cdc" .
arduino-cli upload -b "esp32:esp32:esp32s3:CDCOnBoot=cdc" -p /dev/cu.usbmodem101 .
```

## 注意

- **烧录时**：服务占用串口只是毫秒级写一帧，通常不挡烧录；万一 `upload` 报端口忙，先 `launchctl kickstart -k gui/$(id -u)/com.qiaodeli.agentboard` 重启一下服务再烧
- **拔线换电脑**：板子重新插上后推送自动恢复，无需操作
- **屏幕**：全反射屏靠环境光，越亮越清楚，无背光；刷新无残影
- **数据仅在本机**：`/api/status` 无鉴权（含会话标题），不要把端口暴露到公网
