# DSView MCP

把 DSView 的硬體採集能力（透過 `libsigrok4DSL`）包成 MCP server，讓 Claude Code 之類的 LLM 可以**列出裝置 → 設定參數 → 即時採集 → 分析信號 → 解碼協議**。

> Wraps DSView's hardware capture (`libsigrok4DSL`) as an MCP server so that
> LLMs (Claude Code, etc.) can **list devices → configure → capture → analyze
> signals → decode protocols**.

支援硬體 / Supported hardware：

- DSLogic Plus（pid 0x0020 / 0x0030 / 0x0034 已驗證）
- DSLogic 全系列 / DSCope 系列（同 driver，未個別驗證）
- Demo Device（虛擬，可在沒接硬體時開發）

---

## 中文說明

### 結構

```
mcp/
├── CMakeLists.txt          # 編 dsview_helper（不必編整個 DSView）
├── dsview_helper/main.c    # 薄薄一層 CLI，包 libsigrok4DSL
├── dsview_mcp/server.py    # FastMCP server，暴露 tools 給 LLM
├── pyproject.toml
└── README.md
```

### 提供的 MCP Tools

| Tool | 用途 |
|---|---|
| `list_devices()` | 列出已接上的 DSLogic / DSCope / Demo |
| `device_info(index)` | 該裝置的通道、samplerate、depth |
| `capture(...)` | **二段確認**：先回傳設定 preview，加 `confirm=True` 才實際採集 |
| `list_captures(limit)` | 看最近的 capture |
| `analyze(capture_id, channel)` | edges、duty、估頻率、pulse 寬度統計 |
| `read_window(capture_id, channel, start, length)` | 取一小段 0/1 bit 字串給 LLM 看 |
| `list_decoders(filter)` | 列出可用協議解碼器（150+） |
| `decode(capture_id, protocol, channel_map, options)` | 跑解碼器，回傳 annotations |

採集結果存在 `~/.dsview/captures/cap-<id>.{bin,json}`，bin 採用 DSLogic 原生 atomic-block layout（每 64 sample × 啟用通道，per-channel 8 bytes LSB-first packed），由 server.py 自動解碼。

### `capture(...)` 完整參數

對應 DSView GUI「設備選項」面板：

| 參數 | 類型 | 對應 GUI / 說明 |
|---|---|---|
| `samplerate` | int | 取樣頻率（Hz）。1 MS/s = 1_000_000 |
| `depth` | int | 總取樣數 |
| `channels` | list[int] | 啟用通道索引；None = 維持目前設定 |
| `index` | int | 裝置索引（`list_devices()`），-1 = 最近接上的 |
| `timeout_sec` | int | 硬性逾時（秒）|
| `vth` | float | **閾值電壓**（V）。1.8V 系統建議 0.9；3.3V/5V 留白用預設 |
| `operation_mode` | str/int | **運行模式**：`"buffer"` / `"stream"` / `"internal_test"` / `"external_test"` / `"loopback_test"` |
| `buffer_option` | str/int | **停止選項**：`"stop"`（立即停止）/ `"upload"`（上傳已採集的數據）|
| `filter` | str/int | **濾波器**：`"none"` 或 `"1t"`（1 個取樣週期 glitch filter）|
| `channel_mode` | int | **通道模式**：DSLogic Plus 0=16ch@20MHz, 1=12ch@25MHz, 2=6ch@50MHz, 3=3ch@100MHz |
| `rle` | bool | **RLE 硬件壓縮**（僅 Buffer 模式）|
| `ext_clock` | bool | **使用外部輸入時鐘採樣** |
| `falling_edge_clock` | bool | **使用時鐘下降沿採樣** |
| `max_height` | str | `"1X"` / `"2X"` / `"5X"`（DSO 顯示倍率，logic 採集通常不需要）|
| `confirm` | bool | **必填 `True` 才會實際採集**；預設 `False` 只回傳設定 preview |

### 二段確認流程（重要）

呼叫 `capture(...)` **不帶 `confirm=True`** 時，工具**不會碰硬體**，只回傳：

```json
{
  "needs_confirm": true,
  "preview": { ...所有設定... },
  "hint": "Review the settings above, then call capture(...) again with confirm=True ..."
}
```

