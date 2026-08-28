# ESP-Switch 部署指引（新机器快速安装）

本项目的固件/网页控制台依赖**两套独立编译链**，按目标芯片分工：

| 目标芯片 | 编译链 | 入口 |
|---|---|---|
| ESP32-C3 / ESP8285 / ESP8266 | arduino-cli + esp32/esp8266 core | 网页 Tab1（Arduino CLI） |
| ESP32-C2 | **ESP-IDF 5.3.x + arduino-esp32 作组件** | 网页 Tab2（ESP32-C2） |

> 为什么 C2 要特殊处理：arduino-esp32 3.x 缺少 `esp32c2-libs` 预编译包，arduino-cli/PIO 直接编 C2 会失败。把 arduino-esp32 作为 **ESP-IDF 组件**编译时，bootloader/SDK 由 IDF 生成，绕开缺失的包。

---

## 1. 基础依赖

- **git**、**Python 3.8+**（IDF 需要）、**Node.js 16+**（网页服务）
- macOS / Linux / Windows 均可（下文以 macOS 命令为例，Windows 用 espressif 的 PowerShell 安装器代替 `install.sh`/`export.sh`）

## 2. 安装 arduino-cli（Tab1 用）

```bash
# macOS (Homebrew)
brew install arduino-cli
# 或官方脚本
# curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh

# 安装核心
arduino-cli config init
arduino-cli core update-index
arduino-cli core install esp32:esp32          # C3 用（3.x 即可）
arduino-cli core install esp8266:esp8266      # 8285/8266 用
```

> ⚠️ **不要**依赖 arduino-cli 编 C2 —— 3.x 缺 `esp32c2-libs`。C2 一律走下面 IDF 路径。

## 3. 安装 ESP-IDF 5.3.x（Tab2 用，含 C2 工具链）

```bash
# 下载源码（建议放固定路径，如 ~/esp/esp-idf）
mkdir -p ~/esp && cd ~/esp
git clone -b v5.3.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c2    # 只装 C2 所需工具链（riscv32-esp-elf），也可不传参数装全量
```

> IDF 5.3.x 都行（arduino-esp32 3.3.11 要求 IDF ∈ [5.3.0, 6.1.99]）。工具链装在 `~/.espressif/tools/`，python venv 在 `~/.espressif/python_env/idf5.3_py*_env`。

## 4. arduino-esp32 源码（作为 IDF 组件）

不需要单独装，**复用 arduino-cli 已下载的 3.3.11 源码**，用软链挂进工程：

```bash
# macOS
ln -sfn ~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.11 \
        <仓库>/idf-c2/components/arduino
# Linux 数据目录是 ~/.arduino15（不是 ~/Library/Arduino15）

# 验证
ls idf-c2/components/arduino/variants/esp32c2/pins_arduino.h   # 必须存在
```

> 若 arduino-cli 装的是 3.3.11 之外的 3.x 小版本，路径里的 `3.3.11` 换成实际版本号，并确认 `variants/esp32c2` 存在即可。

## 5. 打补丁：arduino 组件加 WiFi 依赖

**必须改**（否则 C2 编译报 `WiFiType.h includes esp_wifi_types.h`）：

`idf-c2/components/arduino/CMakeLists.txt`（即软链指向的源码文件）第 ~402 行，
在 `set(requires ...)` 末尾追加：

```cmake
esp_wifi esp_netif esp_event esp_phy
```

即：
```cmake
set(requires spi_flash esp_partition mbedtls wpa_supplicant esp_adc esp_eth http_parser esp_ringbuf esp_driver_gptimer esp_driver_usb_serial_jtag driver esp_http_client esp_https_ota esp_timer esp_wifi esp_netif esp_event esp_phy)
```

> 注意：不要加 `esp_mac`（组件不存在）或 `mdns`（IDF 5.3 已移入组件管理器，自动拉取 `espressif__mdns`）。
> 此改动声明依赖、无副作用，但会影响所有复用该 arduino 源码作 IDF 组件的项目。

## 6. 项目内配置（每台机器必改 / 或设环境变量）

