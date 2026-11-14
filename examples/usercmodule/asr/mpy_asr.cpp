// 在包含任何头文件之前，先处理 ESP-IDF 宏问题

// 首先包含 MicroPython 相关头文件
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mpstate.h"

// 标准库头文件
#include <string>
#include <vector>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// ESP-IDF 头文件
extern "C" {
    #include "esp_log.h"
    #include "esp_system.h"
    #include "esp_heap_caps.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
    #include "freertos/task.h"
    #include "freertos/queue.h"
    #include "driver/i2s_std.h"
    #include "driver/gpio.h"
    #include "driver/i2c.h"

    #include "esp_process_sdkconfig.h"
    #include "esp_wn_iface.h"
    #include "esp_wn_models.h"
    #include "esp_afe_sr_iface.h"
    #include "esp_afe_sr_models.h"
    #include "esp_mn_iface.h"
    #include "esp_mn_models.h"
    #include "model_path.h"
    #include "esp_mn_speech_commands.h"
    #include "esp_tts.h"
    #include "esp_tts_voice_xiaole.h"
    #include "esp_tts_voice_template.h"
    #include "esp_tts_player.h"
    #include "esp_partition.h"
}

// 项目头文件
#include "asr_data.h"

extern "C" int audio_capture_flag;

extern "C" void audio_push_result(asr_data_obj_t *data);

extern "C" int free_task;
extern "C" int cb_free_task;

extern "C" QueueHandle_t tts_data_queue;

static asr_data_obj_t g_asr_data;
static int wake_time = 0;
static int wake_flag = 0;
static TaskHandle_t tts_task_handle = NULL;

static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;

static const esp_afe_sr_iface_t *afe_handle = NULL;
srmodel_list_t *models = NULL;


static int wakeup_flag = 0;

// MultiNet 就绪标志：esp_mn_commands_alloc 成功后才允许 add_asr_command。
// 否则 esp_mn_commands_add 只会返回晦涩的 ESP_ERR_INVALID_STATE（源码 esp_mn_speech_commands.c:
// "NULL == esp_mn_root ... Please create mn model first"），且 REPL 在 MN 量化完成前
// 调用 add 是常见时序，必须显式拦截。
static volatile int g_mn_ready = 0;

// 任务生命周期状态（soft reboot 不会杀 FreeRTOS 任务，重复运行 init/start 必须先回收旧实例，
// 否则 i2s_new_channel 在已占用的 I2S0 上失败 -> ESP_ERROR_CHECK abort -> 直接崩溃）
static esp_afe_sr_data_t *g_afe_data = NULL;  // AFE 实例，由 free_asr 统一销毁
static volatile int g_detect_exited = 1;      // 1 = detect 任务不在运行
static volatile int g_feed_exited = 1;        // 1 = feed 任务不在运行

// 麦克风输入模式（init_asr 时确定）
enum {
    MIC_MODE_32BIT_MONO = 0,  // 独立总线 + 32bit 单声道（INMP441，K10 沿用的移位技巧）
    MIC_MODE_16BIT_MONO,      // 独立总线 + 16bit 单声道（ES7210 等 codec 单独接）
    MIC_MODE_16BIT_STEREO,    // 共享总线全双工（ES7210/ES8311 共用 BCLK/WS，掌机类硬件）
};
static int g_mic_mode = MIC_MODE_32BIT_MONO;
static int g_mic_slot = 0;    // 共享 stereo 模式取哪个槽：0=左 1=右

