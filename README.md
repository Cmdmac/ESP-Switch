# ESP-Switch

ESP 通断器固件，支持网页控制。基于 Arduino 框架，同一份代码同时支持 **ESP32-C2 / ESP32-C3** 与 **ESP8285（ESP8266 内核）**。

## 功能

- **Wi-Fi 热点模式**：无需路由器，手机/电脑直接连接设备 AP
- **网页控制**：开关、亮度滑条、环境亮度显示、实时电流显示
- **定时开关**：按日循环，设置开启/关闭时间
- **设置持久化**：亮度、开关状态、定时设置保存到 Flash
- **本地按键**：板载 BOOT 键复用为本地控制——短按切换开关、长按 3 秒恢复出厂
- **网页 OTA 升级**：在网页选择 `.bin` 固件即可无线升级，无需 USB 烧录

> **芯片能力差异**：ESP32-C2/C3 支持全部功能（PWM 调光 + 环境光 + 电流）。ESP8266/ESP8285 只有 **1 路 ADC**（量程 0~1.0V），因此同一块 ESP8266 板只能在“环境光”和“电流”中选其一（见下表）；但亮度 PWM 调光仍然支持（用 `analogWrite` 软件 PWM）。网页会按设备能力自动隐藏不支持的功能卡片。

## 硬件引脚映射（按产品板）

同一份固件通过编译宏 **-DBOARD_XXX** 选择产品板（引脚 / 功能）。未指定时默认 `ESP32C2-Switch-Nano`。

| 产品板 | 芯片 (FQBN) | 灯 PWM | 环境光 | 电流 | BOOT 键 |
|--------|------------|--------|--------|------|---------|
| ESP01F-Switch | ESP8266 (`esp8266:esp8266:generic`) | GPIO2 | ADC (A0) | — | GPIO5 |
| ESP32C3-Switch | ESP32-C3 (`esp32:esp32:esp32c3`) | GPIO4 | GPIO3 | GPIO0 | GPIO9 |
| ESP32C2-Switch-Nano | ESP32-C2 (`esp32:esp32:esp32c2`) | GPIO4 | GPIO3 | GPIO0 | GPIO9 |
| ESP32C2-Switch-Dev | ESP32-C2 (`esp32:esp32:esp32c2`) | GPIO6 | GPIO3 | GPIO0 | GPIO9 |
| ESP8285-Switch | ESP8266 (`esp8266:esp8266:generic`) | GPIO5 | — | ADC (A0) | GPIO0 |
| ESP32C2-Module | ESP32-C2 (`esp32:esp32:esp32c2`) | GPIO18 | — | — | GPIO9 |
| ESP8285-Module | ESP8266 (`esp8266:esp8266:generic`) | GPIO8 | — | — | GPIO0 |
| ESPC3-Module | ESP32-C3 (`esp32:esp32:esp32c3`) | GPIO10 | — | — | GPIO9 |

> **“—” = 未连接（NC）**：对应功能在该板上关闭（环境光/电流不显示，网页自动隐藏相关卡片）。
> **ESP8266 的 “ADC”** 指唯一的 A0 通道（量程 0~1.0V），同一块 ESP8266 板只能把 A0 用于“环境光”或“电流”之一（见上表）。

### 选择产品板编译

- **网页控制台（推荐）**：板型下拉直接选产品板，编译/上传会自动注入对应宏（esp32 核心用 `build.defines`、esp8266 用 `build.extra_flags`，避免覆盖核心自带的关键宏）。
- **arduino-cli**：arduino-cli 没有 `-D` 标志，要用 `--build-property`：
  - ESP32 核心（esp32c2/esp32c3）：注入到 `build.defines` 槽（不要直接覆盖 `build.extra_flags`，否则会丢掉 `-DESP32=ESP32` 等核心宏）：
    ```bash
    arduino-cli compile -b esp32:esp32:esp32c2 --build-property build.defines=-DBOARD_ESP32C2_SWITCH_DEV ESP32_Light_Switch/ESP32_Light_Switch.ino
    ```
  - ESP8266 核心（esp01f/esp8285）：`build.extra_flags` 默认空且被 recipe 直接引用，可覆盖：
    ```bash
    arduino-cli compile -b esp8266:esp8266:generic --build-property build.extra_flags=-DBOARD_ESP8285_SWITCH ESP32_Light_Switch/ESP32_Light_Switch.ino
    ```
