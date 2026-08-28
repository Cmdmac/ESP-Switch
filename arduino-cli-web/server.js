#!/usr/bin/env node
'use strict';

/**
 * arduino-cli-web —— 本地网页包装 arduino-cli
 *
 * 不依赖任何第三方包，仅用 Node 内置模块。
 * 通过 child_process.spawn 调用 arduino-cli，并用 SSE 把编译/上传/监视的
 * 实时输出推送到浏览器。
 *
 * 启动： node server.js     然后浏览器打开 http://localhost:8787
 */

const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, exec, execSync } = require('child_process');

const PORT = process.env.PORT || 8787;
const isWin = () => process.platform === 'win32';

// 依次返回第一个存在的路径；都不存在返回 null
function firstExisting(cands) {
  for (const c of cands) {
    try { if (c && fs.existsSync(c)) return c; } catch (_) {}
  }
  return null;
}

// ---- 路径解析：环境变量优先 -> 跨平台自动探测 -> 仓库内联定位 ----
// 仓库内与 server.js 的相对位置（无论克隆到哪台机器哪个目录都成立）
const REPO_ROOT = path.resolve(__dirname, '..');
const PUBLIC_DIR = path.join(__dirname, 'public');
const SKETCH_DIR = process.env.ESP_SWITCH_SKETCH_DIR || path.join(REPO_ROOT, 'ESP32_Light_Switch');
const IDF_C2_DIR = process.env.ESP_SWITCH_IDF_C2_DIR || path.join(REPO_ROOT, 'idf-c2');
// 编译产物目录：放系统临时目录（Windows: %TEMP%，macOS/Linux: /tmp），按 fqbn 分子目录
const BUILD_BASE = process.env.ESP_SWITCH_BUILD_BASE || path.join(os.tmpdir(), 'espbuild');
// arduino-cli 配置文件（Windows 与 macOS/Linux 的 home 定位统一走 os.homedir()）
const CONFIG_FILE = process.env.ESP_SWITCH_ARDUINO_CLI_YAML || path.join(os.homedir(), '.arduino15', 'arduino-cli.yaml');

// ---- ESP-IDF 环境（用于编译 ESP32-C2，复用本机 IDF + arduino-esp32 作组件）----
// IDF_DIR 解析优先级：ESP_SWITCH_IDF_DIR（显式覆盖）> 环境变量 IDF_PATH > 自动探测常见安装布局。
function findIdfDir() {
  if (process.env.ESP_SWITCH_IDF_DIR) return process.env.ESP_SWITCH_IDF_DIR;
  const marker = isWin() ? 'export.bat' : 'export.sh';
  if (process.env.IDF_PATH && fs.existsSync(path.join(process.env.IDF_PATH, marker))) return process.env.IDF_PATH;
  // 常见安装根：IDF_TOOLS_PATH/frameworks（官方安装器）、~/.espressif（eim 布局）、
  // ~/esp、~/esp-idf、/opt
  const roots = [
    process.env.IDF_TOOLS_PATH && path.join(process.env.IDF_TOOLS_PATH, 'frameworks'),
    path.join(os.homedir(), '.espressif'),
    path.join(os.homedir(), 'esp'),
    path.join(os.homedir(), 'esp-idf'),
    '/opt',
  ].filter(Boolean);
  const found = [];
  for (const root of roots) {
    let names;
    try { names = fs.readdirSync(root); } catch (_) { continue; }
    for (const n of names) {
      const cand = path.join(root, n);
      // 兼容多种布局：<root>/esp-idf、<root>/v5.5/esp-idf、<root>/v5.5.2/esp-idf、<root>/esp-idf-v5.5.2
      for (const c of [cand, path.join(cand, 'esp-idf')]) {
        if (!fs.existsSync(path.join(c, marker))) continue;
        const m = (c.match(/v?(\d+)\.(\d+)\.(\d+)/) || []);
        found.push({ dir: c, ver: m.length ? [Number(m[1]), Number(m[2]), Number(m[3])] : [0, 0, 0] });
      }
    }
  }
  if (!found.length) return null;
  found.sort((a, b) => (b.ver[0] - a.ver[0]) || (b.ver[1] - a.ver[1]) || (b.ver[2] - a.ver[2]));
  return found[0].dir;
}
const IDF_DIR = findIdfDir() || process.env.IDF_PATH || path.join(os.homedir(), 'esp', 'esp-idf');
// Windows 用 export.ps1（PowerShell），macOS/Linux 用 export.sh（bash）
const IDF_EXPORT = isWin() ? path.join(IDF_DIR, 'export.ps1') : path.join(IDF_DIR, 'export.sh');