void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t*)arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel = 2; // "MN" 输入格式：[麦克风, 未用通道]
    mp_print_asr("[ASR] feed task started, chunksize=%d, mode=%d(0=32bit 1=16bit-mono 2=16bit-stereo)\n",
                 audio_chunksize, g_mic_mode);
    // 读缓冲按最大需求分配：32bit 单声道与共享 stereo 模式均为 chunksize*4 字节
    int16_t *i2s_buff = (int16_t *)malloc(audio_chunksize * 4);
    int16_t *feed_buf = (int16_t *)malloc(audio_chunksize * feed_channel * sizeof(int16_t));
    assert(i2s_buff && feed_buf);
    size_t r_bytes = 0;
    int16_t *fed = NULL;      // 本轮实际喂给 AFE 的缓冲（用于电平统计）
    TickType_t last_peak_tick = 0;  // 电平打印节拍（3 秒一次）
    int short_reads = 0;      // I2S 短读计数（读不满=DMA欠载）

    while (1) {
        // 等 detect 任务先退出（cb_free_task）：detect 退出前 feed 必须继续喂数据，
        // 否则 detect 阻塞在 fetch 空环形缓冲上永远退不出来
        if (cb_free_task == 1) {
            break;
        }
        // 读字节数按模式区分：32bit 单声道=chunk*4；16bit 单声道=chunk*2
        // （原版一律 chunk*4，16bit 单声道会把 2 倍时长的音频当 1 块喂，节奏错一倍）；
        // 16bit stereo=chunk*4（chunk 组 L/R 对）
        size_t want = (g_mic_mode == MIC_MODE_16BIT_MONO) ? (size_t)audio_chunksize * 2
                                                          : (size_t)audio_chunksize * 4;
        i2s_channel_read(rx_handle, i2s_buff, want, &r_bytes, portMAX_DELAY);
        if (r_bytes != want) {
            short_reads++;
        }

        if (g_mic_mode == MIC_MODE_32BIT_MONO) {
            // INMP441：32bit 槽内有效位在 29:13，>>14 后原地变成
            // [样本, 符号扩展] 交错的 chunksize*2 个 int16 —— 正好是
            // [麦克风, 未用通道] 的 feed 布局（K10 沿用的移位技巧，同时完成增益放大）
            int32_t *tmp_buff = (int32_t*)i2s_buff;
            for (int i = 0; i < audio_chunksize; i++) {
                tmp_buff[i] = tmp_buff[i] >> 14;
            }
            fed = (int16_t*)tmp_buff;
            afe_handle->feed(afe_data, fed);
        } else if (g_mic_mode == MIC_MODE_16BIT_MONO) {
            for (int i = 0; i < audio_chunksize; i++) {
                feed_buf[2 * i]     = i2s_buff[i];
                feed_buf[2 * i + 1] = 0;
            }
            fed = feed_buf;
            afe_handle->feed(afe_data, fed);
        } else { // MIC_MODE_16BIT_STEREO
            // [L,R] 交错，取 slot 指定的槽作为麦克风，另一槽未用
            for (int i = 0; i < audio_chunksize; i++) {
                feed_buf[2 * i]     = i2s_buff[2 * i + g_mic_slot];
                feed_buf[2 * i + 1] = 0;
            }
            fed = feed_buf;
            afe_handle->feed(afe_data, fed);
        }

        // 每 3 秒打印一次麦克风通道峰值（诊断：确认语音是否到达 AFE、量级是否正常）。
        // 1 秒一打会与 detect 任务/REPL 输出互相打断，日志全是碎片没法读。
        TickType_t now_tick = xTaskGetTickCount();
        if (now_tick - last_peak_tick >= pdMS_TO_TICKS(3000)) {
            last_peak_tick = now_tick;
            int peak = 0;
            for (int i = 0; i < audio_chunksize; i++) {
                int v = fed[2 * i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            if (peak < 100) {
                mp_print_asr("[ASR] mic peak=%d short_read=%d (无有效信号! 检查接线; "
                             "数据可能在另一槽, init_asr 改 slot=1)\n", peak, short_reads);
            } else {
                mp_print_asr("[ASR] mic peak=%d short_read=%d\n", peak, short_reads);
            }
            short_reads = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (i2s_buff) {
        free(i2s_buff);
        i2s_buff = NULL;
    }
    if (feed_buf) {
        free(feed_buf);
        feed_buf = NULL;
    }
    g_feed_exited = 1;  // 通知 free_asr 本任务已退出
    vTaskDelete(NULL);
}

void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t*)arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mn_name == NULL) {
        // 无中文命令词模型：明确报错并退出任务，避免空指针崩溃
        mp_print_asr("[ASR] ERROR: no chinese MN model! "
                     "srmodels.bin does not match sdkconfig (expect mn7_cn)\n");
        cb_free_task = 1;
        g_detect_exited = 1;
        vTaskDelete(NULL);
        return;
    }
    mp_print_asr("[ASR] detect task started, mn=%s, chunksize=%d\n", mn_name, afe_chunksize);
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, wake_time);
    // create 会加载/量化 MN 模型（掌机板曾在此阶段崩溃），必须判空：
    // 返回 NULL 时后续 esp_mn_commands_alloc(multinet, NULL) 会在闭源库内直接空引用
    if (model_data == NULL) {
        mp_print_asr("[ASR] ERROR: multinet->create NULL! "
                     "heap_free=%d psram_free=%d stack_hwm=%d\n",
                     (int)esp_get_free_heap_size(),
                     (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (int)uxTaskGetStackHighWaterMark(NULL));
        cb_free_task = 1;
        g_detect_exited = 1;
        vTaskDelete(NULL);
        return;
    }
    mp_print_asr("[ASR] mn created: heap_free=%d psram_free=%d stack_hwm=%d\n",
                 (int)esp_get_free_heap_size(),
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (int)uxTaskGetStackHighWaterMark(NULL));
    if (esp_mn_commands_alloc(multinet, model_data) != ESP_OK) {
        mp_print_asr("[ASR] ERROR: esp_mn_commands_alloc FAILED\n");
        multinet->destroy(model_data);
        cb_free_task = 1;
        g_detect_exited = 1;
        vTaskDelete(NULL);
        return;
    }
    g_mn_ready = 1;  // 此后 add_asr_command 才可用

    while (1) {
        if (free_task == 1) {
            break;
        }
        afe_fetch_result_t* res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            static int fetch_err_cnt = 0;
            if (++fetch_err_cnt % 100 == 1) {
                mp_print_asr("[ASR] fetch error! res=%d ret=%d\n",
                             (int)(long)res, res ? (int)res->ret_value : -1);
            }
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            mp_print_asr("[ASR] wake detected\n");
	        multinet->clean(model_data);
        }

        if (res->raw_data_channels == 1 && res->wakeup_state == WAKENET_DETECTED) {
            wakeup_flag = 1;
        } else if (res->raw_data_channels > 1 && res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            wakeup_flag = 1;
        }

        if (wakeup_flag == 1) {
            esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

            if (mn_state == ESP_MN_STATE_DETECTING) {
                continue;
            }

            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                mp_print_asr("[ASR] command detected, id=%d\n", (int)mn_result->command_id[0]);
                g_asr_data.audio_id = mn_result->command_id[0];
                audio_push_result(&g_asr_data);
                if (wake_flag == ASR_MODE_SINGLE) {
                    esp_mn_results_t *mn_result = multinet->get_results(model_data);
                    afe_handle->enable_wakenet(afe_data);
                    wakeup_flag = 0;
                }
                
            }

            if (mn_state == ESP_MN_STATE_TIMEOUT) {
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                mp_print_asr("[ASR] command timeout (back to wake standby)\n");
                afe_handle->enable_wakenet(afe_data);
                wakeup_flag = 0;
                continue;
            }
        }
         vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (model_data) {
        multinet->destroy(model_data);
        model_data = NULL;
    }
    // 注意：afe_data 不能在这里销毁——feed 任务此时可能还在跑最后一段 feed。
    // AFE 实例由 free_asr 在确认所有任务退出后统一销毁。
    cb_free_task = 1;        // 通知 feed / 回调任务退出
    g_detect_exited = 1;     // 通知 free_asr 本任务已退出
    vTaskDelete(NULL);
}

