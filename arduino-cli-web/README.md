# arduino-cli-web

本地网页控制台：在本机起一个轻量 Node 服务，浏览器里点按钮就能 **编译 / 编译并上传 / 串口监视** ESP-Switch 固件，实时看日志。不用再手敲 arduino-cli 命令，也绕开了 Arduino IDE 的 `ECONNREFUSED` 问题。

> 纯 Node 内置模块实现，**零第三方依赖**。

## 前置条件

- 已安装 `arduino-cli`（预编译二进制即可，见下文）
- 已存在 `~/.arduino15/arduino-cli.yaml`，其 `directories.data` 指向 IDE 的包目录（这样能直接用已装的 `esp32:esp32 3.3.3`，无需重新下载）
- 固件 sketch 目录：`/Users/meizu/work/ESP-Switch/ESP32_Light_Switch`

### arduino-cli 安装（旧 macOS 注意）

`brew install arduino-cli` 在 macOS < Monterey 会因依赖 `go` 编译失败。改用官方预编译二进制：

```bash
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
sudo mv bin/arduino-cli /usr/local/bin/
arduino-cli version
```

复用 IDE 已装包的配置（`~/.arduino15/arduino-cli.yaml`）：

```yaml
directories:
  data: /Users/meizu/Library/Arduino15
  downloads: /Users/meizu/Library/Arduino15/staging
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

然后浏览器打开 **http://localhost:8787**

## 使用

1. **板型**：选 `esp32:esp32:esp32c2`（C2 板）或 `esp32:esp32:esp32c3`（C3 板）、`esp8266:esp8266:generic`（ESP8285 变体）。
2. **端口**：插好板子后点「刷新」拉取 `/dev/cu.*`；选对应端口（如 `/dev/cu.usbmodem144101`）。
3. **详细日志**：勾选后编译加 `-v`，会打出完整 g++ 命令。
4. **① 编译**：只编译，看是否通过。
5. **② 编译并上传**：编译并烧录到所选端口。C3 原生 USB 进下载模式：按住 BOOT 再点一下 EN，松开 BOOT。
6. **③ 开始监视 / ④ 停止监视**：打开串口监视（默认 115200），实时看设备日志；「停止监视」关闭。

## 接口说明

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/status` | CLI 路径/是否存在、配置与 sketch 目录状态 |
| GET | `/api/boards` | 可选板型列表 |
| GET | `/api/ports`  | 当前 `/dev/cu.*` 端口列表 |
| GET | `/api/stream/compile?fqbn=&verbose=` | SSE 流式编译 |
| GET | `/api/stream/upload?fqbn=&port=&verbose=` | SSE 流式编译+上传 |
| GET | `/api/stream/monitor?port=&baud=` | SSE 流式串口监视（关掉 EventSource 即停止） |

## 备注

- 服务仅监听本机 `localhost`，不上网、不暴露到公网。
- `fqbn` / `port` 都做了格式校验，避免命令注入。
- 若浏览器打不开 `localhost:8787`，可能是本机风控/零信任软件拦截了 loopback 连接（与 Arduino IDE 的 `ECONNREFUSED` 同源），需在该软件里放行本机 Node 进程的网络访问。
