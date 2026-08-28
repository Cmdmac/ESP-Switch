#!/usr/bin/env bash
# ESP-Switch 网页控制台启动脚本（macOS / Linux）
# 零依赖：环境自动探测，无需设置任何环境变量
cd "$(dirname "$0")"
exec node server.js