extern "C" __attribute__((weak)) void init_asr(int time, int flag,
        int mic_blck, int mic_ws, int mic_din, int slot, int mic_bits,
        int amp_blck, int amp_ws, int amp_dout) {
    wake_time = time;
    wake_flag = flag;
    g_mic_slot = (slot == 1) ? 1 : 0;
    mp_print_asr("[ASR] init_asr: mic(bclk=%d, ws=%d, din=%d, slot=%d, bits=%d) "
                 "amp(bclk=%d, ws=%d, dout=%d)\n",
                 mic_blck, mic_ws, mic_din, slot, mic_bits,
                 amp_blck, amp_ws, amp_dout);

    // 共享总线检测：功放与麦克风共用 BCLK/WS（掌机类硬件，ES7210+ES8311 同一 I2S）
    bool shared_bus = (amp_dout != -1 && amp_blck == mic_blck && amp_ws == mic_ws);
    mp_print_asr("[ASR] bus: %s\n", shared_bus ? "shared full-duplex I2S0" :
                 (amp_dout != -1 ? "independent (RX=I2S0, TX=I2S1)" : "RX only (I2S0)"));

    if (shared_bus) {
        // ---- 共享总线全双工：单控制器 I2S0，tx/rx 共用同一 std 配置 ----
        // 16bit 立体声 @16kHz，tx/rx 同配置保证 BCLK/WS 完全一致（沿用 K10 板的同构拓扑）。
        // MCLK 不由固件输出——这类板卡的 MCLK 由 Python 侧 PWM 生成（256×fs）
        g_mic_mode = MIC_MODE_16BIT_STEREO;
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC,
                .bclk = GPIO_NUM_NC,
                .ws = GPIO_NUM_NC,
                .dout = GPIO_NUM_NC,
                .din = GPIO_NUM_NC,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        if (mic_blck != -1) std_cfg.gpio_cfg.bclk = (gpio_num_t)mic_blck;
        if (mic_ws  != -1) std_cfg.gpio_cfg.ws   = (gpio_num_t)mic_ws;
        if (mic_din != -1) std_cfg.gpio_cfg.din  = (gpio_num_t)mic_din;
        if (amp_dout != -1) std_cfg.gpio_cfg.dout = (gpio_num_t)amp_dout;
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
        return;
    }

    // ---- 独立总线 RX：麦克风，I2S0，16kHz，位宽由 mic_bits 决定 ----
    // 32bit（默认，INMP441）：L/R 接地 -> 数据在左槽；接高电平 -> 数据在右槽
    // 16bit（ES7210 等 codec 单独接总线）
    g_mic_mode = (mic_bits == 16) ? MIC_MODE_16BIT_MONO : MIC_MODE_32BIT_MONO;
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle));

    i2s_data_bit_width_t rx_bits = (mic_bits == 16) ? I2S_DATA_BIT_WIDTH_16BIT
                                                    : I2S_DATA_BIT_WIDTH_32BIT;
    i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(rx_bits, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = GPIO_NUM_NC,
            .ws = GPIO_NUM_NC,
            .dout = GPIO_NUM_NC,
            .din = GPIO_NUM_NC,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_cfg.slot_cfg.slot_mask = (slot == 1) ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
    if (mic_blck != -1) rx_cfg.gpio_cfg.bclk = (gpio_num_t)mic_blck;
    if (mic_ws  != -1) rx_cfg.gpio_cfg.ws   = (gpio_num_t)mic_ws;
    if (mic_din != -1) rx_cfg.gpio_cfg.din  = (gpio_num_t)mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    // ---- 独立总线 TX：功放（MAX98357A），I2S1，16bit 立体声（左右槽同数据），16kHz ----
    // 立体声复制是为了兼容 SD 引脚的任意通道选择（左/右/(L+R)/2 都有声）
    // 未接功放时（amp_dout=-1，如 8MB 板只做识别）跳过，I2S1 不占用
    if (amp_dout != -1) {
        i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL));

        i2s_std_config_t tx_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC,
                .bclk = GPIO_NUM_NC,
                .ws = GPIO_NUM_NC,
                .dout = GPIO_NUM_NC,
                .din = GPIO_NUM_NC,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        if (amp_blck != -1) tx_cfg.gpio_cfg.bclk = (gpio_num_t)amp_blck;
        if (amp_ws   != -1) tx_cfg.gpio_cfg.ws   = (gpio_num_t)amp_ws;
        tx_cfg.gpio_cfg.dout = (gpio_num_t)amp_dout;
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    }
}