LLM / 使用者看完設定（特別是 vth、samplerate、channels）覺得對，再帶 `confirm=True` 重打一次同樣的呼叫，硬體才會真的觸發採集。這個機制避免 LLM 在誤解使用者意圖時就把訊號採壞、佔住裝置或設錯 threshold。

對話範例：

> 使用者：「對 ch0 在 1MHz 抓 200k 樣本，1.8V 系統。」
>
> Claude → `capture(samplerate=1_000_000, depth=200_000, channels=[0], vth=0.9)`
>
> 收到 preview，顯示給使用者確認。
>
> 使用者：「OK 跑吧。」
>
> Claude → `capture(samplerate=1_000_000, depth=200_000, channels=[0], vth=0.9, confirm=True)`
>
> 真實採集 → 回傳 `capture_id`。

### 環境需求（macOS）

```bash
brew install cmake pkg-config glib libusb
# Python 3.10+
```

DSLogic Plus 在 macOS 不需 kext，但 **DSView GUI 開著時會佔住裝置** —— 採集前請先關 GUI。

### Build

```bash
cd mcp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 應該得到 mcp/build/dsview_helper
./build/dsview_helper list-devices --firmware ../DSView/res
```

### 安裝 Python 套件

```bash
cd mcp
python3 -m venv .venv
.venv/bin/pip install -e .
```

### 註冊到 Claude Code

```bash
claude mcp add dsview \
  --command /Users/oliver/code/qtui/DSView/mcp/.venv/bin/dsview-mcp
```

或手動編輯 `~/.claude.json`，加入：

```json
{
  "mcpServers": {
    "dsview": {
      "command": "/Users/oliver/code/qtui/DSView/mcp/.venv/bin/dsview-mcp",
      "env": {
        "DSVIEW_HELPER": "/Users/oliver/code/qtui/DSView/mcp/build/dsview_helper",
        "DSVIEW_FIRMWARE_DIR": "/Users/oliver/code/qtui/DSView/DSView/res",
        "DSVIEW_DECODERS_DIR": "/Users/oliver/code/qtui/DSView/libsigrokdecode4DSL/decoders",
        "DSVIEW_WORKDIR": "/Users/oliver/.dsview/captures"
      }
    }
  }
}
```

啟 Claude Code 後 `/mcp` 應看到 `dsview`。

### 環境變數

| 變數 | 預設 | 說明 |
|---|---|---|
| `DSVIEW_HELPER` | `mcp/build/dsview_helper` | C helper 路徑 |
| `DSVIEW_FIRMWARE_DIR` | `DSView/res` | DSLogic firmware bin/fw 目錄 |
| `DSVIEW_DECODERS_DIR` | `libsigrokdecode4DSL/decoders` | 協議解碼器目錄 |
| `DSVIEW_USER_DATA_DIR` | `~/.dsview` | 使用者資料夾 |
| `DSVIEW_WORKDIR` | `~/.dsview/captures` | capture 結果存放處 |

### 直接測試 helper（不經 MCP）

```bash
HELPER=./build/dsview_helper
RES=../DSView/res
DEC=../libsigrokdecode4DSL/decoders

$HELPER list-devices --firmware $RES
$HELPER device-info  --index 0 --firmware $RES

# 1.8V 系統建議 vth=0.9
$HELPER capture --index 0 --output /tmp/cap1 \
        --samplerate 1000000 --depth 200000 \
        --channels 0,1,2,3 --vth 0.9 \
        --operation-mode 1 --filter 1 \
        --timeout 10 --firmware $RES

# 解碼一段 SPI capture：
$HELPER decode --input /tmp/cap1 --protocol 0:spi \
        --map "clk=1,mosi=2,miso=3,cs=0" \
        --decoders-dir $DEC
```

### 協議解碼器命名

DreamSourceLab 的解碼器使用 `0:` 與 `1:` 前綴（兩個版本：upstream / DSL 自家修改版）。常用：

