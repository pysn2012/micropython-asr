# ESP-SR × MicroPython 构建与烧录（N8R8）

## 构建

GitHub Actions → `build mpy with asr` → Run workflow（手动触发）。
产物 Artifacts 下载两个文件：

- `ESP32_GENERIC_S3_SPIRAM_OCT_8MB.bin` — 固件（**srmodels 已合并在内**，约 5.6MB）
- `srmodels.bin` — esp-sr 模型包（单独提供，备用；正常烧录用不到）

## 烧录（一条命令）

```bash
esptool.py --chip esp32s3 --port COM口 --baud 921600 erase_flash
esptool.py --chip esp32s3 --port COM口 --baud 921600 write_flash 0x0 ESP32_GENERIC_S3_SPIRAM_OCT_8MB.bin
```

固件内部已按分区表合并好 bootloader/分区表/应用程序/srmodels，烧 0x0 即可。
若 srmodels 超出 model 分区容量，构建时 makeimg.py 会直接报 overflow 错误（不会静默烧坏）。

- 模型没烧上的表现：能进 REPL，但 `asr.start_asr()` 报模型加载失败

## 分区表（partitions-8MiB.csv）

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| factory | 0x10000 | 2688K | MicroPython 固件 |
| model | 0x2B0000 | 3000K | srmodels.bin |
| vfs | 0x59E000 | 1472K | 用户文件系统 |

8MB flash 放不下中文 TTS 音色数据（~3.9MB），TTS 播报暂不可用（16MB 板固件支持）。

## 使用（REPL 示例，立创 ESP32-S3 + INMP441 + MAX98357A）

接线（双 I²S 控制器，收发时钟独立）：

| 模块 | 引脚 | GPIO |
|---|---|---|
| INMP441 | SCK / WS / SD | 16 / 17 / 18 |
| INMP441 | L/R | GND（→ 左槽，slot=0 默认） |
| MAX98357A | BCLK / LRC / DIN | 39 / 40 / 38 |

```python
import asr, time

# 引脚全参数化，按板卡自由配置
asr.init_asr(wake_time=6000, wake_flag=1,
             mic_blck=16, mic_ws=17, mic_din=18, slot=0,
             amp_blck=39, amp_ws=40, amp_dout=38)
asr.start_asr()
time.sleep(2)  # 等模型加载

# 命令词必须在 start_asr() 之后添加（esp-sr 的 MultiNet 实例此时才创建）
asr.add_asr_command(1, "开灯")
asr.add_asr_command(2, "关灯")

def on_command(id):
    print("识别到命令ID:", id)
asr.set_asr_callback(on_command)
```

唤醒词：**"Hi,乐鑫"**（wn9_hilexin 模型）。

## SDK 关键配置（boards/ESP32_GENERIC_S3/sdkconfig.board）

- 唤醒词 `wn9_hilexin`（Hi,乐鑫）+ 命令词 `mn7_cn` 量化 + WebRTC VAD
- esp-sr 预编译库要求：ICache 32KB / DCache 64KB / 64B line
- **esp-sr 挂载方式（重要）**：CI 克隆到工作区 `esp-sr/`，通过环境变量
  `ESP_SR_COMPONENT_DIR` 传入构建，作为**项目组件**挂载（EXTRA_COMPONENT_DIRS）。
  不要放 IDF_PATH/components——IDF_PATH 组件对 bootloader 子构建可见，
  esp-sr 的依赖清单会破坏 bootloader 的组件求解（esp_psram 头文件丢失）
- 构建矩阵：ESP-IDF v5.5.6（与 ESP-VISION 官方一致，便于后续合并视觉）
- 音频硬件：C 模块只管 I²S 总线（双控制器：I2S0 收麦克风 32bit 单声道 /
  I2S1 发功放 16bit 立体声，引脚全参数化）；INMP441/MAX98357A 零配置，
  ES7210/ES8311 等 I²C codec 由 Python 侧驱动先初始化再 `asr.init_asr()`
- 固件合成（makeimg.py）：srmodels/TTS 音色按分区表中的 model/voice_data
  分区自动定位合并，8MB 板无 voice_data 分区则自动跳过 TTS