// 音频采集任务
extern "C" __attribute__((weak)) void create_asr(void) {

    // 重复启动保护：任务还在跑时直接忽略（Thonny 里重复执行 start_asr 会创建重复任务）
    if (!g_detect_exited) {
        mp_print_asr("[ASR] start_asr 忽略: 任务已在运行, 先执行 asr.free_asr()\n");
        return;
    }
    g_detect_exited = 0;
    g_feed_exited = 0;
    g_mn_ready = 0;  // 新一轮初始化，MN 未就绪
    models = esp_srmodel_init("model");
    if (models == NULL || models->num == 0) {
        // 模型分区缺失/为空：明确报错并退出，避免后续空指针崩溃
        mp_print_asr("[ASR] ERROR: model partition empty! "
                     "srmodels.bin not flashed or invalid\n");
        g_detect_exited = 1;  // 任务没创建成功，复位运行标志
        g_feed_exited = 1;
        return;
    }
    mp_print_asr("[ASR] models loaded: %d\n", models->num);
    for (int i = 0; i < models->num; i++) {
        mp_print_asr("  [ASR] model[%d]: %s\n", i, models->model_name[i]);
    }

    afe_config_t *afe_config = afe_config_init("MN", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    // 配置回读：把 afe_config_init 实际生效的开关打印出来（AFE 是闭源库，只能这样看）
    mp_print_asr("[ASR] afe cfg: aec=%d se=%d ns=%d vad=%d wakenet=%d agc=%d gain=%d/100\n",
                 (int)afe_config->aec_init, (int)afe_config->se_init,
                 (int)afe_config->ns_init, (int)afe_config->vad_init,
                 (int)afe_config->wakenet_init, (int)afe_config->agc_init,
                 (int)(afe_config->afe_linear_gain * 100));
    mp_print_asr("[ASR] afe pcm: total_ch=%d mic=%d ref=%d rate=%d\n",
                 afe_config->pcm_config.total_ch_num, afe_config->pcm_config.mic_num,
                 afe_config->pcm_config.ref_num, afe_config->pcm_config.sample_rate);
    if (afe_config->wakenet_model_name) {
        mp_print_asr("[ASR] wakenet model: %s\n", afe_config->wakenet_model_name);
    } else {
        mp_print_asr("[ASR] ERROR: wakenet_model_name is NULL!\n");
    }
    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (afe_data == NULL) {
        mp_print_asr("[ASR] ERROR: AFE create_from_config NULL! "
                     "heap_free=%d psram_free=%d\n",
                     (int)esp_get_free_heap_size(),
                     (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        g_detect_exited = 1;  // 任务没创建成功，复位运行标志
        g_feed_exited = 1;
        return;
    }
    g_afe_data = afe_data;  // 交给 free_asr 统一销毁

    xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void*)afe_data, 5, NULL, 1);
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void*)afe_data, 5, NULL, 0);

}

