# micropython-asr — MicroPython ESP32-S3 离线语音固件

本仓库是 [micropython/micropython](https://github.com/micropython/micropython) 的 fork，在 ESP32-S3 端口集成 ESP-SR，以用户 C 模块向 Python 暴露 `asr` 模块，提供：

- **离线唤醒**：WakeNet9 "小明同学"（wn9_xiaomingtongxue_tts2，三块板统一）
- **离线中文命令词**：MultiNet7 `mn7_cn` 量化版（命令词传拼音）
- **中文 TTS 播报**：esp-tts xiaoxin 音色（需 16MB Flash 板）

软件栈：MicroPython v1.29 ＋ ESP-IDF v5.5.6 ＋ esp-sr v2.6.1（master，CI 每次 clone 最新）。

## 1. 仓库结构与版本

```
examples/usercmodule/
  asr/                          asr 用户模块（本仓库核心代码）
    mpy_asr.cpp                   核心 C++：I2S 驱动 + feed/detect/tts 任务 + AFE/MN/TTS 生命周期
    mpy_asr.c                     MicroPython 模块封装 + 回调队列任务
    asr_data.h / asr_data.c       数据结构与回调
    micropython.cmake             构建配置（usermod_mpy_asr）
  xiaocheng_modules.cmake       掌机固件用户模块聚合清单（asr + st7789，仅掌机工作流挂载）
  st7789_mpy/                   ST7789 屏幕 C 模块（submodule：russhughes/st7789_mpy，pin 447d5fa）
ports/esp32/
  boards/ESP32_GENERIC_S3/          8MB 开发板（sdkconfig.board 选模型）
  boards/ESP32_GENERIC_S3_ASR16M/   16MB 开发板
  boards/ESP32_GENERIC_S3_XC16M/    小程掌机
  partitions-8MiB.csv / -16MiB.csv  分区表（model/voice_data 分区）
  Makefile                          构建入口 + 固件合成（makeimg.py，TTS 音色 .dat 在此指定）
.github/workflows/
  build esp32 firmware.yml          产出 8MB + ASR16M 固件（手动触发）
  build xiaocheng handheld firmware.yml  产出掌机固件（手动触发，额外挂载 ST7789）
```

**MicroPython 版本选择**：本仓库是完整 fork，版本即 fork 时点的上游版本（当前 v1.29，见 `py/mpconfig.h` 的 `MICROPY_VERSION_MAJOR/MINOR`），**没有配置项可选**。换版本 = 合并上游 tag：

```bash
git remote add upstream https://github.com/micropython/micropython.git
git fetch upstream --tags
git merge v1.28.0        # 换成目标版本 tag；本仓库改过的文件（ports/esp32/Makefile 等）可能有冲突
```

合并后 push 即可，CI 编译的就是新版本。**注意**：`.mpy` 字节码与固件版本相关，跨版本需重编。

**esp-sr 挂载方式（重要）**：CI 克隆 esp-sr 到工作区，经环境变量 `ESP_SR_COMPONENT_DIR` 作为**项目组件**挂载（EXTRA_COMPONENT_DIRS）。不要放 `IDF_PATH/components`——esp-sr 的依赖清单会破坏 bootloader 子构建的组件求解。

## 2. 适配板卡

| 板卡 | 板名（boards/） | Flash | 音频硬件 | CI 工作流 |
|---|---|---|---|---|
| 8MB 立创开发板 | ESP32_GENERIC_S3 | 8MB | INMP441（无功放，TTS 不可用） | build esp32 firmware |
| 16MB 开发板 | ESP32_GENERIC_S3_ASR16M | 16MB | INMP441 + MAX98357A | build esp32 firmware |
| 小程掌机 | ESP32_GENERIC_S3_XC16M | 16MB | ES7210 + ES8311 + NS4152（共享 I²S） | build xiaocheng handheld firmware |

三块板验证状态：8MB 板与掌机全链路（唤醒+命令词+掌机 TTS/灯控）已板上验证通过；16MB 板硬件同 8MB，固件就绪待烧测。

## 3. 编译

**CI 编译（推荐）**：GitHub Actions 手动触发对应工作流，Artifacts 下载固件 zip。

**本地编译**：

```bash
git submodule update --init
make -C mpy-cross
git clone https://github.com/espressif/esp-sr.git esp-sr        # 与 CI 同布局
source esp-idf/export.sh                                        # 需 ESP-IDF v5.5.6
cd ports/esp32
ESP_SR_COMPONENT_DIR=$(pwd)/../../esp-sr make BOARD=ESP32_GENERIC_S3_XC16M BOARD_VARIANT=SPIRAM_OCT
```

esp-sr 预编译库要求：ICache 32KB / DCache 64KB / 64B line（各板 sdkconfig.board 已配）。

## 4. 烧录与测试

### 4.1 烧录（一条命令）

固件已合并 bootloader/分区表/应用/srmodels/TTS 音色：

```bash
esptool.py --chip esp32s3 --port COM口 --baud 921600 write_flash 0x0 固件.bin
```

`erase_flash` 只在换板或出诡异问题时执行。模型超出分区容量时 makeimg.py 构建期直接报错，不会静默烧坏。

### 4.2 分区布局

| 分区 | 8MB 板 | 16MB 板（含掌机） |
|---|---|---|
| factory（应用） | 0x10000, 2688K | 0x10000, 8192K |
| model（srmodels） | 0x2B0000, 3000K | 0x810000, 3000K |
| voice_data（TTS 音色） | 无 | 0xC17000, 3890K |
| vfs（用户文件系统） | 0x59E000, 1472K | 0xAFE000, 1024K |

### 4.3 接线

**8MB/16MB 开发板**（双 I²S 控制器，收发独立，引脚全参数化）：

| 模块 | 引脚 | GPIO |
|---|---|---|
| INMP441 | SCK / WS / SD | 16 / 17 / 18 |
| INMP441 | L/R | GND（左槽，slot=0） |
| MAX98357A | BCLK / LRC / DIN | 39 / 40 / 38（仅 16MB 板，SD/GAIN 悬空） |

**小程掌机**（ES7210+ES8311 双 codec，收发共享 I²S 总线，固件在 `amp_blck==mic_blck` 时自动切单控制器全双工）：

| 器件 | 地址/引脚 | 说明 |
|---|---|---|
| ES7210（收音） | I²C 0x43，DIN=GPIO18 | 16bit，MIC1 增益 30dB，fs=MCLK/256 |
| ES8311（播放） | I²C 0x18，SDIN=GPIO13 | 16k 播放配置用 `es8311_16k.py`（16k 与 24k 的 REG04 差异见该文件注释） |
| 共享 I²S | BCLK=16 / WS=17 | MCLK=GPIO15，Python 侧 PWM 生成 4.096MHz（256×16k） |
| XL9535 | I²C 0x20（SCL=7/SDA=12） | **AUDIO_PWR=P1.4 先上电；NS4152 功放使能 AMP_EN=P0.3 必须单独 `xl.output(AMP_EN,1)`，否则无声** |

### 4.4 asr 模块 API

```python
import asr

asr.init_asr(wake_time=6000, wake_flag=1,     # 1=单次(每命令后重新等唤醒) 2=连续(窗口内连续识别)
             mic_blck=16, mic_ws=17, mic_din=18,    # 麦克风 I2S 引脚
             slot=0,          # 0=左槽(INMP441 L/R接地) 1=右槽
             mic_bits=32,     # 32=INMP441  16=ES7210 等 codec
             amp_blck=-1, amp_ws=-1, amp_dout=-1)  # 功放引脚；未接可全省略
asr.start_asr()                # 创建识别任务，约 3~5s 后就绪
asr.ready()                    # -> True 后才能加命令词
asr.add_asr_command(1, "kai deng")   # mn7_cn 中文命令词必须传拼音，传汉字报 ret=259
asr.set_asr_callback(cb)       # cb(id) 收命令词 ID
asr.start_tts()                # TTS 初始化（需 voice_data 分区；必须在 add_tts_data 之前）
asr.add_tts_data("灯已打开")    # 中文 UTF-8，esp-tts 播报
asr.free_asr()                 # 释放；可反复 init/start，自动回收旧实例
```

I²C codec（ES7210/ES8311）由 Python 侧驱动先初始化，再调 `asr.init_asr()`；固件只管 I²S 总线，不锁引脚。

### 4.5 测试脚本（工作区根目录，Thonny 直接运行）

| 脚本 | 板卡 | 内容 |
|---|---|---|
| `asr_test_8m.py` | 8MB 板 | 基础识别（唤醒+命令词） |
| `asr_test.py` | 掌机 | 基础识别（含 XL9535 上电、MCLK PWM、ES7210 初始化、16k 链路检查） |
| `asr_test_ws2812_tts.py` | 掌机 | 命令动作：开灯/关灯→WS2812(GPIO42,10灯)+TTS，拍照→TTS |
| `tts_probe.py` | 掌机 | 播放硬件探针：machine.I2S 直推 ES8311 播哔声，隔离硬件/固件问题 |

驱动文件在仓库外 `驱动/` 文件夹（xl9535.py / es7210.py / es8311.py / es8311_16k.py），前两者与 es8311_16k.py 需上传到掌机板子。

### 4.6 固件日志关键行

| 日志 | 含义 |
|---|---|
| `[ASR] models loaded: 3` | wn9 / fst / mn7_cn 全加载 |
| `[ASR] mn created: heap_free=...` | MN 模型创建成功 + 资源余量 |
| `[ASR] cmd add id=1 ret=0` + `cmd list` | 命令词写入成功并回显清单 |
| `[ASR] wake detected` | 唤醒词触发 |
| `[ASR] mic peak=X`（每 3 秒） | 说话应上千，静音约 100 |

默认固件只输出 ERROR 级 esp-sr 日志；要看模型加载 INFO 日志：`import esp; esp.osdebug(False)`。

## 5. 更换唤醒词 / TTS 音色（编译期配置，改完需重编译）

**换唤醒词**：改对应板子的 `ports/esp32/boards/<板名>/sdkconfig.board` 一行，如：

```
CONFIG_SR_WN_WN9_XIAOMINGTONGXUE_TTS2=y   # 小明同学
```

符号规则：esp-sr 模型文件夹 `wn9_xxx` → 符号 `SR_WN_WN9_XXX`（大写）。候选清单见 esp-sr 仓库 `wakeword_list.md`（60+ 个），符号须存在于 `Kconfig.projbuild` 的 "Load Multiple Wake Words" 菜单。官方多数模型为 TTS 流水线生成，泛化性能需上板实测。自制唤醒词走乐鑫官方免费定制训练（周期约 2~4 周），模型文件夹进仓库后用 `CONFIG_ESP_SR_EXTERNAL_MODEL_PATH` 指向。wn10 代模型算力开销大，与 `AFE_MODE_LOW_COST` 兼容性未验证。Python 侧零改动。

**换 TTS 音色**：改 `ports/esp32/Makefile` 末尾的 .dat 路径：`xiaoxin`（小鑫 3.64MB，当前）/ `xiaole`（小乐女声 2.87MB）/ `xiaoxin_small`（2.78MB）。掌机 3890K 分区均可容纳。代码用 `esp_tts_voice_template`+分区 mmap，换音色无需改 C 代码。仅对有 voice_data 分区的板生效；不支持自定义录音音色。

## 6. 故障排查速查

| 现象 | 排查 |
|---|---|
| `import asr` 报错 | 固件不含 asr 模块——确认烧的是本仓库 CI 产物 |
| 模型加载失败 | `[ASR] model partition empty`＝srmodels 没烧上（整包重烧 0x0）；`no chinese MN model`＝srmodels 与 sdkconfig 不匹配 |
| 唤醒无反应 | ①确认唤醒词与固件 sdkconfig 一致；②`slot` 与 L/R 接法不匹配（接地=0）对调；③距离/音量 |
| 命令词添加失败 | 必须在 `asr.ready()` 为 True 后、`start_asr()` 之后添加；传拼音；2~5 个字 |
| 掌机完全无声 | **先查 AMP_EN(P0.3) 是否使能**；再查 ES8311 寄存器回读、MCLK PWM、跑 `tts_probe.py` 定位 |
| 识别误触发多 | 环境噪声问题：远离噪声源、缩短 `wake_time` |

## 7. 上游文档

MicroPython 本体的通用文档（REPL、语言特性、其他端口）见 [docs.micropython.org](https://docs.micropython.org) 与上游仓库 README 历史。
