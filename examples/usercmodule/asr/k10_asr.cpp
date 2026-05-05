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

void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t*)arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int nch = afe_handle->get_feed_channel_num(afe_data);
    int feed_channel = 2;
    size_t samp_len_bytes = audio_chunksize * sizeof(int16_t) * feed_channel;
    int samp_len = samp_len_bytes / (sizeof(int32_t));
    int16_t *i2s_buff = (int16_t *)malloc(samp_len_bytes);
    assert(i2s_buff);
    size_t r_bytes = 0;
    
    while (1) {
        if (free_task == 1) {
            break;
        }
        
        i2s_channel_read(rx_handle, i2s_buff, samp_len_bytes, &r_bytes, portMAX_DELAY);
          
        int32_t *tmp_buff = (int32_t*)i2s_buff;
        for (int i = 0; i < samp_len; i++) {
            tmp_buff[i] = tmp_buff[i] >> 14; // 32:8为有效位， 8:0为低8位， 全为0， AFE的输入为16位语音数据，拿29：13位是为了对语音信号放大。
        }
        afe_handle->feed(afe_data, (int16_t*)tmp_buff);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (i2s_buff) {
        free(i2s_buff);
        i2s_buff = NULL;
    }
    vTaskDelete(NULL);
}

void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t*)arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, wake_time);
    esp_mn_commands_alloc(multinet,model_data);
    
    while (1) {
        if (free_task == 1) {
            break;
        }
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); 
        if (!res || res->ret_value == ESP_FAIL) {
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
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
    if (afe_data) {
        afe_handle->destroy(afe_data);
        afe_data = NULL;
    }
    cb_free_task = 1;
    vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelete(NULL);
}

extern "C" __attribute__((weak)) void init_asr(int time, int flag, int blck, int lrck, int dsin, int dout, int mlck) {
    wake_time = time;
    wake_flag = flag;
    // I2S 配置
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
    
    // I2S 标准模式配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000), // 16kHz 采样率
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
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
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
	if (mlck != -1) std_cfg.gpio_cfg.mclk = (gpio_num_t)mlck;
    if (blck != -1) std_cfg.gpio_cfg.bclk = (gpio_num_t)blck;
    if (lrck != -1) std_cfg.gpio_cfg.ws   = (gpio_num_t)lrck;
    if (dout != -1) std_cfg.gpio_cfg.dout = (gpio_num_t)dout;
    if (dsin != -1) std_cfg.gpio_cfg.din  = (gpio_num_t)dsin;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
}

// 音频采集任务
extern "C" __attribute__((weak)) void create_asr(void) {

    models = esp_srmodel_init("model");
    afe_config_t *afe_config = afe_config_init("MN", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void*)afe_data, 5, NULL, 1);
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void*)afe_data, 5, NULL, 0);

}

extern "C" __attribute__((weak)) void add_asr_command(int id, const char *command){
    esp_mn_commands_add(id,command);
    esp_mn_commands_update();
}

extern "C" __attribute__((weak)) void free_asr(void){
    // 2. 释放I2S资源
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

esp_err_t bsp_audio_play(const int16_t* data, int length)
{
    size_t bytes_write = 0;
    esp_err_t ret = ESP_OK;
    size_t bytes_written = 0;
    int out_length= length;

    int *data_out = NULL;

    out_length = length * 2;
    data_out = (int*)malloc(out_length);
    for (int i = 0; i < length / sizeof(int16_t); i++) {
        int ret = data[i];
        data_out[i] = ret << 16;
    }

    ret = i2s_channel_write(tx_handle, data_out, out_length, &bytes_written, portMAX_DELAY);
    free(data_out);
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
