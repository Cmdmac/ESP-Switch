# ESP-Switch 部署指引（新机器快速安装）

本项目的固件/网页控制台依赖**两套独立编译链**，按目标芯片分工：

| 目标芯片 | 编译链 | 入口 |
|---|---|---|
| ESP32-C3 / ESP8285 / ESP8266 | arduino-cli + esp32/esp8266 core | 网页 Tab1（Arduino CLI） |
| ESP32-C2 | **ESP-IDF 5.x + arduino-esp32 作组件** | 网页 Tab2（ESP32-C2） |

> 为什么 C2 要特殊处理：arduino-esp32 3.x 缺少 `esp32c2-libs` 预编译包，arduino-cli/PIO 直接编 C2 会失败。把 arduino-esp32 作为 **ESP-IDF 组件**编译时，bootloader/SDK 由 IDF 生成，绕开缺失的包。

---

## 1. 基础依赖

- **git**、**Python 3.9+**（IDF 5.5 需要；5.3/5.4 为 3.8+）、**Node.js 16+**（网页服务）
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

## 3. 安装 ESP-IDF 5.x（Tab2 用，含 C2 工具链）

```bash
# 下载源码（建议放固定路径，如 ~/esp/esp-idf）
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c2    # 只装 C2 所需工具链（riscv32-esp-elf），也可不传参数装全量
```

> IDF 5.x 均可（本项目已在 5.5.2 上验证；arduino-esp32 3.3.11 要求 IDF ∈ [5.3, 6.2)）。
> 工具链装在 `~/.espressif/tools/`，python venv 在 `~/.espressif/python_env/idf5.5_py*_env`。
> ⚠️ **工具链版本必须与 IDF 匹配**：网页服务启动时会解析每个候选 IDF 的
> `tools/tools.json`，若 `riscv32-esp-elf` 需求版本未安装（例如 IDF 升级到 5.5.4 但工具链还是
> 5.5.2 时代的 `20251107`），会自动跳过该候选、选用匹配的 IDF（如 5.5.2），无需手工干预。

> ⚠️ **Python 版本**：IDF 5.5 官方下限是 python **3.9**（`tools/python_version_checker.py`，5.3/5.4 才是 3.8），
> 无显式上限；但 **3.14 装不上**——依赖 `cryptography<45` 没有 cp314 预编译 wheel 且 `--only-binary` 禁止源码编译。
> 较新的发行版（如 Ubuntu 26.04）自带 python3.14，直接 `./install.sh` 会失败（症状 `idf.py: command not found`）。
> 解决：**重跑 `scripts/setup-linux.sh`** —— 它只检测不安装，会提示缺什么并给出手动命令；或手动
> `sudo apt install python3.12 python3.12-venv` 后 `python3.12 tools/idf_tools.py install_python_env`。

## 4. arduino-esp32 组件（ESP-IDF 组件管理器）

不需要单独下载源码，也**不需要软链**——arduino-esp32 通过官方组件管理器引入，
声明文件是 `idf-c2/main/idf_component.yml`：

```yaml
dependencies:
  espressif/arduino-esp32: "^3.3.11"
```

首次构建时 `idf.py` 会自动从 ESP 组件仓库把它（及其依赖）下载到
`idf-c2/managed_components/espressif__arduino-esp32`，之后复用缓存：

```bash
cd idf-c2 && idf.py set-target esp32c2   # 首次：联网拉取组件，耗时较长
ls managed_components/espressif__arduino-esp32/variants/esp32c2/pins_arduino.h   # 验证
```

> 组件在 CMake 中的名字是 `espressif__arduino-esp32`（见 `main/CMakeLists.txt` 的 REQUIRES）。
> 目录 `managed_components/`、`dependencies.lock` 已在 .gitignore 中，不会提交。
> 若无法访问 components.espressif.com，可设 `IDF_COMPONENT_STORAGE_URL` 指向镜像。

## 5. arduino 组件缺失依赖：由工程侧补齐

arduino-esp32 自带的 CMakeLists.txt 未声明 `esp_wifi` / `esp_netif` / `esp_event` 等依赖
（3.3.4 与 3.3.11 均如此），而 IDF 5.x 起这类组件不再默认加入公共依赖，直接编译会报
`WiFiType.h includes esp_wifi_types.h ... not in the requirements list`。

本工程**不改 arduino 组件自身的文件**，而是在 `idf-c2/main/CMakeLists.txt` 里：
把缺失组件列进 main 的 `REQUIRES`，再用 `target_include_directories` /
`target_link_libraries` 把它们的头文件目录与链接转发给 arduino 组件：