| protocol id | 用途 | 必要通道 |
|---|---|---|
| `0:uart`, `1:uart` | UART / RS-232 | `rxtx` |
| `0:i2c`, `1:i2c`   | I²C | `scl`, `sda` |
| `0:spi`, `1:spi`   | SPI | `clk`, `mosi`, `miso`, `cs` (cs 可選) |
| `0:can`            | CAN | `can` |
| `0:swd`            | ARM SWD | `swclk`, `swdio` |

完整列表用 `list_decoders()` 拉。

### Context-saving（讓 LLM 對話省 token）

每個 tool 都做過 token 精簡，重要 default 行為：

| Tool | 行為 |
|---|---|
| `decode(...)` | 預設 `output="summary"` — 把 SPI/I²C/UART 的 bit-level annotations（每 byte 18 條）摺成 hex byte stream。**SPI 16 byte：41,000 tokens → 200 tokens（99% 節省）**。需要 raw annotation 才指定 `output="raw"` |
| `list_decoders` | 預設 compact 只回 `{id, name}` pair；`detail=True` 才連 channels/options 一起回。`filter_substring` 跟 `detail` 兩個都不指定會 error，避免一次拉 150+ decoder |
| `device_info` | 預設只回 enabled channels；要全部用 `include_disabled=True` |
| `read_window` | 預設 length 從 256 → 128。length=4096 會花 ~1k tokens，避免亂用 |
| `capture(confirm=True)` | 不再 echo 完整 settings dict（你自己傳的，沒必要再回來）。回 `capture_id`、samples、channels 就夠 |

實測一個典型 SPI workflow（list_devices → capture preview → capture confirm → list_decoders → decode）：
- 之前：~16,400 tokens
- 之後：~550 tokens（**節省 30 倍**）

### LLM 自己迭代 channel_map（重要技巧）

`decode(...)` 的 `channel_map` 是 LLM 動態指定的 — 沒解出合理 byte 時直接重新呼叫即可，**不必重新採集**：

```python
# 第一次：先猜
decode(capture_id="...", protocol="1:spi",
       channel_map={"clk":12, "cs":13, "mosi":14, "miso":15})
# → 0 個 annotation 或都是 frame error？

# 看一下波形
read_window(capture_id="...", channel=12, length=200)  # 找誰是 clock
analyze(capture_id="...", channel=12)                  # 找誰 edge 多

# 換 mapping 重 decode（之前的 capture 還在）
decode(... channel_map={"clk":12, "cs":13, "mosi":15, "miso":14})  # swap
# 或試不同 cpol / cpha / cs_polarity 組合
```

採集是真正的「副作用」（佔住硬體、寫盤），decode 是讀檔 — 反覆試 mapping / 解碼參數成本很低，這是 LLM 預期用法。

### Demo Device 的 protocol pattern

Demo Device 有兩種訊號模式：

- **Random**（fallback）：`rand()` 生成的偽隨機 16-channel 雜訊，看不出 i2c/uart/spi
- **Protocol pattern**：載入 `~/.dsview/demo/logic/protocol.demo`，產生 GUI 上看到的完整 i2c / uart / spi / can-fd 訊號（最長 ~5.24ms 視窗）

`dsview-mcp` 啟動時會自動把 repo 內的 `DSView/demo/` symlink 到 `~/.dsview/demo/`，所以 demo 一律走 protocol pattern。如果要重置回 random，刪掉 symlink 即可：`rm ~/.dsview/demo`。

**Demo Device 必須啟全 16 ch**：`.demo` 檔內部是 fixed 16-channel atomic-block layout，driver 不會依 enabled count 重新 pack。只啟用部分 channel 會導致 atomic-block 內 slot 對應錯位，decoder 解不出資料（或拿到位移過的訊號）。實體 DSLogic 沒這個限制 — driver 真的會依 enabled 動態 pack。

對應的 channel 對照（demo `protocol.demo`）：