extern "C" __attribute__((weak)) void add_asr_command(int id, const char *command){
    if (!g_mn_ready) {
        mp_print_asr("[ASR] cmd id=%d 未就绪: MN 还在创建模型, start_asr 后等 3 秒再添加\n", id);
        return;
    }
    esp_err_t ret = esp_mn_commands_add(id, command);
    // 单行短格式：与其他任务的输出交错后仍可辨认（ret=0 即成功）
    mp_print_asr("[ASR] cmd add id=%d ret=%d (0=ok) %s\n", id, (int)ret, command);
    // 注意：esp_mn_commands_update 返回 esp_mn_error_t*（成功为 NULL），不是 esp_err_t。
    // esp_mn_error_t 仅有 int16_t num 与 esp_mn_phrase_t **phrases 两个字段（esp_mn_iface.h L67-70）。
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err != NULL) {
        mp_print_asr("[ASR] cmd update: %d 个词未能写入模型\n", (int)err->num);
        free(err);
    }
    // 回显 MN 当前实际的命令词清单（esp_mn_commands_print 走 LOGI 会被日志级别吞掉，
    // 改用 esp_mn_commands_get_from_index 遍历链表，直接给出 MN 到底认得哪些命令）
    int total = 0;
    for (int i = 0; i < ESP_MN_MAX_PHRASE_NUM; i++) {
        esp_mn_phrase_t *p = esp_mn_commands_get_from_index(i);
        if (p == NULL) break;
        mp_print_asr("[ASR] cmd list[%d]: id=%d %s\n", i, (int)p->command_id, p->string);
        total++;
    }
    mp_print_asr("[ASR] cmd total=%d\n", total);
}