`arduino-cli-web/server.js` 顶部路径**优先读环境变量，缺省才用写死值**。换机器有两种方式（二选一）：

**方式 A：设环境变量（推荐，改一行命令即可）**
```bash
export ESP_SWITCH_SKETCH_DIR=<仓库>/ESP32_Light_Switch   # 固件源码 .ino
export ESP_SWITCH_IDF_DIR=<你的>/esp-idf                 # IDF 源码路径（可选，优先级最高）
export ESP_SWITCH_IDF_C2_DIR=<仓库>/idf-c2              # C2 IDF 工程
node server.js
```

> **IDF 版本选择（重要）**：`IDF_DIR` 解析优先级为 `ESP_SWITCH_IDF_DIR` > 环境变量 `IDF_PATH` > 写死默认值。
> 也就是说：**你 `source <某版 IDF>/export.sh` 后启动 server.js，它就用那个 IDF 编译 C2** —— 无需再设任何变量。
> 前提是版本在 arduino-esp32 3.3.11 支持范围 [5.3.0, 6.1.99] 内。venv 前缀会从该 IDF 的
> `version.cmake` 自动推导（或读环境 `IDF_PYTHON_ENV_PATH`），跨版本无需手工指定。

**方式 B：直接改 server.js 顶部写死值**
```js
const SKETCH_DIR = '<仓库>/ESP32_Light_Switch';
const BUILD_BASE = '/tmp/espbuild';                 // arduino 编译产物缓存（可保持默认）
const IDF_DIR    = '<你的>/esp-idf';
const IDF_C2_DIR = '<仓库>/idf-c2';
```

> ⚠️ 若用 `IDF_PATH` 指向 IDF，请确认版本在 arduino-esp32 支持范围 [5.3.0, 6.1.99] 内；版本不符时用 `ESP_SWITCH_IDF_DIR` 显式覆盖。venv 前缀自动从该 IDF 的 `version.cmake` 推导（或读环境 `IDF_PYTHON_ENV_PATH`），一般无需手工指定；特殊情况可用 `ESP_SWITCH_IDF_VENV_PREFIX` 覆盖。

`idf-c2/sdkconfig.defaults` 默认 4MB flash + 双 OTA 分区；若目标板 flash 不是 4MB 需改
`CONFIG_ESPTOOLPY_FLASHSIZE_*` 与 `partitions.csv`（键名是 `FLASHSIZE`，无 `_SIZE`，写错会被静默忽略退回 2MB）。

## 7. 验证

```bash
# Tab1 链路（C3 编译冒烟）
arduino-cli compile --fqbn esp32:esp32:esp32c3 ESP32_Light_Switch

# Tab2 链路（C2 编译冒烟）
cd idf-c2
. ~/esp/esp-idf/export.sh
idf.py set-target esp32c2 && idf.py -B build/BOARD_ESP32C2_SWITCH_NANO build

# 网页服务
cd arduino-cli-web && node server.js    # 打开 http://localhost:8787
```

成功标志：Tab1 能编 C3/8285；Tab2 能编 C2 且「构建产物」Tab 能看到 `.bin` + 生成时间。

## 8. 常见问题

| 症状 | 原因 / 修复 |
|---|---|
| `Cannot import module "click"` | `export.sh` 探测到无 click 的 python（如 conda）。server.js 已显式锁定 `IDF_PYTHON_ENV_PATH` + PATH 前置 venv bin，若手动命令行复现，先 `source export.sh` 再跑 |
| `Field 'type' can't be left empty` | `partitions.csv` 带 UTF-8 BOM → 用 `sed -i '1s/^\xef\xbb\xbf//' partitions.csv`（macOS 用 python 去 BOM） |
| `does not fit in configured flash size 2MB` | `sdkconfig.defaults` 的 flash 键名写错（应为 `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`），删 `sdkconfig` + `build/` 后重编 |
| `arduino-cli: unknown shorthand flag: 'D'` | 网页服务已用 `--build-property` 注入宏，无需手动加 `-D` |
| C2 在 arduino-cli 下拉里看不到 | 正常 —— C2 板型已从 Tab1 移除，只在 Tab2（IDF）编译 |