// IDF 的 python venv（含 click）：显式锁定，避免 export 在 node 进程 PATH 下
// 探测到无 click 的 python（如 miniconda）而挂错 venv（症状：Cannot import module "click"）。
// 解析优先级：
//   1) 环境变量 IDF_PYTHON_ENV_PATH（source 过 IDF 的 shell 已设好，直接用）
//   2) 从 IDF_DIR 的 version.cmake 推导前缀（idf5.3 / idf5.4 …），再在候选 python_env 根下探测
//   3) ESP_SWITCH_IDF_VENV_PREFIX 显式覆盖前缀（跨机 IDF 版本特殊时用）
function findIdfVenv(idfDir) {
  if (process.env.IDF_PYTHON_ENV_PATH) return process.env.IDF_PYTHON_ENV_PATH;
  let prefix = process.env.ESP_SWITCH_IDF_VENV_PREFIX;
  if (!prefix) {
    try {
      const v = fs.readFileSync(path.join(idfDir, 'tools', 'cmake', 'version.cmake'), 'utf8');
      const mj = (v.match(/set\(IDF_VERSION_MAJOR\s+(\d+)\)/) || [])[1];
      const mn = (v.match(/set\(IDF_VERSION_MINOR\s+(\d+)\)/) || [])[1];
      if (mj && mn) prefix = 'idf' + mj + '.' + mn;
    } catch (_) { /* fallthrough */ }
  }
  prefix = prefix || 'idf5.3';
  // 候选 python_env 根：IDF_TOOLS_PATH/python_env（官方安装器）、~/.espressif/python_env
  const roots = [
    process.env.IDF_TOOLS_PATH && path.join(process.env.IDF_TOOLS_PATH, 'python_env'),
    path.join(os.homedir(), '.espressif', 'python_env'),
  ].filter(Boolean);
  for (const root of roots) {
    try {
      const cands = fs.readdirSync(root)
        .filter(d => d.startsWith(prefix + '_py') && d.endsWith('_env'))
        .sort();
      const pick = cands.find(d => d.includes('py3.13')) || cands[cands.length - 1];
      if (pick) return path.join(root, pick);
    } catch (_) { /* fallthrough */ }
  }
  const fallbackRoot = roots[0] || path.join(os.homedir(), '.espressif', 'python_env');
  return path.join(fallbackRoot, prefix + '_py3.13_env');
}
const IDF_VENV = findIdfVenv(IDF_DIR);
// Windows venv 的可执行目录是 Scripts，macOS/Linux 是 bin
const IDF_VENV_BIN = firstExisting([path.join(IDF_VENV, 'Scripts'), path.join(IDF_VENV, 'bin')]) || path.join(IDF_VENV, 'bin');
const IDF_VENV_PY = path.join(IDF_VENV_BIN, isWin() ? 'python.exe' : 'python3');

// 统一拼一条「初始化 IDF 环境后执行命令」的 shell（按平台选 PowerShell / bash）。
// 显式锁定 IDF_PATH + IDF_PYTHON_ENV_PATH + venv bin 前置，再 source export：
// 防止 node 进程继承的 PATH/残留变量导致探测到无 click 的 python（症状 "Cannot import module click"）。
function idfShell(innerCmd) {
  if (isWin()) {
    return `$env:IDF_PATH='${IDF_DIR}'; $env:IDF_PYTHON_ENV_PATH='${IDF_VENV}'; `
      + `$env:PATH='${IDF_VENV_BIN};' + $env:PATH; `
      + `& '${IDF_EXPORT}' *> $null; Set-Location '${IDF_C2_DIR}'; ${innerCmd}`;
  }
  return `export IDF_PATH="${IDF_DIR}"; export IDF_PYTHON_ENV_PATH="${IDF_VENV}"; `
    + `export PATH="${IDF_VENV_BIN}:$PATH"; `
    + `source "${IDF_EXPORT}" >/dev/null 2>&1; cd "${IDF_C2_DIR}" && ${innerCmd}`;
}