```cmake
set(ARDUINO_MISSING_REQUIRES esp_wifi esp_netif esp_event esp_common driver esp_hw_support)
idf_component_get_property(_arduino_lib espressif__arduino-esp32 COMPONENT_LIB)
...
```

> 若后续又缺别的组件，把组件名同时加进上面的 `REQUIRES` 和 `ARDUINO_MISSING_REQUIRES` 两个列表即可。
> 注意不要加 `esp_mac`（IDF 中无此组件）。

## 6. 项目内配置（每台机器必改 / 或设环境变量）

`arduino-cli-web/server.js` 顶部路径**优先读环境变量，缺省才用写死值**。换机器有两种方式（二选一）：

**方式 A：设环境变量（推荐，改一行命令即可）**
```bash
export ESP_SWITCH_SKETCH_DIR=<仓库>/ESP32_Light_Switch    # 固件源码 .ino
export ESP_SWITCH_IDF_DIR=<你的>/esp-idf                  # IDF 源码路径（可选，优先级最高）
export ESP_SWITCH_IDF_C2_DIR=<仓库>/idf-c2               # C2 IDF 工程
export ESP_SWITCH_BUILD_BASE=<你的>/espbuild             # arduino 编译缓存（可选，默认系统临时目录）
node server.js
```

> **IDF 探测逻辑（重要）**：优先级为 `ESP_SWITCH_IDF_DIR` > 环境变量 `IDF_PATH` > 自动扫描
> （`$IDF_TOOLS_PATH/frameworks`、`~/.espressif/<版本>/esp-idf`（eim 布局）、`~/esp`、`~/esp-idf`、`/opt`）。
> 自动扫描会**校验工具链匹配**（见第 3 步），只挑 `riscv32-esp-elf` 需求版本已安装的 IDF，避免选中
> "IDF 新但工具链旧"的组合。也就是说：**你 `source <某版 IDF>/export.sh` 后启动 server.js，它就用那个
> IDF 编译 C2** —— 无需再设任何变量。
> 前提是版本在 arduino-esp32 3.3.11 支持范围 [5.3, 6.2) 内。venv 前缀会从该 IDF 的
> `version.cmake` 自动推导（或读环境 `IDF_PYTHON_ENV_PATH`），跨版本无需手工指定。

**方式 B：直接改 server.js 顶部写死值**
```js
const SKETCH_DIR = '<仓库>/ESP32_Light_Switch';
const BUILD_BASE = '/tmp/espbuild';                 // arduino 编译产物缓存（默认系统临时目录，Windows 为 %TEMP%\espbuild）
const IDF_DIR    = '<你的>/esp-idf';
const IDF_C2_DIR = '<仓库>/idf-c2';
```

> ⚠️ 若用 `IDF_PATH` 指向 IDF，请确认版本在 arduino-esp32 支持范围 [5.3, 6.2) 内；版本不符时用 `ESP_SWITCH_IDF_DIR` 显式覆盖。venv 前缀自动从该 IDF 的 `version.cmake` 推导（或读环境 `IDF_PYTHON_ENV_PATH`），一般无需手工指定；特殊情况可用 `ESP_SWITCH_IDF_VENV_PREFIX` 覆盖。

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
| `idf.py: command not found`（C2 构建） | 多为系统 python 过新（如 Ubuntu 26.04 的 3.14，IDF 5.5 不支持）导致 venv 名不匹配。`scripts/setup-linux.sh` **只检测不安装**，会提示缺什么；需手动装兼容 python（如 `sudo apt install python3.12 python3.12-venv`）并建 venv（`cd <idf> && python3.12 tools/idf_tools.py install_python_env`）。已建好 venv 后首次构建由 server 自动识别接管 |
| `Field 'type' can't be left empty` | `partitions.csv` 带 UTF-8 BOM → 用 `sed -i '1s/^\xef\xbb\xbf//' partitions.csv`（macOS 用 python 去 BOM） |
| `does not fit in configured flash size 2MB` | `sdkconfig.defaults` 的 flash 键名写错（应为 `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`），删 `sdkconfig` + `build/` 后重编 |
| `arduino-cli: unknown shorthand flag: 'D'` | 网页服务已用 `--build-property` 注入宏，无需手动加 `-D` |
| C2 在 arduino-cli 下拉里看不到 | 正常 —— C2 板型已从 Tab1 移除，只在 Tab2（IDF）编译 |
