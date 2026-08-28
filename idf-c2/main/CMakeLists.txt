idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino
)

# main.cpp 是薄壳：include ../../ESP32_Light_Switch/ESP32_Light_Switch.ino（唯一固件源码）。
# arduino-cli 路径直接编译 .ino，本工程经 include 复用同一份代码，避免两份拷贝分叉。

# 选定产品板：优先读环境变量 IDF_C2_BOARD（网页控制台按所选板注入），
# 手动 idf.py build 时未设置则默认 ESP32-C2-Switch-Nano。
# 可选值：BOARD_ESP32C2_SWITCH_NANO / BOARD_ESP32C2_SWITCH_DEV / BOARD_ESP32C2_MODULE
if(NOT DEFINED ENV{IDF_C2_BOARD})
  set(ENV{IDF_C2_BOARD} "BOARD_ESP32C2_SWITCH_NANO")
endif()
target_compile_definitions(${COMPONENT_TARGET} PRIVATE $ENV{IDF_C2_BOARD})