- **Arduino IDE / PlatformIO**：在编译选项里加 `-DBOARD_ESP32C3_SWITCH` 之类的宏定义即可。

> 选 ESP8266 产品板（ESP01F / ESP8285）时务必用 ESP8266 核心（FQBN `esp8266:esp8266:generic`）；选 ESP32 产品板时用 ESP32 核心。芯片与板型不匹配会导致引脚无意义或编译报错（如 ESP8266 上引用 `A0` 以外的 ADC 引脚）。

## 开发环境

> 📄 **新机器部署**：完整的安装步骤（两套编译链、路径修改、常见坑）见 **[DEPLOY.md](DEPLOY.md)**。

本项目固件按目标芯片分两条编译链：

| 目标芯片 | 编译链 | 说明 |
|---|---|---|
| ESP32-C3 / ESP8285 / ESP8266 | arduino-cli + esp32(3.x)/esp8266 core | 网页 Tab1「Arduino CLI」 |
| ESP32-C2 | **ESP-IDF 5.3.x + arduino-esp32 作组件** | 网页 Tab2「ESP32-C2」 |

- **C2 为什么走 IDF**：arduino-esp32 3.x 删掉了 `esp32c2-libs` 预编译包，arduino-cli/PIO 无法直接编 C2；把 arduino-esp32 作为 **ESP-IDF 组件**编译时，bootloader/SDK 由 IDF 5.3.2 生成，绕开缺失的包。C2 板型因此**只出现在网页 Tab2**（Tab1 下拉不含 C2）。
- **同一份源码、两条编译链**：`ESP32_Light_Switch.ino` 是唯一固件源码（arduino-cli 直接编译）；`idf-c2/main/main.cpp` 是薄壳，`#include` 同一份 `.ino` + 自写 `app_main()`。改固件只改 `.ino`，两边编译都生效。
- C3/8285 板在 arduino 路径选板后，用 `-DBOARD_XXX` 宏决定引脚/功能（网页控制台自动注入宏）。

### 网页控制台（推荐）

```bash
cd arduino-cli-web && node server.js   # 打开 http://localhost:8787
```

- Tab1「Arduino CLI」：C3 / 8285 等 5 个产品板的编译、上传、串口监视、日志、固件下载。
- Tab2「ESP32-C2」：C2 三个子板的 `idf.py` 构建 / 烧录（实时日志）。
- Tab3「构建产物」：按板型分组展示所有编译产物 + 生成时间，可直接下载。
- 首次使用若报 `Cannot import module "click"` 等，先跑 `DEPLOY.md` 第 3 步安装 IDF，并核对 `server.js` 顶部 4 处路径。

### 手动编译（不依赖网页）

```bash
# C3（arduino-cli）
arduino-cli compile --fqbn esp32:esp32:esp32c3 \
  --build-property build.defines=-DBOARD_ESP32C3_SWITCH ESP32_Light_Switch

# ESP8285（arduino-cli，宏注入走 build.extra_flags）
arduino-cli compile --fqbn esp8266:esp8266:generic \
  --build-property build.extra_flags=-DBOARD_ESP8285_SWITCH ESP32_Light_Switch

# C2（ESP-IDF）
cd idf-c2 && . ~/esp/esp-idf/export.sh
IDF_C2_BOARD=BOARD_ESP32C2_SWITCH_NANO idf.py -B build/BOARD_ESP32C2_SWITCH_NANO build
```

