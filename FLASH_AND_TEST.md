# 固件烧录与测试指南

适用硬件：立创ESP32-S3开发板（N8R8：8MB Flash + 8MB Octal PSRAM）+ INMP441麦克风 + MAX98357A功放。

## 1. 固件产物说明

GitHub Actions → `build mpy with asr` → Run workflow，从Artifacts下载：

| 文件 | 大小 | 说明 |
|---|---|---|
| `ESP32_GENERIC_S3_SPIRAM_OCT_8MB.bin` | 5,790,544 字节 | 8MB板固件，**srmodels已合并在内**（唤醒+命令词，无TTS） |
| `ESP32_GENERIC_S3_ASR16M_SPIRAM_OCT_16MB.bin` | 约 16,498,431 字节 | 16MB板固件，含srmodels+TTS音色（全功能） |
| `srmodels.bin` | 2,972,496 字节 | 模型包备用副本（换模型时单独覆盖烧录用，平时不需要） |

验收方法：文件字节数与上表对不上就不要烧，先核对CI构建日志。

## 2. 烧录步骤

模型已按分区表合并进固件，**一条命令烧0x0**：

```
esptool.py --chip esp32s3 --port COM口 --baud 921600 erase_flash
esptool.py --chip esp32s3 --port COM口 --baud 921600 write_flash 0x0 固件文件名.bin
```

- 换COM口为实际端口（设备管理器里看，通常需要CP210x/CH343驱动）
- `erase_flash`只需在换板子或出诡异问题时执行；日常重烧可省略
- 烧完按RST复位，波特率115200打开REPL（Thonny/mpremote-rx均可）

### 16MB板换模型（可选）

模型分区是独立的，换模型不用重烧固件：

```
esptool.py --chip esp32s3 --port COM口 --baud 921600 write_flash 0x810000 新的srmodels.bin
```

8MB板的模型分区在`0x2B0000`。注意：srmodels里的模型名必须与固件Kconfig启用的模型一致（改模型需重新构建固件）。

## 3. 接线

双I²S控制器架构：I2S0收麦克风（32bit单声道），I2S1发功放（16bit立体声），收发时钟完全独立，引脚全参数化。

**8MB板（只做唤醒+命令词识别）只需要INMP441，MAX98357A可以不接**——识别纯靠收音；TTS播报只在16MB板有，需要出声才接功放。

### INMP441麦克风（8MB/16MB板都必接）

| 引脚 | 接ESP32-S3 | 备注 |
|---|---|---|
| VDD / GND | 3.3V / GND | |
| L/R | **GND** | 数据走左槽，对应`slot=0`（默认） |
| SCK | GPIO16 | BCLK |
| WS | GPIO17 | 字选择 |
| SD | GPIO18 | 数据输出 |

### MAX98357A功放（仅16MB板需要，8MB板可省略）

| 引脚 | 接ESP32-S3 | 备注 |
|---|---|---|
| VIN / GND | 5V / GND | |
| BCLK | GPIO39 | 与麦克风不共用 |
| LRC | GPIO40 | 字选择 |
| DIN | GPIO38 | 数据输入 |
| SD / GAIN | 悬空 | SD悬空=(L+R)/2输出；TX左右槽同数据，任意选择都有声 |
| 扬声器 | + / - | MAX98357A输出 | 4Ω/8Ω 2~3W |

不接功放时`init_asr()`省略全部amp参数即可，固件内部不占用I2S1。

## 4. REPL测试流程

### 第一步：验证模块加载

```python
import asr
dir(asr)
```

应列出：`init_asr / start_asr / add_asr_command / set_asr_callback / free_asr / start_tts / add_tts_data / ASR_MODE_SINGLE / ASR_MODE_CONTINUOUS`。

### 第二步：初始化并启动

8MB板（未接功放，省略amp参数）：

```python
import asr, time

asr.init_asr(wake_time=6000, wake_flag=1,
             mic_blck=16, mic_ws=17, mic_din=18, slot=0)
asr.start_asr()
```

16MB板（接了功放，带amp参数）：

```python
asr.init_asr(wake_time=6000, wake_flag=1,
             mic_blck=16, mic_ws=17, mic_din=18, slot=0,
             amp_blck=39, amp_ws=40, amp_dout=38)
asr.start_asr()
```

`start_asr()`时串口会打印模型加载日志，应看到`WN9_HILEXIN`（唤醒词）和`MN7_CN`（中文命令词）相关输出。模型缺失的表现：报模型加载失败或`SR models found`列表为空——说明srmodels没烧上，回第2步。

### 第三步：添加命令词并设回调

```python
# 命令词必须在 start_asr() 之后添加（MultiNet实例此时才创建），ID从1开始，2~5个字
asr.add_asr_command(1, "开灯")
asr.add_asr_command(2, "关灯")
asr.add_asr_command(3, "向前走")
asr.add_asr_command(4, "向后退")

def on_command(id):
    print("识别到命令ID:", id)
asr.set_asr_callback(on_command)
```

### 第四步：语音测试

