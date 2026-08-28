/*
  ESP32-C2 (ESP-IDF + arduino 组件) 入口 —— 薄壳文件
  ===================================================
  唯一固件源码是 ESP32_Light_Switch/ESP32_Light_Switch.ino：
    - arduino-cli 路径（Tab1）：直接编译 .ino
    - ESP-IDF 路径（Tab2，本工程）：本文件 include 同一份 .ino
  这样两个编译体系共用一份固件代码，不再维护两份拷贝。

  组件模式注意：
  - arduino 组件 AUTOSTART_ARDUINO=n，不自带 app_main，必须在此提供；
  - 先 initArduino() 完成 Arduino 运行时初始化，再跑 setup()/loop()。
*/
#include "Arduino.h"

// 引入唯一固件源码（.ino 是合法 C++ 源文件，直接 include 复用）
#include "../../ESP32_Light_Switch/ESP32_Light_Switch.ino"

extern "C" void app_main() {
  initArduino();
  setup();
  for (;;) {
    loop();
  }
}
