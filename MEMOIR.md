---
name:          "MEMOIR.md"
description:   "INMP441 Dataset Collector Node - design decisions and lessons learned"
created_date:  "2026/08/22 00:00:00"
modified_date: "2026/08/22 00:00:00"
project_version: "0.1.1"
document_version: "1.0.1"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash', 'opencode/ox-alpha']
---

# MEMOIR — Dataset Collector Node (feature01a)

## Design Decisions

- **COLLECTOR vs RECORDER 架構分歧**: 本分支移除 VAD 自動觸發，改為「伺服器遙控 + 看門狗」的受管理收集模式，專供訓練資料集蒐集。兩分支 main.c 結構已大幅分歧（本分支有 web server/registration/watchdog；feature01 有校準/VAD），跨分支搬運功能須手動移植、不可 cherry-pick。
- **ESP-IDF v6.0.2 移植 (0.1.0)**: 與 feature01 相同模式——`driver/i2s.h` → `esp_driver_i2s` 標準模式。移植時的差異點：
  - v6 **沒有**獨立的 `i2s_channel_set_pin`；pin 設定由 `i2s_channel_init_std_mode` 內含的 `gpio_cfg` 一併套用。
  - 舊 `.use_apll = true` 未保留，採用預設時脈源（與 feature01 移植一致，實測音質正常）。
  - `i2s_read(portMAX_DELAY)` 改為 1000ms timeout + 空讀保護：使 Stop 指令與 server 失聯能在單一讀取週期內被感知。
  - DMA 清空以 `i2s_channel_disable`（drain）+ `enable`（重啟）取代已移除的 `i2s_zero_dma_buffer`。
  - CMakeLists `REQUIRES driver` 拆分為 `esp_driver_i2s esp_driver_gpio`。

## Lessons Learned

- 分支長期停在舊框架版本會累積「無法編譯」的技術債；框架大版本遷移後應同步移植所有活躍分支。
- 版本控制衛生：歷史上曾將錄音輸出 .wav commit 進 repo（本分支已移除）；建置產物（如 `.cache/clangd/index/*.idx`）追蹤後會反覆阻擋分支切換，宜加入 .gitignore。
