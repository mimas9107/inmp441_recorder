---
name:          "README.md"
description:   "INMP441 Dataset Collector Node (ESP32) - COLLECTOR variant overview"
created_date:  "2026/02/09 00:00:00"
modified_date: "2026/08/22 00:00:00"
project_version: "0.1.1"
document_version: "1.0.1"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash', 'opencode/ox-alpha']
---

# INMP441 Dataset Collector Node (ESP32) — feature01a

本分支（COLLECTOR 變體）將 ESP32 (DevKit V1) + **INMP441 I2S 麥克風** 變成一台**受伺服器遙控的資料收集站**：移除 VAD 自動觸發，改由 Flask Server 遠端下達開始/停止指令，連續循環錄音上傳，專門用於蒐集聲音分類訓練資料集（如 Edge Impulse 關鍵詞模型）。

## 分支說明

| 分支 | 定位 | 觸發方式 |
|------|------|----------|
| **feature01**（主線/預設分支） | 語音觸發節點 | RMS VAD 自動觸發 |
| **feature01a**（本分支） | 資料收集工具 | 伺服器遙控 Start/Stop |
| **waitfornewhardware** | esp-sr 喚醒詞實驗封存 | WakeNet（需 S/C 系列硬體） |

## 系統架構

1.  **裝置註冊**: 開機後向 Server `POST /register` 回報 IP，每 5 秒重試直到成功。
2.  **連線看門狗**: 每 5 秒 GET Server 根路徑；斷線時自動中止收集並停止上傳。
3.  **遙控通道**: ESP32 自身運行 HTTP Server（`/control?cmd=start|stop`），接收 Server 的轉發指令。
4.  **循環錄音**: 收集期間持續擷取 3 秒 WAV（RAM buffer）並 HTTP POST 上傳，間隔 0.5 秒。
5.  **LED 指示**（GPIO2 板載燈）: 恆亮＝收集中；熄滅＝待機或傳輸中。

## 硬體連接

| INMP441 Pin | ESP32 GPIO | 功能 |
|------------|-----------|------|
| **VDD**    | 3.3V      | 電源 |
| **GND**    | GND       | 接地 |
| **SCK**    | GPIO 32   | BCLK |
| **WS**     | GPIO 25   | WS (Word Select) |
| **SD**     | GPIO 33   | DIN (Data In) |
| **L/R**    | GND       | 左聲道 |

## 軟體設定（ESP-IDF v6.0.2）

需先安裝 ESP-IDF **v6.0.2** 並執行 `export.sh`。

```bash
idf.py menuconfig   # Inmp441 Recorder Configuration: WiFi SSID/Password、Server URL、Record Seconds
idf.py build flash monitor
```

## PC 端控制伺服器（server/）

```bash
cd server
uv sync                 # 或使用既有 .venv
uv run server.py        # 監聽 0.0.0.0:5000
```

主要端點：

| 端點 | 功能 |
|------|------|
| `/` | 網頁儀表板（狀態、標籤、控制按鈕） |
| `/api/status` | 即時狀態 JSON |
| `/register` | 裝置註冊（POST JSON） |
| `/esp_control?cmd=start\|stop` | 轉發控制指令至裝置 |
| `/set_label?label=<名稱>` | 切換目前標籤（決定存檔命名） |
| `/upload` | 收檔端點，存為 `{label}.{count}.wav` |
| `/reset_counts` | 重置計數器 |
| `/file/<name>` | 回播已上傳檔案 |

典型收集流程：`set_label` 設標籤 → `esp_control?cmd=start` 連續收集 → `cmd=stop` 停止 → 檔案落於 `server/uploads/{label}.{n}.wav`。

## 技術細節

*   **框架**: ESP-IDF v6.0.2（I2S 使用新 `esp_driver_i2s` 標準模式 API）
*   **Sample Rate**: 16000 Hz
*   **Bit Depth**: 16-bit PCM（自 32-bit 樣本位移 `>> 11` 轉換）
*   **DMA 配置**: 4 × 256 frames
*   **傳輸協議**: HTTP POST（audio/wav）
