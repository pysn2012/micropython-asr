# mic_probe.py — INMP441 接线/槽位自检（独立于 asr 模块，任何 MicroPython 固件可跑）
#
# 用法：烧录/保存到板子后运行，对着麦克风连续说话 1 秒，看输出：
#   左槽(L)有峰值、右槽静   -> 正常（L/R 接地），asr.init_asr 用 slot=0
#   右槽(R)有峰值、左槽静   -> INMP441 的 L/R 悬空/接高了，init_asr 用 slot=1（或把 L/R 真正接地）
#   两槽都静（峰值<几百）   -> 接线问题：查 SCK/WS/SD 三根线与 3.3V 供电
#
# 判定量级参考（说话时）：有效信号峰值应在 100000+（原始 24bit 数据）；静音底噪约几百~几千

import time
from machine import I2S, Pin

# 按实际接线改这三根线
SCK, WS, SD = 16, 17, 18

RATE = 16000
SEC = 1
BYTES = RATE * SEC * 2 * 4   # 立体声 × 32bit

i2s = I2S(0, sck=Pin(SCK), ws=Pin(WS), sd=Pin(SD),
          mode=I2S.RX, bits=32, format=I2S.STEREO, rate=RATE,
          ibuf=BYTES + 4096)

buf = bytearray(BYTES)
mv = memoryview(buf)
got = 0
t0 = time.ticks_ms()
print("采样 1 秒，请对麦克风说话...")
while got < BYTES:
    n = i2s.readinto(mv[got:])
    if n:
        got += n
print("读取 %d 字节，耗时 %d ms" % (got, time.ticks_diff(time.ticks_ms(), t0)))
i2s.deinit()

a = memoryview(buf).cast('i')   # 按 int32 小端解析
peak_l = peak_r = 0
sum_l = sum_r = 0
cnt = got // 8
for i in range(0, got // 4 - 1, 2):
    l = a[i]
    r = a[i + 1]
    if abs(l) > peak_l:
        peak_l = abs(l)
    if abs(r) > peak_r:
        peak_r = abs(r)
    sum_l += abs(l)
    sum_r += abs(r)

print("左槽(L): 峰值 %d, 平均 %d" % (peak_l, sum_l // max(cnt, 1)))
print("右槽(R): 峰值 %d, 平均 %d" % (peak_r, sum_r // max(cnt, 1)))

if peak_l > 100000 and peak_r < 10000:
    print("结论: 数据在左槽 -> init_asr(slot=0) 正确")
elif peak_r > 100000 and peak_l < 10000:
    print("结论: 数据在右槽 -> L/R 未接地，init_asr 改用 slot=1")
elif peak_l < 10000 and peak_r < 10000:
    print("结论: 两槽都静 -> 检查接线(SCK/WS/SD)与供电")
else:
    print("结论: 两槽都有信号 -> 检查 L/R 引脚接法")