## 上传步骤

1. 用 USB 线连接板子
2. 按住 BOOT 键，点按 RESET 键进入下载模式（部分自动下载电路可跳过）
3. 在 Arduino IDE 中选择正确的端口和开发板
4. 点击上传
5. 打开串口监视器（波特率 115200），确认 AP 信息输出

## 使用网页控制

1. 手机或电脑连接 Wi-Fi：`ESP-Light-Switch`
2. 密码：`12345678`
3. 浏览器访问：`http://192.168.4.1`
4. 使用页面控制开关、亮度、定时，数据每 2 秒自动刷新

## 网络模式（热点 + 局域网）

设备支持两种工作方式，开机时自动选择：

1. **首次使用（无 Wi-Fi 凭证）**：自动进入热点模式，建立 `ESP-Light-Switch` 热点，手机/电脑连上后用浏览器访问 `http://192.168.4.1` 完成配置。
2. **已配置 Wi-Fi 后**：开机尝试连接保存的 Wi-Fi（STA 模式，超时 20 秒）。连接成功后使用局域网 IP，之后都走 LAN；若连接失败则自动回退到热点，仍可访问。
3. **局域网访问地址**：连接成功后页面和串口会显示局域网 IP；也可直接用主机名 `http://esp-light-switch.local`（mDNS）访问。

### 配置局域网 Wi-Fi

在网页底部「网络」卡片中：

- 填写路由器 **Wi-Fi 名称（SSID）** 和 **密码**
- 点击「保存并连接 Wi-Fi」→ 设备重启并尝试连入局域网
- 若需要回到热点模式，点击「忘记 Wi-Fi，恢复热点模式」，设备会清除凭证并重启为热点

> 提示：在局域网模式下，若忘记 IP，可通过串口监视器（115200）查看，或使用 `esp-light-switch.local` 主机名访问。

## 固件升级（网页 OTA）

设备支持通过网页直接无线升级固件，省去每次接 USB 烧录：

1. 在网页底部「固件升级（OTA）」卡片点击「选择 .bin 并升级」，选中编译好的固件（`*.bin`）。
2. 等待上传完成（期间**切勿断电**），设备会自动写入 OTA 分区并重启。
3. 重启后即为新固件，原有配置（灯光、定时、Wi-Fi）通常保留（OTA 只更新程序，不影响 NVS）。

### 分区方案要求

OTA 需要 Flash 上有至少两个 App 分区（factory / ota_0 / ota_1）。

- **ESP32-C3（arduino 路径）**：工具 → Partition Scheme → 选择带 OTA 的方案，例如 `Default 4MB with spiffs`（含 OTA 双分区）。若选了「Minimal / No OTA」类方案，`Update.begin()` 会因空间不足而失败。
- **ESP32-C2（IDF 路径）**：无需配置 —— `idf-c2/partitions.csv` 已内置含 otadata 的 4MB 双 OTA 分区（factory/ota_0/ota_1 各 1.25MB），`sdkconfig.defaults` 锁定 4MB flash。
- **ESP8285（1MB Flash）**：OTA 空间很紧张。选 `Flash Size: 1M (128K SPIFFS)` 时通常**不带** OTA 分区；需选带 OTA 的 1MB 方案（如 `1M (512K+512K OTA)` 之类，具体取决于板包菜单）。1MB 下固件+网页字符串+Wi-Fi 栈已偏满，OTA 可能无法容纳较大固件；若 OTA 失败，请改用 USB 串口烧录。

> 安全提示：OTA 接口当前无任何鉴权，仅适合在受信任的局域网/热点下使用。若设备暴露在公网，请自行增加认证。

## 本地 BOOT 键用法

本板只有这一个物理按键，已复用为本地控制：

- **短按**（松开）：切换灯的开关。
- **长按 3 秒**（`RESET_HOLD_MS`，可在 `.ino` 顶部修改）：恢复出厂设置，清空所有设置（灯光状态、定时、Wi-Fi 凭证），并重启回到热点模式，等待重新配网。

