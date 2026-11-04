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

| 模块 | 引脚 | 接ESP32-S3 | 备注 |
|---|---|---|---|
| INMP441 | VDD / GND | 3.3V / GND | |
| INMP441 | L/R | **GND** | 数据走左槽，对应`slot=0`（默认） |
| INMP441 | SCK | GPIO16 | BCLK |
| INMP441 | WS | GPIO17 | 字选择 |
| INMP441 | SD | GPIO18 | 数据输出 |
| MAX98357A | VIN / GND | 5V / GND | |
| MAX98357A | BCLK | GPIO39 | 与麦克风不共用 |
| MAX98357A | LRC | GPIO40 | 字选择 |
| MAX98357A | DIN | GPIO38 | 数据输入 |
| MAX98357A | SD / GAIN | 悬空 | SD悬空=(L+R)/2输出；TX左右槽同数据，任意选择都有声 |
| 扬声器 | + / - | MAX98357A输出 | 4Ω/8Ω 2~3W |

## 4. REPL测试流程

### 第一步：验证模块加载

```python
import asr
dir(asr)
```

应列出：`init_asr / start_asr / add_asr_command / set_asr_callback / free_asr / start_tts / add_tts_data / ASR_MODE_SINGLE / ASR_MODE_CONTINUOUS`。

### 第二步：初始化并启动

```python
import asr, time

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

8MB板无voice_data分区，`start_tts()`会提示无法映射分区，属预期行为。

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
| `amp_blck` / `amp_ws` / `amp_dout` | -1 | 功放BCLK/WS/数据引脚（GPIO号） |

换任何板卡只需改这几个参数，固件不锁引脚。ES7210/ES8311等I²C codec板卡：先用Python侧驱动初始化codec，再调`asr.init_asr()`。

## 6. 故障排查速查

| 现象 | 排查顺序 |
|---|---|
| `import asr`报错 | 固件不含asr模块——确认烧的是本仓库CI产物 |
| `start_asr()`模型加载失败 | srmodels没烧上：整包重烧`write_flash 0x0 固件.bin` |
| 喊唤醒词无反应，REPL正常 | ①确认唤醒词是"Hi,乐鑫"；②`slot`参数与INMP441的L/R接法不匹配（接地=0，接高=1）对调试试；③说话距离拉近、音量加大（增益偏小的备选：C代码feed路径移位`>>14`改小） |
| 命令词添加报错 | 命令词必须在`start_asr()`之后添加；命令词2~5个字 |
| 完全无声 | 检查功放BCLK/LRC/DIN三线与5V供电；SD悬空勿接地（接地=关断） |
| 识别误触发多 | 提高`wake_time`没有用，属环境噪声/阈值问题：远离噪声源，或缩短`wake_time` |

## 7. 分区布局参考

| 分区 | 8MB板 | 16MB板 |
|---|---|---|
| factory（固件应用） | 0x10000, 2688K | 0x10000, 8192K |
| model（srmodels） | 0x2B0000, 3000K | 0x810000, 3000K |
| voice_data（TTS音色） | 无 | 0xC17000, 3890K |
| vfs（用户文件系统） | 0x59E000, 1472K | 0xAFE000, 1024K |
