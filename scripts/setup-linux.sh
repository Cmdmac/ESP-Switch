#!/usr/bin/env bash
# ============================================================
# ESP-Switch 环境搭建脚本（Linux / macOS）
# 目标：让 arduino-cli-web 网页控制台可用（C3 / ESP8285 / C2 编译）
# 特点：幂等（可重复执行，已安装的跳过）、交互可选大件下载
# 用法： bash setup-linux.sh
# ============================================================
set -u
# 不启用 set -e：逐项容错，某项失败继续后续检查

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  [OK]${NC} $*"; }
warn() { echo -e "${YELLOW}  [..]${NC} $*"; }
fail() { echo -e "${RED}  [!!]${NC} $*"; }

echo "=============================================="
echo " ESP-Switch 环境搭建 (Linux/macOS)"
echo "=============================================="

HAVE_APT=0; HAVE_DNF=0; HAVE_YUM=0; HAVE_BREW=0
command -v apt-get >/dev/null 2>&1 && HAVE_APT=1
command -v dnf     >/dev/null 2>&1 && HAVE_DNF=1
command -v yum     >/dev/null 2>&1 && HAVE_YUM=1
command -v brew    >/dev/null 2>&1 && HAVE_BREW=1

# ---------- 1. Node.js ----------
echo; echo "[1/5] Node.js"
if command -v node >/dev/null 2>&1; then
  NODE_VER=$(node --version 2>/dev/null)
  ok "Node.js 已安装: $NODE_VER"
  NODE_MAJOR=$(echo "$NODE_VER" | sed 's/^v//' | cut -d. -f1)
  if [ "${NODE_MAJOR:-0}" -lt 14 ] 2>/dev/null; then
    warn "Node.js 版本过旧（<14），arduino-cli-web 需要 v14+，建议升级"
  fi
else
  warn "未检测到 Node.js，尝试安装 ..."
  if command -v nvm >/dev/null 2>&1 || [ -s "$HOME/.nvm/nvm.sh" ]; then
    # shellcheck disable=SC1091
    [ -s "$HOME/.nvm/nvm.sh" ] && . "$HOME/.nvm/nvm.sh"
    nvm install --lts >/dev/null 2>&1 && nvm use --lts >/dev/null 2>&1
    command -v node >/dev/null 2>&1 && ok "Node.js 已通过 nvm 安装: $(node --version)" || fail "nvm 安装失败"
  elif command -v curl >/dev/null 2>&1; then
    echo "    使用 nvm 官方脚本安装（推荐）..."
    curl -fsSL https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.1/install.sh | bash
    # shellcheck disable=SC1091
    [ -s "$HOME/.nvm/nvm.sh" ] && . "$HOME/.nvm/nvm.sh"
    nvm install --lts >/dev/null 2>&1
    if command -v node >/dev/null 2>&1; then
      ok "Node.js 已通过 nvm 安装: $(node --version)（新终端生效）"
    else
      warn "nvm 安装未生效，请新开终端后重新运行本脚本，或手动安装 https://nodejs.org"
    fi
  elif [ $HAVE_APT -eq 1 ]; then
    sudo apt-get update -qq && sudo apt-get install -y -qq nodejs npm
    command -v node >/dev/null 2>&1 && ok "Node.js 已通过 apt 安装: $(node --version)（版本可能偏旧）" || fail "apt 安装失败"
  else
    fail "未找到安装方式，请手动安装 Node.js v14+：https://nodejs.org"
  fi
fi

# ---------- 2. arduino-cli ----------
echo; echo "[2/5] arduino-cli"
if command -v arduino-cli >/dev/null 2>&1; then
  ok "arduino-cli 已安装: $(arduino-cli version 2>/dev/null | head -1)"
else
  warn "未检测到 arduino-cli，使用官方脚本安装 ..."
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
    # 官方脚本装到 ~/bin 或 /usr/local/bin
    export PATH="$HOME/bin:$PATH"
    if command -v arduino-cli >/dev/null 2>&1; then
      ok "arduino-cli 已安装: $(arduino-cli version | head -1)"
      grep -q "$HOME/bin" "$HOME/.bashrc" 2>/dev/null || echo 'export PATH="$HOME/bin:$PATH"' >> "$HOME/.bashrc"
    else
      fail "安装失败，请手动安装：https://github.com/arduino/arduino-cli/releases"
    fi
  else
    fail "缺少 curl，请先安装 curl 后重试"
  fi
fi

# ---------- 3. arduino-cli 配置 ----------
echo; echo "[3/5] arduino-cli 配置 (~/.arduino15/arduino-cli.yaml)"
ARDUINO15="$HOME/.arduino15"
CLI_YAML="$ARDUINO15/arduino-cli.yaml"
if [ -f "$CLI_YAML" ]; then
  ok "配置已存在: $CLI_YAML"
else
  mkdir -p "$ARDUINO15"
  cat > "$CLI_YAML" <<EOF
directories:
  data: $ARDUINO15
  downloads: $ARDUINO15/staging
board_manager:
  additional_urls:
    - https://espressif.github.io/arduino-esp32/package_esp32_index.json
    - https://arduino.esp8266.com/stable/package_esp8266com_index.json
