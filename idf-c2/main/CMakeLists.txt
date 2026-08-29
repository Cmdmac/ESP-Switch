# REQUIRES 里的 arduino 组件名 = 本地 core 链接目录名：
# server.js 在 C2 构建前会把本机 Arduino IDE / arduino-cli 已装的 esp32 core
# 链接为 idf-c2/components/arduino-esp32（Windows 用 junction，mac/Linux 用 symlink）
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino-esp32 esp_wifi esp_netif esp_event esp_common driver esp_hw_support
)

# main.cpp 是薄壳：include ../../ESP32_Light_Switch/ESP32_Light_Switch.ino（唯一固件源码）。
# arduino-cli 路径直接编译 .ino，本工程经 include 复用同一份代码，避免两份拷贝分叉。

# ---- 从工程侧补齐 arduino-esp32 组件缺失的组件依赖 ----
# arduino-esp32 自带的 CMakeLists.txt 未声明 esp_wifi / esp_netif / esp_event 等依赖，
# 而 IDF 5.x 起这类组件不再默认加入公共依赖，必须显式声明，否则编译报
# "WiFiType.h includes esp_wifi_types.h ... not in the requirements list"。
# 这里不改 arduino 组件自身的文件，而是在本工程内给它补 include 目录与链接。
# 组件列表按需扩展：把缺失的组件名同时加到上面 REQUIRES 与下面的列表即可。
set(ARDUINO_MISSING_REQUIRES esp_wifi esp_netif esp_event esp_common driver esp_hw_support)
idf_component_get_property(_arduino_lib arduino-esp32 COMPONENT_LIB)
if(_arduino_lib)
  foreach(_c ${ARDUINO_MISSING_REQUIRES})
    idf_component_get_property(_c_lib ${_c} COMPONENT_LIB)
    if(_c_lib)
      idf_component_get_property(_c_inc ${_c} INCLUDE_DIRS)
      idf_component_get_property(_c_alias ${_c} COMPONENT_ALIAS)
      target_include_directories(${_arduino_lib} PRIVATE ${_c_inc})
      target_link_libraries(${_arduino_lib} INTERFACE ${_c_alias})
    endif()
  endforeach()
endif()

# 选定产品板：优先读环境变量 IDF_C2_BOARD（网页控制台按所选板注入），
# 手动 idf.py build 时未设置则默认 ESP32-C2-Switch-Nano。
# 可选值：BOARD_ESP32C2_SWITCH_NANO / BOARD_ESP32C2_SWITCH_DEV / BOARD_ESP32C2_MODULE
if(NOT DEFINED ENV{IDF_C2_BOARD})
  set(ENV{IDF_C2_BOARD} "BOARD_ESP32C2_SWITCH_NANO")
endif()
target_compile_definitions(${COMPONENT_TARGET} PRIVATE $ENV{IDF_C2_BOARD})