// ---- 解析 arduino-cli 路径：PATH -> 平台常见安装位置 ----
let CLI = 'arduino-cli';
try {
  const which = isWin() ? 'where arduino-cli' : 'which arduino-cli';
  const p = execSync(which).toString().trim().split(/\r?\n/)[0];
  if (p) CLI = p;
} catch (e) { /* ignore */ }
if (CLI === 'arduino-cli') {
  const exe = isWin() ? 'arduino-cli.exe' : 'arduino-cli';
  const cands = [
    process.env.LOCALAPPDATA && path.join(process.env.LOCALAPPDATA, 'Programs', 'arduino-cli', exe),
    process.env.PROGRAMFILES && path.join(process.env.PROGRAMFILES, 'Arduino CLI', exe),
    path.join(os.homedir(), '.arduino15', 'bin', exe),
    '/usr/local/bin/' + exe,
    '/opt/homebrew/bin/' + exe,
  ].filter(Boolean);
  for (const c of cands) {
    if (fs.existsSync(c)) { CLI = c; break; }
  }
}

// ---- 产品板型：每个板同时指定芯片 FQBN 与固件内的 BOARD_XXX 宏 ----
// 编译时会用 --build-property （esp32: build.defines / esp8266: build.extra_flags）=-D<macro>
// 传给 arduino-cli（arduino-cli 无 -D 标志）。esp32 不能直接覆盖 build.extra_flags，否则会
// 丢掉 -DESP32=ESP32 等关键宏，故注入到被合并引用的 build.defines 槽。
const BOARDS = [
  { fqbn: 'esp32:esp32:esp32c3',  macro: 'BOARD_ESP32C3_SWITCH',      name: 'ESP32C3-Switch' },
  { fqbn: 'esp32:esp32:esp32c3',  macro: 'BOARD_ESPC3_MODULE',        name: 'ESPC3-Module' },
  { fqbn: 'esp8266:esp8266:generic', macro: 'BOARD_ESP01F_SWITCH',    name: 'ESP01F-Switch' },
  { fqbn: 'esp8266:esp8266:generic', macro: 'BOARD_ESP8285_SWITCH',   name: 'ESP8285-Switch' },
  { fqbn: 'esp8266:esp8266:generic', macro: 'BOARD_ESP8285_MODULE',   name: 'ESP8285-Module' },
];

function isValidBoardMacro(s) {
  return typeof s === 'string' && /^BOARD_[A-Z0-9_]+$/.test(s) && s.length < 60;
}

// ESP32-C2 子板（ESP-IDF 构建专用）：只在 arduino-cli 路径下编不过的 C2 板，
// 经 idf-c2 工程（arduino 作为 component）编译。板宏经 IDF_C2_BOARD 环境变量注入。
const C2_BOARDS = [
  { macro: 'BOARD_ESP32C2_SWITCH_NANO', name: 'ESP32-C2-Switch-Nano' },
  { macro: 'BOARD_ESP32C2_SWITCH_DEV',  name: 'ESP32-C2-Switch-Dev' },
  { macro: 'BOARD_ESP32C2_MODULE',      name: 'ESP32-C2-Module' },
];
function isValidC2Board(s) {
  return typeof s === 'string' && /^BOARD_ESP32C2_[A-Z0-9_]+$/.test(s) && s.length < 60;
}

// arduino-cli 没有 -D 标志，必须走 --build-property。
// esp32 核心里 build.extra_flags 是“合并字符串”，直接覆盖会丢掉 -DESP32=ESP32、
// USB 模式等关键宏；它的 {build.defines} 槽位是空的且被合并进后者，因此 esp32 用
// build.defines。esp8266 核心的 recipe 直接引用 build.extra_flags 且该值默认空，
// 覆盖它即可（generic 板无 mcu 专属 extra flags）。
function extraFlagKey(fqbn) {
  return fqbn.startsWith('esp32:esp32:') ? 'build.defines' : 'build.extra_flags';
}

