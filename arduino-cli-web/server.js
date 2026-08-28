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
const path = require('path');
const { spawn, execSync } = require('child_process');

const PORT = process.env.PORT || 8787;
// 路径一律「环境变量优先，缺省回退写死值」：换机器部署时设环境变量即可，无需改代码。
// 注意不用 IDF 官方的 IDF_PATH（可能被终端残留的其他 IDF 版本污染），本项目用专用变量。
const SKETCH_DIR = process.env.ESP_SWITCH_SKETCH_DIR || '/Users/meizu/work/ESP-Switch/ESP32_Light_Switch';
const PUBLIC_DIR = path.join(__dirname, 'public');
// 编译产物目录（按 fqbn 分子目录，避免 c2/c3 互相覆盖）
const BUILD_BASE = '/tmp/espbuild';
const CONFIG_FILE = path.join(process.env.HOME || '', '.arduino15', 'arduino-cli.yaml');

// ---- ESP-IDF 环境（用于编译 ESP32-C2，复用本机 IDF + arduino-esp32 作组件）----
// IDF_DIR 解析优先级：ESP_SWITCH_IDF_DIR（显式覆盖）> 环境变量 IDF_PATH（当前 shell 的 IDF）> 写死默认值。
// 这样你 source 哪个 IDF 就编哪个；显式变量可压过环境，防止误用。
const IDF_DIR = process.env.ESP_SWITCH_IDF_DIR || process.env.IDF_PATH || '/Users/meizu/esp/esp-idf';
const IDF_EXPORT = path.join(IDF_DIR, 'export.sh');
const IDF_C2_DIR = process.env.ESP_SWITCH_IDF_C2_DIR || '/Users/meizu/work/ESP-Switch/idf-c2';
// IDF 的 python venv（含 click）：显式锁定，避免 export.sh 在 node 进程 PATH 下
// 探测到无 click 的 python（如 miniconda）而挂错 venv（症状：Cannot import module "click"）。
// 解析优先级：
//   1) 环境变量 IDF_PYTHON_ENV_PATH（source 过 IDF 的 shell 已设好，直接用）
//   2) 从 IDF_DIR 的 version.cmake 推导前缀（idf5.3 / idf5.4 …），再在 ~/.espressif/python_env 下探测
//   3) ESP_SWITCH_IDF_VENV_PREFIX 显式覆盖前缀（跨机 IDF 版本特殊时用）
const IDF_VENV = (() => {
  if (process.env.IDF_PYTHON_ENV_PATH) return process.env.IDF_PYTHON_ENV_PATH;
  const envRoot = path.join(process.env.HOME || '', '.espressif', 'python_env');
  // 从 version.cmake 读 IDF 主次版本 -> 前缀 idfX.Y
  let prefix = process.env.ESP_SWITCH_IDF_VENV_PREFIX;
  if (!prefix) {
    try {
      const v = fs.readFileSync(path.join(IDF_DIR, 'tools', 'cmake', 'version.cmake'), 'utf8');
      const mj = (v.match(/set\(IDF_VERSION_MAJOR\s+(\d+)\)/) || [])[1];
      const mn = (v.match(/set\(IDF_VERSION_MINOR\s+(\d+)\)/) || [])[1];
      if (mj && mn) prefix = 'idf' + mj + '.' + mn;
    } catch (_) { /* fallthrough */ }
  }
  prefix = prefix || 'idf5.3';
  try {
    const cands = fs.readdirSync(envRoot)
      .filter(d => d.startsWith(prefix + '_py') && d.endsWith('_env'))
      .sort();
    const pick = cands.find(d => d.includes('py3.13')) || cands[cands.length - 1];
    if (pick) return path.join(envRoot, pick);
  } catch (_) { /* fallthrough */ }
  return path.join(envRoot, prefix + '_py3.13_env');
})();
const IDF_VENV_BIN = path.join(IDF_VENV, 'bin');

// ---- 解析 arduino-cli 路径 ----
let CLI = 'arduino-cli';
try {
  const p = execSync('which arduino-cli').toString().trim();
  if (p) CLI = p;
} catch (e) { /* ignore */ }
if (CLI === 'arduino-cli') {
  const cands = [
    '/usr/local/bin/arduino-cli',
    '/opt/homebrew/bin/arduino-cli',
    path.join(process.env.HOME || '', '.arduino15', 'bin', 'arduino-cli'),
  ];
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
function listPorts() {
  try {
    return fs.readdirSync('/dev')
      .filter(n => n.startsWith('cu.'))
      .map(n => '/dev/' + n);
  } catch (e) { return []; }
}

function isValidFqbn(s) {
  return typeof s === 'string' && /^[a-zA-Z0-9:._-]+$/.test(s) && s.length < 80;
}
function isValidPort(s) {
  return typeof s === 'string' && (s.startsWith('/dev/cu.') || s.startsWith('/dev/tty.'));
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
function runIdfStream(res, shell, label, board) {
  sseHeaders(res);
  sseSend(res, 'start', { label, cli: 'idf.py', args: shell });

  const env = Object.assign({}, process.env, { IDF_C2_BOARD: board });
  let child;
  try {
    child = spawn('bash', ['-c', shell], { cwd: IDF_C2_DIR, env });
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
      cli: CLI,
      cliExists: fs.existsSync(CLI),
      config: CONFIG_FILE,
      configExists: fs.existsSync(CONFIG_FILE),
      sketch: SKETCH_DIR,
      sketchExists: fs.existsSync(SKETCH_DIR),
      idfDir: IDF_DIR,
      idfExists: fs.existsSync(path.join(IDF_DIR, 'export.sh')),
      idfVenv: IDF_VENV,
      idfVenvExists: fs.existsSync(path.join(IDF_VENV, 'bin', 'python3')),
      idfC2Exists: fs.existsSync(IDF_C2_DIR),
    }));
    return;
  }

  if (p === '/api/ports') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ports: listPorts() }));
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
    // 显式锁定 IDF_PATH + IDF_PYTHON_ENV_PATH + venv bin 前置，再 source export.sh：
    // 防止 node 进程继承的 PATH/残留变量导致探测到无 click 的 python（症状 "Cannot import module click"）。
    // 进入 idf-c2 -> (可选)set-target -> build/flash
    const shell = `export IDF_PATH="${IDF_DIR}"; export IDF_PYTHON_ENV_PATH="${IDF_VENV}"; `
      + `export PATH="${IDF_VENV_BIN}:$PATH"; `
      + `source "${IDF_EXPORT}" >/dev/null 2>&1; cd "${IDF_C2_DIR}" && ${setStep}${idfCmd}`;
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
    const shell = `export IDF_PATH="${IDF_DIR}"; export IDF_PYTHON_ENV_PATH="${IDF_VENV}"; `
      + `export PATH="${IDF_VENV_BIN}:$PATH"; `
      + `source "${IDF_EXPORT}" >/dev/null 2>&1; cd "${IDF_C2_DIR}" && idf.py -p "${port}" -b ${baud} monitor`;
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
      const shell = `export IDF_PATH="${IDF_DIR}"; export IDF_PYTHON_ENV_PATH="${IDF_VENV}"; `
        + `export PATH="${IDF_VENV_BIN}:$PATH"; `
        + `source "${IDF_EXPORT}" >/dev/null 2>&1; cd "${IDF_C2_DIR}" && idf.py -B "${bdir}" -p "${port}" flash`;
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
