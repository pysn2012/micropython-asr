# 创建用户模块库
add_library(usermod_K10_asr INTERFACE)

# 添加源文件
target_sources(usermod_K10_asr INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/k10_asr.cpp
    ${CMAKE_CURRENT_LIST_DIR}/k10_asr.c
)

# 设置包含目录
target_include_directories(usermod_K10_asr INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${IDF_PATH}/components/esp-sr
    ${IDF_PATH}/components/esp-sr/include/esp32s3
    ${IDF_PATH}/components/esp-sr/src/include
    ${IDF_PATH}/components/json/cJSON
    ${IDF_PATH}/components/esp-sr/esp-tts/esp_tts_chinese/include
)

# 启用C++异常和RTTI - 只对C++文件
target_compile_options(usermod_K10_asr INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    $<$<COMPILE_LANGUAGE:CXX>:-D_GNU_SOURCE>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-gnu-zero-variadic-macro-arguments>
)

target_compile_definitions(usermod_K10_asr INTERFACE
    CONFIG_IDF_TARGET_ESP32S3
    ESP_PLATFORM
    ESP32
    ESP32S3
    CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240
)

# 链接到主用户模块
target_link_libraries(usermod INTERFACE usermod_K10_asr)