> **ESP32-C2/C3**：BOOT 键在 **GPIO9**，与灯 PWM(GPIO4) 不冲突。注意 GPIO9 也是 strapping 脚，**上电/复位时按住会进入下载模式**（正常烧录用），此长按恢复出厂逻辑仅在启动完成后运行时生效，不会与烧录冲突。
>
> **ESP8285**：板载 BOOT/下载键通常是 **GPIO0**（ESP8266 的下载脚）。上电/复位时按住 GPIO0 进入下载模式，长按恢复出厂逻辑同样只在启动完成后运行时生效。默认的 `BUTTON_PIN` 在 ESP8266 分支里已设为 `0`。

## 电流检测校准

根据你的原理图，电流检测部分使用：

- 电流检测放大器 **U4: INA180A1IDBVR**，固定增益 `Gain = 20`
- 高侧分流电阻 **R83: 150 mΩ**，位于 VCC 与 USB 输出 VOUT 之间

公式：

```
电流(A) = ADC电压(V) / (Rshunt(Ω) × Gain)
```

按当前原理图计算：

```cpp
#define SHUNT_RESISTANCE_MILLIOHM  150.0f
#define CURRENT_SENSE_GAIN         20.0f
```

此时 1A 负载电流对应放大器输出约 3.0V（`1 × 0.15 × 20 = 3.0`），ESP32 ADC 满量程约 1.1A。

> **ESP8285 注意**：其 ADC 量程仅 **0~1.0V**（10 位），而 1A 时放大器输出已达 3.0V，会超出量程被削顶。ESP8285 上最大可测电流约 `1.0 / (0.15 × 20) ≈ 0.33A`。若你的 ESP8285 板电流检测输出超过 1.0V，请在放大器输出端加电阻分压，或减小分流电阻/增益，使满量程落在 1.0V 以内，并保持 `SHUNT_RESISTANCE_MILLIOHM` 与 `CURRENT_SENSE_GAIN` 与实际电路一致。

如果你的板子使用了不同的分流电阻或放大器型号，请按实际参数修改上述两个宏。

## 环境亮度显示说明

GPIO3 读取光敏分压电路电压。页面显示百分比：

- 100% = 最亮
- 0% = 最暗

如果你的光敏电阻接法相反（亮时电压高），请修改 `readAmbientPercent()` 函数中的映射关系。

## 定时开关说明

设备运行在 AP 模式下没有网络时间，因此使用“上电后经过的分钟数”来模拟一天 24 小时。

- 设置 07:00 开、23:00 关，设备会在上电 7 小时后开灯，23 小时后关灯。
- 若设置跨夜（例如 23:00 开、07:00 关），程序会自动处理跨天逻辑。
- 如需精确 RTC 时间，后续可扩展 DS3231 或 NTP（STA 模式）。

## 常见问题

**Q：上传后串口没有输出 AP IP**
A：检查波特率是否为 115200，开发板是否选错（C2 选成 C3 会无法启动）。

**Q：网页打不开**
A：确认手机已连接 `ESP-Light-Switch`，并输入 `http://192.168.4.1`（不是 https）。

**Q：亮度调节无效 / 网页没有亮度滑条**
A：ESP32-C2/C3 与 ESP8285 都支持亮度 PWM 调光（ESP8285 用 `analogWrite` 软件 PWM）。若网页仍无亮度滑条，确认对应产品板的 `BRIGHTNESS_SUPPORTED` 是否为 1；继电器类模块建议亮度保持 100%，中间值会令其抖动/蜂鸣。

**Q：电流显示不准**
A：根据实际分流电阻和放大器增益修改校准常量，必要时用万用表对比校准。

## 许可证

本项目采用 MIT 许可证，详见仓库根目录 [LICENSE](./LICENSE)。
