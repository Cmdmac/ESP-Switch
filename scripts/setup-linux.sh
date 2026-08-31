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

# sudo 前置处理：需要密码时先提示，避免用户以为脚本卡死（密码不回显）
SUDO=""
ensure_sudo() {
  if [ "$(id -u)" -eq 0 ]; then SUDO=""; return 0; fi
  if ! command -v sudo >/dev/null 2>&1; then return 1; fi
  if sudo -n true 2>/dev/null; then SUDO="sudo"; return 0; fi
  echo -e "${YELLOW}  需要 sudo 权限安装依赖，请输入密码（输入时不显示，回车继续）${NC}"
  if sudo -v; then SUDO="sudo"; return 0; fi
  return 1
}

# 下载工具：优先 curl，其次 wget；都没有则自动装一个
ensure_downloader() {
  command -v curl >/dev/null 2>&1 && { DL="curl"; return 0; }
  command -v wget >/dev/null 2>&1 && { DL="wget"; return 0; }
  warn "未检测到 curl / wget，尝试自动安装 ..."
  if ! ensure_sudo; then fail "无法获取 sudo 权限，请手动安装 curl 后重试：sudo apt install curl"; return 1; fi
  if [ $HAVE_APT -eq 1 ]; then
    $SUDO apt-get update -qq && $SUDO apt-get install -y -qq curl && { DL="curl"; return 0; }
  elif [ $HAVE_DNF -eq 1 ]; then
    $SUDO dnf install -y curl && { DL="curl"; return 0; }
  elif [ $HAVE_YUM -eq 1 ]; then
    $SUDO yum install -y curl && { DL="curl"; return 0; }
  elif [ $HAVE_BREW -eq 1 ]; then
    brew install curl && { DL="curl"; return 0; }
  fi
  return 1
}

# 用已就绪的下载工具取文件：$1=URL $2=输出路径
fetch() {
  if [ "${DL:-}" = "curl" ]; then
    curl -fsSL "$1" -o "$2"
  else
    wget -qO "$2" "$1"
  fi
}

# 环境变量持久化：写入 ~/.bashrc 与 ~/.zshrc（若存在），并立即在当前会话生效，
# 这样装完工具不用重开命令行。
# 用法： persist_env 'export PATH="$HOME/bin:$PATH"'
persist_env() {
  local line="$1"
  local rc
  for rc in "$HOME/.bashrc" "$HOME/.zshrc"; do
    [ -f "$rc" ] || continue
    grep -qF "$line" "$rc" 2>/dev/null || echo "$line" >> "$rc"
  done
  eval "$line"
}

