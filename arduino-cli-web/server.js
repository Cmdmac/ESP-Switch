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
const SKETCH_DIR = '/Users/meizu/work/ESP-Switch/ESP32_Light_Switch';
const PUBLIC_DIR = path.join(__dirname, 'public');
// 编译产物目录（按 fqbn 分子目录，避免 c2/c3 互相覆盖）
const BUILD_BASE = '/tmp/espbuild';
const CONFIG_FILE = path.join(process.env.HOME || '', '.arduino15', 'arduino-cli.yaml');

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

// ---- 可选板型 ----
const BOARDS = [
  { fqbn: 'esp32:esp32:esp32c2', name: 'ESP32-C2 Dev Module' },
  { fqbn: 'esp32:esp32:esp32c3', name: 'ESP32-C3 Dev Module' },
  { fqbn: 'esp8266:esp8266:generic', name: 'ESP8285 / ESP8266 Generic' },
];

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

/**
 * 以 SSE 方式流式执行一条 arduino-cli 命令。
 * 客户端断开 EventSource 时自动 kill 子进程（对 monitor 尤其重要）。
 * buildDir/fqbn 提供时，命令成功结束后会额外推送 firmware 事件（固件产物清单）。
 */
function runStream(res, args, label, buildDir, fqbn) {
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
      sseSend(res, 'firmware', { files, size: m ? parseInt(m[1], 10) : null, fqbn: fqbn || '' });
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
    if (!isValidFqbn(fqbn)) { res.writeHead(400); res.end('bad fqbn'); return; }
    const buildDir = path.join(BUILD_BASE, fqbn.replace(/[:]/g, '_'));
    const args = ['compile', '--fqbn', fqbn, '--output-dir', buildDir];
    if (verbose) args.push('--verbose');
    args.push(SKETCH_DIR);
    runStream(res, args, 'compile', buildDir, fqbn);
    return;
  }

  if (p === '/api/stream/upload') {
    const fqbn = url.searchParams.get('fqbn');
    const port = url.searchParams.get('port');
    const verbose = url.searchParams.get('verbose') === '1';
    if (!isValidFqbn(fqbn) || !isValidPort(port)) { res.writeHead(400); res.end('bad params'); return; }
    const buildDir = path.join(BUILD_BASE, fqbn.replace(/[:]/g, '_'));
    const args = ['compile', '--fqbn', fqbn, '--output-dir', buildDir, '--upload', '-p', port];
    if (verbose) args.push('--verbose');
    args.push(SKETCH_DIR);
    runStream(res, args, 'upload', buildDir, fqbn);
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
    const file = url.searchParams.get('file');
    if (!isValidFqbn(fqbn) || !file || !/^[\w.\-]+$/.test(file)) { res.writeHead(400); res.end('bad params'); return; }
    const buildDir = path.join(BUILD_BASE, fqbn.replace(/[:]/g, '_'));
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

  res.writeHead(404);
  res.end('not found');
});

server.listen(PORT, '0.0.0.0', () => {
  console.log('arduino-cli-web 已启动: http://localhost:' + PORT);
  console.log('CLI: ' + CLI + ' | sketch: ' + SKETCH_DIR);
});
