# arduino-cli-web

本地网页控制台：在本机起一个轻量 Node 服务，浏览器里点按钮就能 **编译 / 编译并上传 / 串口监视** ESP-Switch 固件，实时看日志。不用再手敲 arduino-cli 命令，也绕开了 Arduino IDE 的 `ECONNREFUSED` 问题。

> 纯 Node 内置模块实现，**零第三方依赖**，Windows / macOS / Linux 通用。

## 前置条件

- **Node.js**（v14+，任意平台）
- **arduino-cli**（编译 C3/ESP8285 需要；C2 走本机 ESP-IDF 工具链，不需要 arduino-cli）
- 可选：`~/.arduino15/arduino-cli.yaml` 配置（`directories.data` 指向 arduino 包目录，避免重复下载核心）

## 环境自动探测（无需任何环境变量）

启动时自动解析以下路径，跨平台兼容常见安装布局：

| 项 | 探测顺序 |
|----|----------|
| 固件 sketch 目录 | `ESP_SWITCH_SKETCH_DIR` → 仓库内联定位（`<仓库>/ESP32_Light_Switch`） |
| idf-c2 工程目录 | `ESP_SWITCH_IDF_C2_DIR` → 仓库内联定位（`<仓库>/idf-c2`） |
| ESP-IDF 目录 | `ESP_SWITCH_IDF_DIR` → `IDF_PATH` → 自动扫描（`$IDF_TOOLS_PATH/frameworks`、`~/esp` 等，取版本最高） |
| IDF Python venv | `IDF_PYTHON_ENV_PATH` → `$IDF_TOOLS_PATH/python_env` 或 `~/.espressif/python_env` 内按版本前缀匹配 |
| arduino-cli | `PATH`（`where`/`which`）→ 各平台常见安装位置（`%LOCALAPPDATA%\Programs`、`~/.arduino15/bin`、`/usr/local/bin` 等） |
| 编译产物目录 | `ESP_SWITCH_BUILD_BASE` → 系统临时目录（Windows `%TEMP%`，macOS/Linux `/tmp`） |
| arduino-cli 配置 | `ESP_SWITCH_ARDUINO_CLI_YAML` → `~/.arduino15/arduino-cli.yaml` |

需要覆盖默认行为时，启动前设对应环境变量即可，无需改代码。

### arduino-cli 安装

- **Windows**：`winget install Arduino.arduino-cli`，或从 [GitHub Releases](https://github.com/arduino/arduino-cli/releases) 下载 exe 放入 PATH
- **macOS**：`brew install arduino-cli`（旧系统用官方预编译二进制：`curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh`）
- **Linux**：`curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh`

复用 IDE 已装包的配置（`~/.arduino15/arduino-cli.yaml`）：

```yaml
directories:
  data: /Users/<you>/Library/Arduino15          # macOS
  # data: C:\Users\<you>\AppData\Local\Arduino15  # Windows
  downloads: <同 data 目录>/staging
board_manager:
  additional_urls:
    - https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

## 启动

```bash
cd arduino-cli-web
node server.js
# 或指定端口： PORT=9000 node server.js
```

或直接双击/运行启动脚本：**Windows** `start.bat`，**macOS/Linux** `./start.sh`。

然后浏览器打开 **http://localhost:8787**

## 使用

1. **板型**：选 `esp32:esp32:esp32c3`（C3 板）、`esp8266:esp8266:generic`（ESP8285 变体）；C2 板在「IDF 构建」页选。
2. **端口**：插好板子后点「刷新」拉取端口列表（Windows 为 `COMx`，macOS/Linux 为 `/dev/cu.*`）。
3. **详细日志**：勾选后编译加 `-v`，会打出完整 g++ 命令。
4. **① 编译**：只编译，看是否通过。
5. **② 编译并上传**：编译并烧录到所选端口。C3 原生 USB 进下载模式：按住 BOOT 再点一下 EN，松开 BOOT。
6. **③ 开始监视 / ④ 停止监视**：打开串口监视（默认 115200），实时看设备日志；「停止监视」关闭。

## 接口说明

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/status` | CLI 路径/是否存在、配置与 sketch 目录状态 |
| GET | `/api/boards` | 可选板型列表（C3/ESP8285） |
| GET | `/api/ports`  | 当前串口列表（Windows `COMx`，macOS/Linux `/dev/cu.*`） |
| GET | `/api/stream/compile?fqbn=&verbose=` | SSE 流式编译 |
| GET | `/api/stream/upload?fqbn=&port=&verbose=` | SSE 流式编译+上传 |
| GET | `/api/stream/monitor?port=&baud=` | SSE 流式串口监视（关掉 EventSource 即停止） |
| GET | `/api/stream/idfbuild?board=&action=&port=` | SSE 流式 ESP-IDF 构建/烧录（C2） |
| GET | `/api/artifacts` | 聚合各板型编译产物 |
| GET | `/api/download` `/api/idf/download` | 下载固件 .bin |

## 备注

- 服务仅监听本机 `localhost`，不上网、不暴露到公网。
- `fqbn` / `port` 都做了格式校验，避免命令注入。
- C2 构建在 Windows 走 PowerShell（`export.ps1`）、macOS/Linux 走 bash（`export.sh`），ESP-IDF 官方仅支持这两种 shell。
- 若浏览器打不开 `localhost:8787`，可能是本机风控/零信任软件拦截了 loopback 连接（与 Arduino IDE 的 `ECONNREFUSED` 同源），需在该软件里放行本机 Node 进程的网络访问。
