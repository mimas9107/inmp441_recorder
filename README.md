# INMP441 AI Voice Node (ESP32)

本專案將 ESP32 (DevKit V1) 與 **INMP441 I2S 麥克風** 結合，實作了一個具備語音活動偵測 (VAD) 與 WiFi 上傳功能的 AI 語音節點。

## 分支說明 (Branch Information)

*   **feature01 (當前穩定分支)**: 
    *   採用輕量級 **RMS-based VAD** 偵測。
    *   具備 10 秒開機自動校正 (Auto-Calibration) 以適應環境底噪。
    *   偵測到語音後自動錄製 3 秒 (RAM Buffer)。
    *   透過 WiFi 將 WAV 檔 HTTP POST 到指定的 PC Server。
    *   適合記憶體有限的 ESP32-WROOM (無 PSRAM) 環境。
*   **main (主分支)**: 
    *   嘗試整合官方 `esp-sr` (WakeNet 喚醒詞) 的版本。
    *   **注意**: 此版本在標準 ESP32-WROOM 上會因為記憶體不足 (Memory Exhausted) 而崩潰。若要運行此分支，建議使用具備 PSRAM 的 ESP32 模組 (如 WROVER 或 S3)。

---

## 系統架構

1.  **開機校正**: 前 10 秒讀取環境音，計算平均 RMS 作為底噪基準。
2.  **動態監聽**: 當 `即時 RMS > (底噪 + Margin)` 時觸發錄音。
3.  **錄音儲存**: 音訊直接存入內部 RAM (約 96KB)，確保無卡頓與斷音。
4.  **WiFi 上傳**: 將 WAV 資料透過 HTTP POST 傳送至區網內的 Flask Server。

---

## 硬體連接

| INMP441 Pin | ESP32 GPIO | 功能 |
|------------|-----------|------|
| **VDD**    | 3.3V      | 電源 |
| **GND**    | GND       | 接地 |
| **SCK**    | GPIO 32   | BCLK |
| **WS**     | GPIO 25   | WS (Word Select) |
| **SD**     | GPIO 33   | DIN (Data In) |
| **L/R**    | GND       | 設定為左聲道 |

---

## 軟體設定 (ESP-IDF)

1.  **設定參數**:
    ```bash
    idf.py menuconfig
    ```
    進入 **Inmp441 Recorder Configuration** 設定：
    *   **WiFi SSID / Password**: 連線資訊。
    *   **Server URL**: 例如 `http://192.168.1.100:5000/upload`。
    *   **VAD RMS Threshold**: 觸發靈敏度 (Margin)，預設 500。
2.  **編譯與燒錄**:
    ```bash
    idf.py build flash monitor
    ```

---

## PC 端接收伺服器 (Server Side)

位於 `server/` 目錄下：
1.  **安裝依賴**: `pip install flask requests openai-whisper`
2.  **啟動服務**: `python3 server/server.py`
3.  伺服器會將收到的錄音存放在 `server/uploads/` 並嘗試播放。

---

## 技術細節

*   **Sample Rate**: 16000 Hz
*   **Bit Depth**: 16-bit PCM (從 24-bit 數據位移 `>> 11` 轉換)
*   **VAD 策略**: 動態能量門檻 (Noise Floor + Margin)
*   **傳輸協議**: HTTP POST (application/octet-stream)