// 编译产物目录：按 fqbn（+ 产品板宏）分子目录。
// 同一 fqbn 下不同产品板（如 ESP32C2-Switch-Nano 与 -Dev）必须各自独立，
// 否则切换板型时 arduino-cli 可能复用旧 .bin，导致烧错引脚。
function buildDirFor(fqbn, board) {
  const base = fqbn.replace(/[:/]/g, '_');
  return path.join(BUILD_BASE, (board && isValidBoardMacro(board)) ? base + '_' + board : base);
}

// IDF 构建产物目录：按 C2 板型分开（idf.py -B build/<BOARD_XXX>），避免切换板型互相覆盖。
function idfBuildDirFor(board) {
  return path.join(IDF_C2_DIR, 'build', (board && isValidC2Board(board)) ? board : 'BOARD_ESP32C2_SWITCH_NANO');
}

// ---- 工具函数 ----

// 异步执行一条命令并取 stdout，带超时兜底（子进程挂起也不阻塞事件循环）。
// Windows 上 reg/mode 这类命令在部分受限环境可能被策略挂起，必须双重保险。
function execQuick(cmd, timeoutMs) {
  return new Promise(resolve => {
    let done = false;
    let child = null;
    const finish = val => {
      if (done) return;
      done = true;
      try { if (child && child.pid) child.kill('SIGKILL'); } catch (_) {}
      resolve(val);
    };
    try {
      child = exec(cmd, { timeout: timeoutMs, windowsHide: true, maxBuffer: 1024 * 1024 },
        (err, stdout) => finish(err ? '' : (stdout || '').toString()));
    } catch (e) { finish(''); return; }
    // 兜底：即便 timeout 机制失效，也强制在超时后返回
    setTimeout(() => finish(''), timeoutMs + 1000);
  });
}

function parseComPorts(raw) {
  // 一次性 match 提取 COM 口（避免 while + /g exec 在某些字符序列下 lastIndex 不推进导致死循环）
  const all = (raw || '').match(/COM\d{1,3}/g) || [];
  return [...new Set(all)];
}

async function listPorts() {
  if (!isWin()) {
    // macOS / Linux：直接扫描串口设备节点（快）
    try {
      return fs.readdirSync('/dev')
        .filter(n => /^cu\./.test(n) || /^tty\.(USB|ACM)/.test(n) || /^ttyUSB\d+$/.test(n) || /^ttyACM\d+$/.test(n))
        .map(n => '/dev/' + n);
    } catch (e) { return []; }
  }
  // Windows：优先查注册表 SERIALCOMM（秒回、不触碰硬件），失败回退 mode 命令。
  for (const cmd of ['reg query HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM', 'mode']) {
    const ports = parseComPorts(await execQuick(cmd, 3000));
    if (ports.length) return ports;
  }
  return [];
}

function isValidFqbn(s) {
  return typeof s === 'string' && /^[a-zA-Z0-9:._-]+$/.test(s) && s.length < 80;
}
function isValidPort(s) {
  if (typeof s !== 'string' || !s.length) return false;
  if (isWin()) return /^COM\d{1,3}$/i.test(s);   // Windows: COM1..COM255
  return s.startsWith('/dev/cu.') || s.startsWith('/dev/tty.');  // macOS/Linux
}

function sseHeaders(res) {
  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    'Connection': 'keep-alive',
  });
}
function sseSend(res, event, data) {
  try {
    res.write('event: ' + event + '\n');
    res.write('data: ' + JSON.stringify(data) + '\n\n');
  } catch (e) { /* client gone */ }
}

// 收集 buildDir 下所有 .bin 固件产物（递归，按主 sketch bin 优先排序）
function collectFirmware(buildDir) {
  const out = [];
  if (!fs.existsSync(buildDir)) return out;
  (function walk(dir) {
    let names;
    try { names = fs.readdirSync(dir); } catch (e) { return; }
    for (const n of names) {
      const full = path.join(dir, n);
      let st;
      try { st = fs.statSync(full); } catch (e) { continue; }
      if (st.isDirectory()) walk(full);
      else if (n.endsWith('.bin')) out.push({ name: n, size: st.size });
    }
  })(buildDir);
  out.sort((a, b) => {
    // 主 sketch 固件（*.ino.bin）永远排第一，其余按大小降序
    const am = a.name.endsWith('.ino.bin');
    const bm = b.name.endsWith('.ino.bin');
    if (am !== bm) return am ? -1 : 1;
    return b.size - a.size;
  });
  return out;
}