EOF
  ok "配置已生成: $CLI_YAML"
fi

# ---------- 4. 可选：安装核心包（大下载） ----------
echo; echo "[4/5] Arduino 核心包 (esp32 / esp8266，可选，约 1GB+ 下载)"
read -r -p "    现在安装核心包？[y/N]: " INSTALL_CORES
if [ "$INSTALL_CORES" = "y" ] || [ "$INSTALL_CORES" = "Y" ]; then
  if command -v arduino-cli >/dev/null 2>&1; then
    # 已安装的核心直接跳过，避免重复下载
    CORE_LIST=$(arduino-cli --config-file "$CLI_YAML" core list 2>/dev/null)
    NEED=""
    echo "$CORE_LIST" | grep -q "esp32:esp32 "        || NEED="$NEED esp32:esp32"
    echo "$CORE_LIST" | grep -q "esp8266:esp8266"     || NEED="$NEED esp8266:esp8266"
    if [ -z "$NEED" ]; then
      ok "esp32:esp32 与 esp8266:esp8266 均已安装，跳过"
    else
      echo "    更新核心索引 ..."
      arduino-cli --config-file "$CLI_YAML" core update-index || warn "索引更新失败（可能网络问题）"
      echo "    安装缺失核心:$NEED（耗时较长）..."
      arduino-cli --config-file "$CLI_YAML" core install $NEED \
        && ok "核心包安装完成" || warn "核心包安装失败，可稍后手动执行: arduino-cli core install esp32:esp32 esp8266:esp8266"
    fi
  else
    warn "arduino-cli 尚不可用，跳过核心包安装"
  fi
else
  warn "跳过核心包安装。之后可重跑本脚本选择 y，或手动执行 arduino-cli core install"
fi

# ---------- 5. 可选：ESP-IDF（C2 编译需要，约 3GB） ----------
echo; echo "[5/5] ESP-IDF（ESP32-C2 编译需要，可选，约 3GB）"
IDF_HOME=""
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
  IDF_HOME="$IDF_PATH"
elif [ -d "$HOME/esp/esp-idf" ] && [ -f "$HOME/esp/esp-idf/export.sh" ]; then
  IDF_HOME="$HOME/esp/esp-idf"
elif [ -d "/opt/esp-idf" ] && [ -f "/opt/esp-idf/export.sh" ]; then
  IDF_HOME="/opt/esp-idf"
fi
if [ -n "$IDF_HOME" ]; then
  ok "ESP-IDF 已安装: $IDF_HOME"
  echo "    提示：编译 C2 时设置 export IDF_PATH=$IDF_HOME 即可被自动探测"
else
  warn "未检测到 ESP-IDF"
  read -r -p "    是否现在安装到 ~/esp/esp-idf（v5.5.2，支持 esp32c2）？[y/N]: " INSTALL_IDF
  if [ "$INSTALL_IDF" = "y" ] || [ "$INSTALL_IDF" = "Y" ]; then
    if command -v git >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
      mkdir -p "$HOME/esp" && cd "$HOME/esp"
      git clone --recursive -b v5.5.2 https://github.com/espressif/esp-idf.git esp-idf \
        && cd esp-idf \
        && ./install.sh esp32c2 \
        && ok "ESP-IDF v5.5.2 安装完成（~/.espressif 工具链）"
      echo "    使用前先 source ~/esp/esp-idf/export.sh"
    else
      fail "需要 git 与 python3（>=3.8），请先安装后重试；或按官方文档安装: https://docs.espressif.com/projects/esp-idf/"
    fi
  else
    warn "跳过 ESP-IDF。C2 编译将不可用，其余板型不受影响"
  fi
fi

# ---------- 串口权限提示 ----------
echo; echo "--- 串口权限（Linux 常见问题）---"
if [ "$(uname)" = "Linux" ]; then
  if ! groups | grep -q dialout; then
    warn "当前用户不在 dialout 组，可能无法访问串口设备。执行："
    echo "    sudo usermod -aG dialout \$USER    （然后重新登录）"
  else
    ok "已在 dialout 组"
  fi
fi

# ---------- 汇总 ----------
echo; echo "=============================================="
echo " 环境汇总"
echo "=============================================="
node --version      >/dev/null 2>&1 && echo "  node        : $(node --version)"       || echo "  node        : 缺失"
arduino-cli version >/dev/null 2>&1 && echo "  arduino-cli : $(arduino-cli version | head -1)" || echo "  arduino-cli : 缺失（C3/8285 编译不可用）"
[ -n "${IDF_HOME:-}" ] && echo "  esp-idf     : $IDF_HOME" || echo "  esp-idf     : 未安装（C2 编译不可用）"
[ -f "$CLI_YAML" ] && echo "  config      : $CLI_YAML" || echo "  config      : 缺失"
echo
echo " 启动网页控制台："
echo "   cd arduino-cli-web && ./start.sh"
echo "   或 bash setup-linux.sh 后执行 node arduino-cli-web/server.js"
echo "   （本脚本所在目录的上一级即仓库根）"
echo " 浏览器打开 http://localhost:8787"
echo "=============================================="
