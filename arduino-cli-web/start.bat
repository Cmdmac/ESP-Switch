@echo off
rem ESP-Switch 网页控制台启动脚本（Windows）
rem 零依赖：环境自动探测，无需设置任何环境变量
cd /d "%~dp0"
node server.js
pause