extern "C" __attribute__((weak)) void free_asr(void){
    g_mn_ready = 0;  // 停止接受命令词添加
    // 1. 请求 detect 任务退出（feed 任务此时仍继续喂数据，保证 detect 的 fetch 不会
    //    阻塞在空环形缓冲上退不出来——这正是旧版"free 后任务永远杀不掉"的原因）
    free_task = 1;
    for (int i = 0; i < 60 && g_detect_exited == 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < 60 && g_feed_exited == 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (g_detect_exited == 0 || g_feed_exited == 0) {
        mp_print_asr("[ASR] WARN: 任务未在 600ms 内退出, 跳过资源释放(泄漏但不崩溃)\n");
        return;
    }
    // 2. 所有任务已确认退出，安全销毁 AFE 实例与 I2S
    if (g_afe_data && afe_handle) {
        afe_handle->destroy(g_afe_data);
        g_afe_data = NULL;
    }
    if(rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
    if(tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    if(tts_task_handle) {
        vTaskDelete(tts_task_handle);
        tts_task_handle = NULL;
    }

     // 5. 等待一小段时间确保资源完全释放
     vTaskDelay(pdMS_TO_TICKS(10));
}

// 供 Python 层轮询 MN 就绪状态（asr.ready()）
extern "C" int asr_mn_ready(void){
    return g_mn_ready;
}

// 供 Python 层查询唤醒状态（asr.wake_state()）：1=已唤醒（命令窗口开启），0=待唤醒
extern "C" int asr_wake_state(void){
    return wakeup_flag;
}

esp_err_t bsp_audio_play(const int16_t* data, int length)
{
    // length 为字节数；TX 为 16bit 立体声，每个采样复制到左右两个槽
    if (!tx_handle) {
        return ESP_ERR_INVALID_STATE; // 未接功放（init_asr 时省略 amp 参数）
    }
    int nsamp = length / (int)sizeof(int16_t);
    int16_t *out = (int16_t*)malloc(nsamp * 2 * sizeof(int16_t));
    if (!out) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < nsamp; i++) {
        out[2 * i]     = data[i]; // 左槽
        out[2 * i + 1] = data[i]; // 右槽
    }
    size_t bytes_write = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, out, nsamp * 2 * sizeof(int16_t), &bytes_write, portMAX_DELAY);
    free(out);
    return ret;
}

void tts_Task(void *arg)
{
    esp_tts_handle_t *tts_handle = (esp_tts_handle_t *)arg;
    const char *data;
    while (1) {
        if (xQueueReceive(tts_data_queue, &data, portMAX_DELAY)) {
            if(esp_tts_parse_chinese(tts_handle, data)){
                int len[1]={0};
                do{
                    short *mp_data = esp_tts_stream_play(tts_handle, len, 3);
                    bsp_audio_play(mp_data, len[0] * 2);
                }while(len[0]>0);
            }
            free((void*)data);
            esp_tts_stream_reset(tts_handle);
            
            for(int i = 0; i < 10; i++) {
                int16_t silence[256];   // 静音数据
                memset(silence, 0x00, sizeof(silence));
                bsp_audio_play(silence, sizeof(silence));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelete(NULL);
}

extern "C" __attribute__((weak)) void start_tts(void){
    // 1. 初始化TTS
    const esp_partition_t* part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (part == NULL) {
        // 8MB 板无 voice_data 分区，属预期情况
        mp_print_asr("voice_data partition not found (TTS unavailable)\n");
        return;
    }
    void* voicedata;
    esp_partition_mmap_handle_t mmap;
    esp_err_t err=esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, (const void**)&voicedata, &mmap);
    if (err != ESP_OK) {
        mp_print_asr("Couldn't map voice data partition!\n"); 
        return;
    }
    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_template, (int16_t*)voicedata); 
    esp_tts_handle_t *tts_handle = (esp_tts_handle_t *)esp_tts_create(voice);
    xTaskCreatePinnedToCore(&tts_Task, "tts", 4 * 1024, (void*)tts_handle, 6, &tts_task_handle, 0);
}