| Channel | 內容 |
|---|---|
| ch0, ch1 | I²C (sda=0, scl=1) — 24xx EEPROM `Sequential random read` |
| ch5 | UART RX/TX |
| ch9 | CAN-FD |
| ch12-15 | SPI (clk=12, cs#=13, mosi=14, miso=15) — SSDP/UPnP `M-SEARCH` |

### 已知限制

- **觸發條件**未支援（v3 預留）
- **連續串流**未支援
- 載入/儲存 `.dsl` session 檔未支援
- 多解碼器堆疊（stacked decoders，例如 spi → spiflash、i2c → 24xx EEPROM）未支援
- DSLogic Plus 的 vth 上限約 1.98V（pid 0x0020 此版 firmware）
- 採集中 helper 是同步 block，timeout 預設 30s。長採集請拉高 `timeout_sec`

---

## English

### Structure

```
mcp/
├── CMakeLists.txt          # Builds dsview_helper (no need to build full DSView)
├── dsview_helper/main.c    # Thin CLI wrapping libsigrok4DSL
├── dsview_mcp/server.py    # FastMCP server exposing tools to LLMs
├── pyproject.toml
└── README.md
```

### MCP Tools

| Tool | Purpose |
|---|---|
| `list_devices()` | List attached DSLogic / DSCope / Demo |
| `device_info(index)` | Channels, samplerate, depth for the device |
| `capture(...)` | **Two-step confirm**: returns a settings preview first; pass `confirm=True` to actually run |
| `list_captures(limit)` | Recent captures in workdir |
| `analyze(capture_id, channel)` | Edges, duty cycle, frequency estimate, pulse-width stats |
| `read_window(capture_id, channel, start, length)` | Raw 0/1 bit slice for the LLM to inspect |
| `list_decoders(filter)` | Available protocol decoders (150+) |
| `decode(capture_id, protocol, channel_map, options)` | Run a decoder, return annotations |

Captures land in `~/.dsview/captures/cap-<id>.{bin,json}`. The `.bin` uses
DSLogic's native atomic-block layout (each 64-sample × enabled-channel block
holds per-channel 8-byte LSB-first packed runs); `server.py` decodes it
transparently.

### `capture(...)` Parameters

Mirrors the DSView GUI "Device Options" panel:

| Param | Type | GUI mapping / notes |
|---|---|---|
| `samplerate` | int | Samples/sec (e.g. 1_000_000 = 1 MS/s) |
| `depth` | int | Total samples to capture |
| `channels` | list[int] | Channel indices to enable; None = keep current |
| `index` | int | Device index from `list_devices()`; -1 = last attached |
| `timeout_sec` | int | Hard timeout (seconds) |
| `vth` | float | **Voltage threshold** (V). For 1.8V logic try 0.9; for 3.3V/5V leave None |
| `operation_mode` | str/int | **Run mode**: `"buffer"` / `"stream"` / `"internal_test"` / `"external_test"` / `"loopback_test"` |
| `buffer_option` | str/int | **Stop option**: `"stop"` (abort) or `"upload"` (upload partial data) |
| `filter` | str/int | **Filter**: `"none"` or `"1t"` (1-sample-clock glitch filter) |
| `channel_mode` | int | **Channel mode**: 0=16ch@20MHz, 1=12ch@25MHz, 2=6ch@50MHz, 3=3ch@100MHz (DSLogic Plus) |
| `rle` | bool | **Hardware RLE compression** (Buffer mode only) |
| `ext_clock` | bool | **Sample on external clock** input |
| `falling_edge_clock` | bool | **Sample on falling edge** of the clock |
| `max_height` | str | `"1X"` / `"2X"` / `"5X"` (DSO display, rarely used for logic) |
| `confirm` | bool | **Required `True` for actual acquisition**; default `False` returns preview only |

### Two-Step Confirmation Flow (important)

Calling `capture(...)` **without `confirm=True`** does **not touch the
hardware**; it only returns:

```json
{
  "needs_confirm": true,
  "preview": { ...all settings... },
  "hint": "Review the settings above, then call capture(...) again with confirm=True ..."
}
```

The LLM (or human) reviews the preview — especially `vth`, `samplerate`,
`channels` — and only then re-issues the same call with `confirm=True` to
trigger real acquisition. This guards against the LLM mis-interpreting the
user's intent and corrupting a capture, hogging the device, or applying the
wrong threshold.

Example dialogue:

