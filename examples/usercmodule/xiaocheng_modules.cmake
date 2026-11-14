# xiaocheng_modules.cmake — 小程掌机固件的用户模块聚合清单
# 仅掌机 CI 工作流（build xiaocheng handheld firmware）通过 USER_C_MODULES 挂载。
# 其他板卡（8M/ASR16M）继续用 asr/micropython.cmake，不带 ST7789。

# ASR：esp-sr 离线语音（唤醒 + 命令词 + TTS）
include(${CMAKE_CURRENT_LIST_DIR}/asr/micropython.cmake)

# ST7789：屏幕驱动 C 模块（russhughes/st7789_mpy，submodule pin 447d5fa）
include(${CMAKE_CURRENT_LIST_DIR}/st7789_mpy/st7789/micropython.cmake)
