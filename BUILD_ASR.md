# ESP-SR × MicroPython 构建与烧录（N8R8）

## 构建

GitHub Actions → `build mpy with asr` → Run workflow（手动触发）。
产物 Artifacts 下载两个文件：

- `ESP32_GENERIC_S3_SPIRAM_OCT_8MB.bin` — 固件
- `srmodels.bin` — esp-sr 模型包（WakeNet9 + MultiNet7 中文量化）

## 烧录

```bash
esptool.py --chip esp32s3 --port COM口 erase_flash
esptool.py --chip esp32s3 --port COM口 write_flash 0x0 ESP32_GENERIC_S3_SPIRAM_OCT_8MB.bin
esptool.py --chip esp32s3 --port COM口 write_flash 0x2B0000 srmodels.bin
```

- `0x2B0000` 是 model 分区偏移（partitions-8MiB.csv），srmodels.bin 必须烧到这里
- 模型没烧或烧错地址的表现：能进 REPL，但 `asr.init()` 报模型加载失败

## 分区表（partitions-8MiB.csv）

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| factory | 0x10000 | 2688K | MicroPython 固件 |
| model | 0x2B0000 | 3000K | srmodels.bin |
| vfs | 0x59E000 | 1472K | 用户文件系统 |

8MB flash 放不下中文 TTS 音色数据（~3.9MB），TTS 播报暂不可用。

## SDK 关键配置（boards/ESP32_GENERIC_S3/sdkconfig.board）

- 唤醒词 `wn9_hilexin`（Hi,乐鑫）+ 命令词 `mn7_cn` 量化 + WebRTC VAD
- esp-sr 预编译库要求：ICache 32KB / DCache 64KB / 64B line
- esp-sr v2.6.1 由 CI 克隆进 `$IDF_PATH/components/esp-sr`（非组件管理器路线）
- 构建矩阵：ESP-IDF v5.5.6（与 ESP-VISION 官方一致，便于后续合并视觉）
- 音频硬件：C 模块只管 I²S 总线（引脚全参数化）；INMP441/MAX98357A 零配置，
  ES7210/ES8311 等 I²C codec 由 Python 侧驱动先初始化再 `asr.init()`