> User: "Capture ch0 at 1 MHz for 200 k samples, it's a 1.8V system."
>
> Claude → `capture(samplerate=1_000_000, depth=200_000, channels=[0], vth=0.9)`
>
> Receives preview; shows it to the user.
>
> User: "Looks right, go."
>
> Claude → `capture(samplerate=1_000_000, depth=200_000, channels=[0], vth=0.9, confirm=True)`
>
> Real acquisition → returns `capture_id`.

### Requirements (macOS)

```bash
brew install cmake pkg-config glib libusb
# Python 3.10+
```

DSLogic Plus needs no kext on macOS, but **the DSView GUI holds the device
exclusively** — close the GUI before running captures.

### Build

```bash
cd mcp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/dsview_helper list-devices --firmware ../DSView/res
```

### Install the Python Package

```bash
cd mcp
python3 -m venv .venv
.venv/bin/pip install -e .
```

### Register with Claude Code

```bash
claude mcp add dsview \
  --command /Users/oliver/code/qtui/DSView/mcp/.venv/bin/dsview-mcp
```

Or edit `~/.claude.json` directly:

```json
{
  "mcpServers": {
    "dsview": {
      "command": "/Users/oliver/code/qtui/DSView/mcp/.venv/bin/dsview-mcp",
      "env": {
        "DSVIEW_HELPER": "/Users/oliver/code/qtui/DSView/mcp/build/dsview_helper",
        "DSVIEW_FIRMWARE_DIR": "/Users/oliver/code/qtui/DSView/DSView/res",
        "DSVIEW_DECODERS_DIR": "/Users/oliver/code/qtui/DSView/libsigrokdecode4DSL/decoders",
        "DSVIEW_WORKDIR": "/Users/oliver/.dsview/captures"
      }
    }
  }
}
```

Restart Claude Code; `/mcp` should show `dsview`.

### Environment Variables

| Var | Default | Meaning |
|---|---|---|
| `DSVIEW_HELPER` | `mcp/build/dsview_helper` | C helper binary |
| `DSVIEW_FIRMWARE_DIR` | `DSView/res` | DSLogic firmware bin/fw dir |
| `DSVIEW_DECODERS_DIR` | `libsigrokdecode4DSL/decoders` | Protocol-decoder dir |
| `DSVIEW_USER_DATA_DIR` | `~/.dsview` | User data dir |
| `DSVIEW_WORKDIR` | `~/.dsview/captures` | Where captures land |

### Testing the Helper Directly (no MCP)

```bash
HELPER=./build/dsview_helper
RES=../DSView/res
DEC=../libsigrokdecode4DSL/decoders

$HELPER list-devices --firmware $RES
$HELPER device-info  --index 0 --firmware $RES

# vth=0.9 recommended for a 1.8V system
$HELPER capture --index 0 --output /tmp/cap1 \
        --samplerate 1000000 --depth 200000 \
        --channels 0,1,2,3 --vth 0.9 \
        --operation-mode 1 --filter 1 \
        --timeout 10 --firmware $RES

# Decode an SPI capture:
$HELPER decode --input /tmp/cap1 --protocol 0:spi \
        --map "clk=1,mosi=2,miso=3,cs=0" \
        --decoders-dir $DEC
```

### Decoder Naming

DreamSourceLab's decoders are prefixed `0:` / `1:` (two flavors: upstream /
DSL-modified). Common ones:

| protocol id | Use | Required pins |
|---|---|---|
| `0:uart`, `1:uart` | UART / RS-232 | `rxtx` |
| `0:i2c`, `1:i2c`   | I²C | `scl`, `sda` |
| `0:spi`, `1:spi`   | SPI | `clk`, `mosi`, `miso`, `cs` (cs optional) |
| `0:can`            | CAN | `can` |
| `0:swd`            | ARM SWD | `swclk`, `swdio` |

Use `list_decoders()` for the full list.

### Known Limitations

- Trigger conditions not yet exposed (v3)
- Continuous streaming not exposed
- `.dsl` session save/load not exposed
- Stacked decoders (e.g. `spi → spiflash`) not exposed
- DSLogic Plus `vth` upper limit ≈ 1.98 V on this firmware
- Helper is synchronous and blocking; default timeout 30 s — bump
  `timeout_sec` for long captures