// 收集 idf-c2 构建产物（指定板型的 build/<BOARD_XXX> 下所有 .bin，按主 app bin 优先排序）
function collectIdfFirmware(board) {
  const out = [];
  const root = idfBuildDirFor(board);
  if (!fs.existsSync(root)) return out;
  (function walk(dir) {
    let names;
    try { names = fs.readdirSync(dir); } catch (e) { return; }
    for (const n of names) {
      const full = path.join(dir, n);
      let st;
      try { st = fs.statSync(full); } catch (e) { continue; }
      if (st.isDirectory()) walk(full);
      else if (n.endsWith('.bin')) out.push({ name: n, size: st.size });
    }
  })(root);
  // 主 app bin（与工程同名，如 esp32_light_switch_c2.bin）排第一，其余按大小降序
  out.sort((a, b) => {
    const am = a.name.startsWith('esp32_light_switch_c2.') || a.name.startsWith('idf-c2.');
    const bm = b.name.startsWith('esp32_light_switch_c2.') || b.name.startsWith('idf-c2.');
    if (am !== bm) return am ? -1 : 1;
    return b.size - a.size;
  });
  return out;
}

// 扫描 BUILD_BASE 下所有 arduino 板型的编译产物目录（目录名 = fqbn_<BOARD_XXX>）
function scanArduinoArtifacts() {
  const out = [];
  if (!fs.existsSync(BUILD_BASE)) return out;
  let dirs;
  try { dirs = fs.readdirSync(BUILD_BASE); } catch (e) { return out; }
  for (const d of dirs) {
    const full = path.join(BUILD_BASE, d);
    let st;
    try { st = fs.statSync(full); } catch (e) { continue; }
    if (!st.isDirectory()) continue;
    // 目录名反查产品板：找 BOARDS 中 fqbn 拼 board 宏能对上目录名的项
    const board = BOARDS.find(b => buildDirFor(b.fqbn, b.macro) === full);
    if (!board) continue;
    const files = collectFirmware(full).map(f => {
      let mt = 0;
      try { mt = fs.statSync(path.join(full, f.name)).mtimeMs; } catch (e) {}
      return { name: f.name, size: f.size, mtime: mt };
    });
    if (!files.length) continue;
    out.push({ type: 'arduino', fqbn: board.fqbn, board: board.macro, boardName: board.name, files });
  }
  return out;
}

// 扫描 idf-c2/build/<BOARD_XXX> 下所有 C2 板型的构建产物
function scanIdfArtifacts() {
  const out = [];
  const root = path.join(IDF_C2_DIR, 'build');
  if (!fs.existsSync(root)) return out;
  let dirs;
  try { dirs = fs.readdirSync(root); } catch (e) { return out; }
  for (const d of dirs) {
    const full = path.join(root, d);
    let st;
    try { st = fs.statSync(full); } catch (e) { continue; }
    if (!st.isDirectory()) continue;
    const board = C2_BOARDS.find(b => b.macro === d);
    if (!board) continue;
    const files = collectFirmware(full).map(f => {
      let mt = 0;
      try { mt = fs.statSync(path.join(full, f.name)).mtimeMs; } catch (e) {}
      return { name: f.name, size: f.size, mtime: mt };
    });
    if (!files.length) continue;
    out.push({ type: 'idf', board: board.macro, boardName: board.name, files });
  }
  return out;
}

/**
 * 以 SSE 方式流式执行一条 arduino-cli 命令。
 * 客户端断开 EventSource 时自动 kill 子进程（对 monitor 尤其重要）。
 * buildDir/fqbn 提供时，命令成功结束后会额外推送 firmware 事件（固件产物清单）。
 */