1. 喊唤醒词 **"Hi,乐鑫"**（wn9_hilexin模型，发音接近"嗨 乐鑫"）
2. 唤醒后说命令词（如"开灯"），REPL应打印`识别到命令ID: 1`
3. `wake_flag=1`（ASR_MODE_SINGLE）：识别一条后回到待机，需重新唤醒
4. `wake_flag=2`（ASR_MODE_CONTINUOUS）：唤醒后连续识别，超时（wake_time ms）后回到待机

### 第五步（仅16MB板）：TTS播报

```python
asr.start_tts()
asr.add_tts_data("你好，我是小乐")
```

8MB板无voice_data分区（也无功放），`start_tts()`会提示`voice_data partition not found`，属预期行为。

### 测完释放资源

```python
asr.free_asr()
```

## 5. 参数速查：`asr.init_asr()`

| 参数 | 默认 | 说明 |
|---|---|---|
| `wake_time` | 6000 | 唤醒后等待命令词的超时（ms） |
| `wake_flag` | 1 | 1=单次识别，2=连续识别 |
| `mic_blck` / `mic_ws` / `mic_din` | -1 | 麦克风BCLK/WS/数据引脚（GPIO号） |
| `slot` | 0 | 麦克风槽位：0=左槽（INMP441的L/R接地），1=右槽（L/R接高电平） |
| `amp_blck` / `amp_ws` / `amp_dout` | -1 | 功放BCLK/WS/数据引脚（GPIO号）；**未接功放可全部省略**，固件不占用I2S1 |

换任何板卡只需改这几个参数，固件不锁引脚。ES7210/ES8311等I²C codec板卡：先用Python侧驱动初始化codec，再调`asr.init_asr()`。

## 6. 故障排查速查

| 现象 | 排查顺序 |
|---|---|
| `import asr`报错 | 固件不含asr模块——确认烧的是本仓库CI产物 |
| `start_asr()`模型加载失败 | srmodels没烧上：整包重烧`write_flash 0x0 固件.bin` |
| 喊唤醒词无反应，REPL正常 | ①确认唤醒词是"Hi,乐鑫"；②`slot`参数与INMP441的L/R接法不匹配（接地=0，接高=1）对调试试；③说话距离拉近、音量加大（增益偏小的备选：C代码feed路径移位`>>14`改小） |
| 命令词添加报错 | 命令词必须在`start_asr()`之后添加；命令词2~5个字 |
| 完全无声（16MB板） | 检查功放BCLK/LRC/DIN三线与5V供电；SD悬空勿接地（接地=关断）；确认`init_asr()`带了amp参数 |
| 识别误触发多 | 提高`wake_time`没有用，属环境噪声/阈值问题：远离噪声源，或缩短`wake_time` |

## 7. 分区布局参考

| 分区 | 8MB板 | 16MB板 |
|---|---|---|
| factory（固件应用） | 0x10000, 2688K | 0x10000, 8192K |
| model（srmodels） | 0x2B0000, 3000K | 0x810000, 3000K |
| voice_data（TTS音色） | 无 | 0xC17000, 3890K |
| vfs（用户文件系统） | 0x59E000, 1472K | 0xAFE000, 1024K |

## 8. 小程掌机（ESP32_GENERIC_S3_XC16M）专用说明

构建工作流：`build xiaocheng handheld firmware`（与主工作流分开，产物名带`XC16M`）。
固件尺寸与16MB板一致（约16,498,431字节，srmodels+TTS音色均合并）。

掌机音频硬件与前两节的INMP441/MAX98357A不同——**ES7210+ES8311双codec、收发共享I²S总线**：

| 项目 | 掌机实测配置 |
|---|---|
| ES7210（收音，I²C 0x43） | DIN=GPIO18，16-bit，fs=MCLK/256 |
| ES8311（播放，I²C 0x18） | DOUT=GPIO13，16-bit立体声 |
| BCLK / WS（收发共享） | GPIO16 / GPIO17 |
| MCLK | GPIO15，**Python侧PWM生成**（256×fs，16k识别用4.096MHz） |
| XL9535（I²C 0x20） | I²C1：SCL=7/SDA=12；AUDIO_PWR=P1.4（先上电），AMP_EN=P0.3 |

固件自动识别共享总线：`amp_blck==mic_blck && amp_ws==mic_ws`时切单控制器全双工
（16-bit立体声@16k，tx/rx同配置）。

识别测试脚本：工作空间`小程ESP32S3掌机/asr_test.py`（含XL9535上电、MCLK PWM、
ES7210寄存器初始化、命令词识别全流程）。

```python
asr.init_asr(wake_time=6000, wake_flag=1,
             mic_blck=16, mic_ws=17, mic_din=18, slot=0, mic_bits=16,
             amp_blck=16, amp_ws=17, amp_dout=13)
```

- `mic_bits=16`：ES7210是16-bit输出，必须显式指定（默认32对应INMP441）
- 共享stereo模式下`slot`指取左右槽哪一路作麦克风（ES7210实际输出槽位以实测为准，无反应就换1）
- ES8311播放/TTS需其时钟分频寄存器匹配16kHz（原寄存器表按24kHz配置），待后续实测调整