# 深度探测 node：command -v 找不到时，扫描常见安装位置（nvm / brew / apt / 版本管理器）
find_node() {
  command -v node >/dev/null 2>&1 && return 0
  local cand
  # nvm 已装版本（取最新）
  for cand in "$HOME"/.nvm/versions/node/*/bin; do
    [ -d "$cand" ] && [ -x "$cand/node" ] || continue
    export PATH="$cand:$PATH"
    return 0
  done
  # brew / 系统路径
  for cand in /opt/homebrew/bin /usr/local/bin /usr/bin; do
    [ -x "$cand/node" ] || continue
    export PATH="$cand:$PATH"
    return 0
  done
  # WorkBuddy 管理运行时（桌面端自带）
  for cand in "$HOME"/.workbuddy/binaries/node/versions/*/bin; do
    [ -d "$cand" ] && [ -x "$cand/node" ] || continue
    export PATH="$cand:$PATH"
    return 0
  done
  # fnm / volta / asdf 等其他版本管理器
  for cand in "$HOME"/.local/share/fnm/node-versions/*/installation/bin "$HOME"/.volta/bin "$HOME"/.asdf/shims; do
    [ -d "$cand" ] && [ -x "$cand/node" ] && { export PATH="$cand:$PATH"; return 0; }
  done
  return 1
}

# ---------- 1. Node.js ----------
echo; echo "[1/5] Node.js"
if find_node; then
  NODE_VER=$(node --version 2>/dev/null)
  ok "Node.js 已安装: $NODE_VER  ($(command -v node))"
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
    if find_node; then
      ok "Node.js 已通过 nvm 安装: $(node --version)"
      persist_env 'export NVM_DIR="$HOME/.nvm"'
      persist_env '[ -s "$NVM_DIR/nvm.sh" ] && . "$NVM_DIR/nvm.sh"'
    else
      fail "nvm 安装失败"
    fi
  elif ensure_downloader; then
    echo "    使用 nvm 官方脚本安装（推荐）..."
    fetch https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.1/install.sh /tmp/nvm_install.sh && bash /tmp/nvm_install.sh
    # shellcheck disable=SC1091
    [ -s "$HOME/.nvm/nvm.sh" ] && . "$HOME/.nvm/nvm.sh"
    nvm install --lts >/dev/null 2>&1 && nvm use --lts >/dev/null 2>&1
    if find_node; then
      ok "Node.js 已通过 nvm 安装: $(node --version)（已写入 .bashrc/.zshrc，当前会话即刻生效）"
      persist_env 'export NVM_DIR="$HOME/.nvm"'
      persist_env '[ -s "$NVM_DIR/nvm.sh" ] && . "$NVM_DIR/nvm.sh"'
    else
      fail "nvm 安装未生效，请新开终端后重新运行本脚本，或手动安装 https://nodejs.org"
    fi
  elif [ $HAVE_APT -eq 1 ]; then
    sudo apt-get update -qq && sudo apt-get install -y -qq nodejs npm
    if find_node; then
      ok "Node.js 已通过 apt 安装: $(node --version)（版本可能偏旧）"
    else
      fail "apt 安装失败"
    fi
  else
    fail "未找到安装方式，请手动安装 Node.js v14+：https://nodejs.org"
  fi
fi

# ---------- 2. ESP-IDF（C2 编译需要）：扫描已有环境 -> 多选 -> 写入环境变量 ----------
echo; echo "[2/5] ESP-IDF（ESP32-C2 编译需要，可选，约 3GB）"

# 收集所有有效的 ESP-IDF（目录内含 export.sh 即视为有效）
IDF_FOUND=()
IDF_COUNT=0
collect_idf() {  # $1=候选目录
  [ -n "${1:-}" ] || return
  [ -d "$1" ] && [ -f "$1/export.sh" ] || return
  # 排除作为其他框架子模块的 IDF（如 ~/.espressif/esp-adf/esp-idf、esp-ai 等）
  case "$1" in
    *"/esp-adf/esp-idf"|*"/esp-ai/esp-idf"|*"/esp-rainmaker/esp-idf") return ;;
  esac
  # 去重（空数组 + set -u 需用 ${arr[@]+...} 防御展开）
  for e in ${IDF_FOUND[@]+"${IDF_FOUND[@]}"}; do [ "$e" = "$1" ] && return; done
  IDF_FOUND+=("$1")
  IDF_COUNT=$((IDF_COUNT+1))
}

# ---------- 2a. 兼容 Python 与 IDF venv 检查（仅检测，不自动安装）----------
# 重要：本脚本【不会】自动安装 python、也不会自动重建 venv。用户已自行管理工具链，
# 这里只检测现状并给出手动修复命令，由用户决定是否执行。
# 背景：Ubuntu 26.04 自带 python3.14，IDF 5.5 下不可用（依赖 cryptography<45 无 cp314 预编译
# wheel 且 --only-binary 禁源码编译），需用户手动装 3.9+（v5.5 官方下限）并建 venv。
# 注意：不含 3.8 —— v5.5 的 python_version_checker.py 下限是 3.9（5.3/5.4 才是 3.8）。

# 在 PATH 上探测已装兼容版本，输出 pythonX.Y 或空（3.12 优先：发行版仓库更易获得）
detect_compat_python() {
  for v in 3.12 3.13 3.11 3.10 3.9; do
    if command -v "python$v" >/dev/null 2>&1; then
      echo "python$v"; return 0
    fi
  done
  return 1
}

# 检测 IDF venv 是否已就绪（存在一个能 import click 的 python3），输出 venv 目录或空
check_idf_venv() {
  local penv="$HOME/.espressif/python_env" d
  [ -d "$penv" ] || return 1
  for d in "$penv"/*; do
    [ -x "$d/bin/python3" ] || continue
    if "$d/bin/python3" -c "import click" >/dev/null 2>&1; then echo "$d"; return 0; fi
  done
  return 1
}

# 仅检测并报告 IDF python 环境，不自动安装/重建（用户自行管理工具链）。
report_idf_python_and_venv() {
  [ -n "${1:-}" ] || return 0
  local py venv
  py=$(detect_compat_python)
  if [ -n "$py" ]; then
    ok "检测到兼容 python: $py"
  else
    warn "未检测到兼容 python（需 3.9+）；Ubuntu 26.04 自带 python3.14 在 IDF 5.5 下不可用"
    echo "    请手动安装兼容 python 后重跑本脚本（本脚本不会代你安装），例如："
    echo "      sudo apt install python3.12 python3.12-venv     （若仓库无旧版本：uv python install 3.12）"
  fi
  venv=$(check_idf_venv)
  if [ -n "$venv" ]; then
    ok "IDF python venv 已就绪 ($(basename "$venv"))"
  else
    warn "IDF python venv 未就绪（缺失或 click 不可用）"
    if [ -n "$py" ]; then
      echo "    可用现有 $py 手动建 venv： cd $1 && $py tools/idf_tools.py install_python_env"
    else
      echo "    请先安装兼容 python，再执行： cd $1 && python3.12 tools/idf_tools.py install_python_env"
    fi
  fi
}

# 优先：环境变量 IDF_PATH
collect_idf "${IDF_PATH:-}"
# 常见布局：~/esp/esp-idf、~/esp/<版本>/esp-idf、/opt/esp-idf
for cand in "$HOME"/esp/esp-idf "$HOME"/esp/*/esp-idf /opt/esp-idf; do
  collect_idf "$cand"
done
# eim / 官方安装器布局：~/.espressif/<版本>/esp-idf（Linux 与 macOS 一致）
for cand in "$HOME"/.espressif/*/esp-idf "$HOME"/.espressif/esp-idf; do
  collect_idf "$cand"
done
# 当前 shell 已 source 过 IDF（idf.py 在 PATH 且能定位到 export.sh）
if command -v idf.py >/dev/null 2>&1; then
  IDF_PY_REAL=$(readlink -f "$(command -v idf.py)" 2>/dev/null || echo "$(command -v idf.py)")
  collect_idf "$(dirname "$(dirname "$IDF_PY_REAL")")"
fi

if [ "$IDF_COUNT" -gt 0 ]; then
  if [ "$IDF_COUNT" -eq 1 ]; then
    IDF_HOME="${IDF_FOUND[0]}"
    ok "检测到 1 个 ESP-IDF: $IDF_HOME"
  else
    echo "    检测到 $IDF_COUNT 个 ESP-IDF 环境，请选择要使用的："
    i=1
    for e in ${IDF_FOUND[@]+"${IDF_FOUND[@]}"}; do
      ver=""
      if [ -f "$e/tools/cmake/version.cmake" ]; then
        # macOS BSD grep 无 -oP，用 grep -Eo 兼容
        ver=$(grep -Eo 'set\(IDF_VERSION_(MAJOR|MINOR|PATCH) +[0-9]+\)' "$e/tools/cmake/version.cmake" 2>/dev/null | grep -Eo '[0-9]+' | paste -sd. -)
      fi
      echo "      [$i] $e${ver:+  (v$ver)}"
      i=$((i+1))
    done
    while :; do
      read -r -p "    输入序号 [1-$IDF_COUNT]: " IDF_CHOICE
      if [ "$IDF_CHOICE" -ge 1 ] 2>/dev/null && [ "$IDF_CHOICE" -le "$IDF_COUNT" ] 2>/dev/null; then
        IDF_HOME="${IDF_FOUND[$((IDF_CHOICE-1))]}"
        break
      fi
      echo "    无效序号，请重新输入"
    done
    ok "已选择: $IDF_HOME"
  fi
  # 写入环境变量：让 arduino-cli-web 的 server.js 自动探测到（ESP_SWITCH_IDF_DIR 优先级最高）
  # 用 persist_env 同时写 .bashrc/.zshrc 并当前会话立即生效，避免重开终端
  persist_env "export ESP_SWITCH_IDF_DIR=\"$IDF_HOME\""
  ok "已写入环境变量 ESP_SWITCH_IDF_DIR=$IDF_HOME (已加入 ~/.bashrc 与 ~/.zshrc，当前及新终端均生效)"
  # 检查 IDF 的 python 环境（仅检测，不自动安装/重建 venv）
  report_idf_python_and_venv "$IDF_HOME"
else
  warn "未检测到 ESP-IDF"
  read -r -p "    是否现在安装到 ~/esp/esp-idf（v5.5.2，支持 esp32c2）？[y/N]: " INSTALL_IDF
  if [ "$INSTALL_IDF" = "y" ] || [ "$INSTALL_IDF" = "Y" ]; then
    if command -v git >/dev/null 2>&1; then
      idfpy=$(detect_compat_python)
      if [ -z "$idfpy" ]; then
        fail "缺少兼容 python(需 3.9+，v5.5 官方下限)，无法安装 IDF 工具链。请先手动安装： sudo apt install python3.12 python3.12-venv"
      else
        mkdir -p "$HOME/esp" && cd "$HOME/esp"
        git clone --recursive -b v5.5.2 https://github.com/espressif/esp-idf.git esp-idf \
          && cd esp-idf \
          && { echo "    使用 $idfpy 安装工具链..."; "$idfpy" tools/idf_tools.py install esp32c2; } \
          && ok "ESP-IDF v5.5.2 工具链安装完成（~/.espressif）"
        IDF_HOME="$HOME/esp/esp-idf"
        persist_env "export ESP_SWITCH_IDF_DIR=\"$IDF_HOME\""
        # 检查 python 环境（仅检测，不自动安装/重建 venv）
        report_idf_python_and_venv "$IDF_HOME"
        echo "    使用前先 source ~/esp/esp-idf/export.sh"
      fi
    else
      fail "需要 git 与 python3（>=3.9，IDF 5.5 下限；5.3/5.4 为 3.8），请先安装后重试；或按官方文档安装: https://docs.espressif.com/projects/esp-idf/"
    fi
  else
    warn "跳过 ESP-IDF。C2 编译将不可用，其余板型不受影响"
  fi
fi

# ---------- 3. arduino-cli ----------
echo; echo "[3/5] arduino-cli"
if command -v arduino-cli >/dev/null 2>&1; then
  ok "arduino-cli 已安装: $(arduino-cli version 2>/dev/null | head -1)"
else
  warn "未检测到 arduino-cli，开始安装 ..."
  if ensure_downloader; then
    # 优先官方 install.sh（需要 curl）；失败或只有 wget 时直下 release 二进制
    if command -v curl >/dev/null 2>&1; then
      curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
      export PATH="$HOME/bin:$PATH"
    fi
    if ! command -v arduino-cli >/dev/null 2>&1; then
      echo "    直接下载 arduino-cli 二进制 ..."
      mkdir -p "$HOME/bin"
      fetch "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz" /tmp/arduino-cli.tar.gz \
        && tar -xzf /tmp/arduino-cli.tar.gz -C "$HOME/bin" arduino-cli \
        && chmod +x "$HOME/bin/arduino-cli" \
        && export PATH="$HOME/bin:$PATH"
    fi
    if command -v arduino-cli >/dev/null 2>&1; then
      ok "arduino-cli 已安装: $(arduino-cli version | head -1)"
      persist_env 'export PATH="$HOME/bin:$PATH"'
    else
      fail "安装失败，请手动安装：https://github.com/arduino/arduino-cli/releases"
    fi
  else
    fail "缺少 curl 且无法自动安装，请手动执行 sudo apt install curl 后重试"
  fi
fi

# ---------- 4. arduino-cli 配置 ----------
echo; echo "[4/5] arduino-cli 配置 (~/.arduino15/arduino-cli.yaml)"
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

# ---------- 5. 可选：安装核心包（大下载） ----------
echo; echo "[5/5] Arduino 核心包 (esp32 / esp8266，可选，约 1GB+ 下载)"
# IDF 已在 [2/5] 检测并写入 IDF_HOME，这里直接复用结果做提示
if [ -n "${IDF_HOME:-}" ]; then
  echo "    检测到 ESP-IDF 环境: $IDF_HOME"
  echo "      C2 编译走 IDF，无需 arduino 的 esp32c2-libs；"
  echo "      此处仍会安装 esp32 核心包（C3 编译必需），esp8266 核心包（8285 必需）。"
fi
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
      echo "    安装缺失核心: $NEED (耗时较长)..."
      arduino-cli --config-file "$CLI_YAML" core install $NEED \
        && ok "核心包安装完成" || warn "核心包安装失败，可稍后手动执行: arduino-cli core install esp32:esp32 esp8266:esp8266"
    fi
  else
    warn "arduino-cli 尚不可用，跳过核心包安装"
  fi
else
  warn "跳过核心包安装。之后可重跑本脚本选择 y，或手动执行 arduino-cli core install"
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
[ -n "${ESP_SWITCH_IDF_DIR:-}" ] && echo "  环境变量    : ESP_SWITCH_IDF_DIR=$ESP_SWITCH_IDF_DIR" || echo "  环境变量    : ESP_SWITCH_IDF_DIR 未设置（server.js 将回退自动探测）"
[ -f "$CLI_YAML" ] && echo "  config      : $CLI_YAML" || echo "  config      : 缺失"
echo
echo " 启动网页控制台："
echo "   cd arduino-cli-web && ./start.sh"
echo "   或 bash setup-linux.sh 后执行 node arduino-cli-web/server.js"
echo "   （本脚本所在目录的上一级即仓库根）"
echo " 浏览器打开 http://localhost:8787"
echo "=============================================="