function runStream(res, args, label, buildDir, fqbn, board) {
  sseHeaders(res);
  sseSend(res, 'start', { label, cli: CLI, args });

  const fullArgs = ['--config-file', CONFIG_FILE].concat(args);
  let child;
  try {
    child = spawn(CLI, fullArgs, { cwd: SKETCH_DIR });
  } catch (e) {
    sseSend(res, 'err', 'spawn error: ' + e.message);
    sseSend(res, 'done', { code: -1 });
    try { res.end(); } catch (_) {}
    return;
  }

  const cleanup = () => { try { child.kill('SIGKILL'); } catch (_) {} };
  res.on('close', cleanup);

  let stdoutBuf = '';
  child.stdout.on('data', d => {
    const s = d.toString();
    stdoutBuf += s;
    sseSend(res, 'out', s);
  });
  child.stderr.on('data', d => sseSend(res, 'err', d.toString()));
  child.on('error', err => {
    sseSend(res, 'err', 'spawn error: ' + err.message);
    sseSend(res, 'done', { code: -1 });
    try { res.end(); } catch (_) {}
  });
  child.on('close', code => {
    code = code == null ? 0 : code;
    if (buildDir && code === 0) {
      const files = collectFirmware(buildDir);
      const m = stdoutBuf.match(/Sketch uses (\d+) bytes/);
      sseSend(res, 'firmware', { files, size: m ? parseInt(m[1], 10) : null, fqbn: fqbn || '', board: board || '' });
    }
    sseSend(res, 'done', { code });
    try { res.end(); } catch (_) {}
  });
}

// 以 SSE 方式流式执行一条 idf.py 命令：source IDF 环境后，在 idf-c2 目录跑构建/烧录。
// IDF_C2_BOARD 通过 env 注入，供 idf-c2/main/CMakeLists.txt 选择产品板宏。
// Windows 用 PowerShell 执行（ESP-IDF 官方仅支持 cmd/PowerShell，bash 会被 MSYS 检测拒绝），
// macOS/Linux 用 bash。
function runIdfStream(res, shell, label, board) {
  sseHeaders(res);
  sseSend(res, 'start', { label, cli: 'idf.py', args: shell });

  const env = Object.assign({}, process.env, { IDF_C2_BOARD: board });
  let child;
  try {
    if (isWin()) {
      child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', shell], { cwd: IDF_C2_DIR, env });
    } else {
      child = spawn('bash', ['-c', shell], { cwd: IDF_C2_DIR, env });
    }
  } catch (e) {
    sseSend(res, 'err', 'spawn error: ' + e.message);
    sseSend(res, 'done', { code: -1 });
    try { res.end(); } catch (_) {}
    return;
  }

  const cleanup = () => { try { child.kill('SIGKILL'); } catch (_) {} };
  res.on('close', cleanup);

  child.stdout.on('data', d => sseSend(res, 'out', d.toString()));
  child.stderr.on('data', d => sseSend(res, 'err', d.toString()));
  child.on('error', err => {
    sseSend(res, 'err', 'spawn error: ' + err.message);
    sseSend(res, 'done', { code: -1 });
    try { res.end(); } catch (_) {}
  });
  child.on('close', code => {
    code = code == null ? 0 : code;
    if (code === 0) {
      const files = collectIdfFirmware(board);
      sseSend(res, 'firmware', { files, size: null, board: board || '' });
    }
    sseSend(res, 'done', { code });
    try { res.end(); } catch (_) {}
  });
}

