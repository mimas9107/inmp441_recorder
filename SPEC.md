---
name:          "SPEC.md"
description:   "INMP441 Dataset Collector Node - system specification"
created_date:  "2026/08/22 00:00:00"
modified_date: "2026/08/22 00:00:00"
project_version: "0.1.1"
document_version: "1.0.1"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash', 'opencode/ox-alpha']
---

# SPEC — Dataset Collector Node (feature01a)

## Framework
- **ESP-IDF**: v6.0.2（tag）
- **I2S Driver**: `esp_driver_i2s` standard mode

## Firmware 行為流程

```
app_main
 ├─ GPIO2 初始化（LED 輸出，預設熄滅）
 ├─ init_wifi()            → STA 模式，斷線自動重連
 ├─ init_i2s()             → RX channel、Philips 32-bit mono @16kHz、DMA 4×256
 ├─ start_webserver()      → 裝置端 /control?cmd=start|stop
 ├─ watchdog_task          → 每 5s GET Server 根路徑；失聯→server_alive=false 並中止收集
 └─ collection_task        → register 迴圈(5s 重試) → 等待 (is_collecting && server_alive)
                              → 擷取 RECORD_SECONDS 秒 WAV 至 RAM → 上傳 → 間隔 500ms 循環
```

- 收集中途收到 stop 或 server 失聯：丟棄緩衝並以 channel disable/enable 清空 DMA。
- 上傳失敗：自動停止收集（`is_collecting = false`）。

## Audio Format
- **Sample Rate**: 16000 Hz
- **Bit Depth**: 16-bit PCM（32-bit 樣本 `>> 11` 轉換）
- **Channels**: Mono（L/R 接地）

## Indicator LED
- **Pin**: GPIO2（DevKit V1 板載）
- **States**: 恆亮＝收集中；熄滅＝待機或傳輸中

## 網路端點

### 裝置端（ESP32 自身 HTTP Server）
| 端點 | 說明 |
|------|------|
| `GET /control?cmd=start` | 需 server_alive 才啟動收集 |
| `GET /control?cmd=stop` | 停止收集 |

### Server 端（Flask, 0.0.0.0:5000）
| 端點 | 方法 | 說明 |
|------|------|------|
| `/` | GET | 網頁儀表板 |
| `/api/status` | GET | 狀態 JSON |
| `/register` | POST | 裝置註冊 `{ip, id}` |
| `/esp_control?cmd=` | GET | 轉發指令至已註冊裝置 |
| `/set_label?label=` | GET | 切換標籤 |
| `/upload` | POST | 收檔 → `{label}.{count}.wav` |
| `/reset_counts` | POST | 計數歸零 |
| `/file/<name>` | GET | 回播檔案 |

## Kconfig 參數
- `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD`
- `CONFIG_SERVER_URL`（例：`http://192.168.1.103:5000/upload`）
- `CONFIG_I2S_BCK_GPIO`=32 / `CONFIG_I2S_WS_GPIO`=25 / `CONFIG_I2S_DIN_GPIO`=33
- `CONFIG_RECORD_SECONDS`=3