// ---- HTTP 路由 ----
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const p = url.pathname;

  // 静态首页
  if (p === '/' || p === '/index.html') {
    fs.readFile(path.join(PUBLIC_DIR, 'index.html'), (err, data) => {
      if (err) { res.writeHead(500); res.end('index.html not found'); return; }
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(data);
    });
    return;
  }

  if (p === '/api/status') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      platform: process.platform,
      cli: CLI,
      cliExists: fs.existsSync(CLI),
      config: CONFIG_FILE,
      configExists: fs.existsSync(CONFIG_FILE),
      sketch: SKETCH_DIR,
      sketchExists: fs.existsSync(SKETCH_DIR),
      idfDir: IDF_DIR,
      idfExists: fs.existsSync(IDF_EXPORT),
      idfVenv: IDF_VENV,
      idfVenvExists: fs.existsSync(IDF_VENV_PY),
      idfC2Exists: fs.existsSync(IDF_C2_DIR),
    }));
    return;
  }

  if (p === '/api/ports') {
    listPorts().then(ports => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ports }));
    }).catch(() => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end('{"ports":[]}');
    });
    return;
  }

  if (p === '/api/boards') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ boards: BOARDS }));
    return;
  }

  if (p === '/api/stream/compile') {
    const fqbn = url.searchParams.get('fqbn');
    const verbose = url.searchParams.get('verbose') === '1';
    const board = url.searchParams.get('board');
    if (!isValidFqbn(fqbn)) { res.writeHead(400); res.end('bad fqbn'); return; }
    const buildDir = buildDirFor(fqbn, board);
    const args = ['compile', '--fqbn', fqbn, '--output-dir', buildDir];
    if (board && isValidBoardMacro(board)) args.push('--build-property', extraFlagKey(fqbn) + '=-D' + board);  // 选定产品板的引脚/功能
    if (verbose) args.push('--verbose');
    args.push(SKETCH_DIR);
    runStream(res, args, 'compile', buildDir, fqbn, board);
    return;
  }

  if (p === '/api/stream/upload') {
    const fqbn = url.searchParams.get('fqbn');
    const port = url.searchParams.get('port');
    const verbose = url.searchParams.get('verbose') === '1';
    const board = url.searchParams.get('board');
    if (!isValidFqbn(fqbn) || !isValidPort(port)) { res.writeHead(400); res.end('bad params'); return; }
    const buildDir = buildDirFor(fqbn, board);
    const args = ['compile', '--fqbn', fqbn, '--output-dir', buildDir, '--upload', '-p', port];
    if (board && isValidBoardMacro(board)) args.push('--build-property', extraFlagKey(fqbn) + '=-D' + board);  // 选定产品板的引脚/功能
    if (verbose) args.push('--verbose');
    args.push(SKETCH_DIR);
    runStream(res, args, 'upload', buildDir, fqbn, board);
    return;
  }

  if (p === '/api/stream/monitor') {
    const port = url.searchParams.get('port');
    const baud = url.searchParams.get('baud') || '115200';
    if (!isValidPort(port)) { res.writeHead(400); res.end('bad port'); return; }
    const args = ['monitor', '-p', port, '-c', 'baudrate=' + baud];
    runStream(res, args, 'monitor');
    return;
  }

  if (p === '/api/download') {
    const fqbn = url.searchParams.get('fqbn');
    const board = url.searchParams.get('board');
    const file = url.searchParams.get('file');
    if (!isValidFqbn(fqbn) || !file || !/^[\w.\-]+$/.test(file)) { res.writeHead(400); res.end('bad params'); return; }
    const buildDir = buildDirFor(fqbn, board);
    const base = path.resolve(BUILD_BASE);
    const fp = path.resolve(buildDir, file);
    // 路径穿越防护：解析后必须仍位于 BUILD_BASE 之内
    if (fp !== base && !fp.startsWith(base + path.sep)) { res.writeHead(403); res.end('forbidden'); return; }
    if (!fs.existsSync(fp) || !fs.statSync(fp).isFile()) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, {
      'Content-Type': 'application/octet-stream',
      'Content-Disposition': 'attachment; filename="' + file + '"',
      'Content-Length': fs.statSync(fp).size,
    });
    fs.createReadStream(fp).pipe(res);
    return;
  }

  if (p === '/api/idf/boards') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ boards: C2_BOARDS }));
    return;
  }

  if (p === '/api/stream/idfbuild') {
    const board = url.searchParams.get('board') || 'BOARD_ESP32C2_SWITCH_NANO';
    const action = (url.searchParams.get('action') || 'build').toLowerCase();
    const port = url.searchParams.get('port') || '';
    const doSet = url.searchParams.get('settarget') !== '0';
    if (!isValidC2Board(board)) { res.writeHead(400); res.end('bad board'); return; }
    if (action === 'flash') {
      if (!isValidPort(port)) { res.writeHead(400); res.end('bad port'); return; }
    } else if (action !== 'build') { res.writeHead(400); res.end('bad action'); return; }
    const target = 'esp32c2';
    // 按板型分构建目录：build/<BOARD_XXX>，切换板型互不覆盖
    const bdir = idfBuildDirFor(board);
    const idfCmd = action === 'flash' ? `idf.py -B "${bdir}" -p "${port}" flash` : `idf.py -B "${bdir}" build`;
    const setStep = doSet ? `idf.py -B "${bdir}" set-target ${target} && ` : '';
    // idfShell() 内部已按平台初始化 IDF 环境（Win: export.ps1 / mac-Linux: export.sh）并进入 idf-c2
    const shell = idfShell(setStep + idfCmd);
    runIdfStream(res, shell, (action === 'flash' ? 'C2 构建并烧录' : 'C2 构建'), board);
    return;
  }

  if (p === '/api/idf/download') {
    const file = url.searchParams.get('file');
    const board = url.searchParams.get('board') || 'BOARD_ESP32C2_SWITCH_NANO';
    if (!file || !/^[\w.\-]+$/.test(file)) { res.writeHead(400); res.end('bad params'); return; }
    if (!isValidC2Board(board)) { res.writeHead(400); res.end('bad board'); return; }
    const root = path.resolve(idfBuildDirFor(board));
    let found = null;
    (function walk(dir) {
      if (found) return;
      let names; try { names = fs.readdirSync(dir); } catch (e) { return; }
      for (const n of names) {
        if (found) return;
        const full = path.join(dir, n);
        let st; try { st = fs.statSync(full); } catch (e) { continue; }
        if (st.isDirectory()) walk(full);
        else if (n === file) { found = full; }
      }
    })(root);
    if (!found) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, {
      'Content-Type': 'application/octet-stream',
      'Content-Disposition': 'attachment; filename="' + file + '"',
      'Content-Length': fs.statSync(found).size,
    });
    fs.createReadStream(found).pipe(res);
    return;
  }

  if (p === '/api/stream/idfmonitor') {
    const port = url.searchParams.get('port');
    const baud = url.searchParams.get('baud') || '115200';
    if (!isValidPort(port)) { res.writeHead(400); res.end('bad port'); return; }
    if (!/^\d{4,7}$/.test(baud)) { res.writeHead(400); res.end('bad baud'); return; }
    const shell = idfShell(`idf.py -p "${port}" -b ${baud} monitor`);
    runIdfStream(res, shell, '监视 ' + port, '');
    return;
  }

  if (p === '/api/artifacts') {
    // 聚合所有板型（arduino + IDF）的编译产物及生成时间
    const list = scanArduinoArtifacts().concat(scanIdfArtifacts());
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ artifacts: list }));
    return;
  }

  // 构建产物 Tab：把指定板型的产物烧写到串口（arduino 用 arduino-cli upload --input-dir 直接烧已有 .bin；
  // idf 用 idf.py -B <board> flash）。不重新编译。
  if (p === '/api/stream/flash') {
    const type = url.searchParams.get('type');
    const fqbn = url.searchParams.get('fqbn') || '';
    const board = url.searchParams.get('board') || '';
    const port = url.searchParams.get('port') || '';
    if (!isValidPort(port)) { res.writeHead(400); res.end('bad port'); return; }
    if (type === 'arduino') {
      if (!isValidFqbn(fqbn) || !isValidBoardMacro(board)) { res.writeHead(400); res.end('bad params'); return; }
      const buildDir = buildDirFor(fqbn, board);
      if (!fs.existsSync(buildDir) || !collectFirmware(buildDir).length) { res.writeHead(404); res.end('no artifacts'); return; }
      const args = ['upload', '--fqbn', fqbn, '-p', port, '--input-dir', buildDir];
      runStream(res, args, '烧写 ' + board, buildDir, fqbn, board);
    } else if (type === 'idf') {
      if (!isValidC2Board(board)) { res.writeHead(400); res.end('bad board'); return; }
      const bdir = idfBuildDirFor(board);
      if (!fs.existsSync(bdir) || !collectIdfFirmware(board).length) { res.writeHead(404); res.end('no artifacts'); return; }
      const shell = idfShell(`idf.py -B "${bdir}" -p "${port}" flash`);
      runIdfStream(res, shell, '烧写 ' + board, board);
    } else {
      res.writeHead(400); res.end('bad type');
    }
    return;
  }

  res.writeHead(404);
  res.end('not found');
});

server.listen(PORT, '0.0.0.0', () => {
  console.log('ESP-Switch 网页控制台已启动: http://localhost:' + PORT);
  console.log('arduino-cli: ' + CLI + ' | IDF: ' + IDF_DIR + ' | venv: ' + IDF_VENV);
  console.log('sketch: ' + SKETCH_DIR + ' | idf-c2: ' + IDF_C2_DIR);
});
