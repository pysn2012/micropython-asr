// asr_data.h
#ifndef ASR_DATA_H
#define ASR_DATA_H

#include "py/obj.h"
#include <stdbool.h>
#include "driver/gpio.h"


#define  ASR_MODE_SINGLE            1
#define  ASR_MODE_CONTINUOUS        2

// 结构体定义
typedef struct {
    uint8_t reg;
    uint8_t val;
} reg_cfg_t;

typedef struct _asr_data_obj_t {
    mp_obj_base_t base;
    
    // 音频相关数据
    int audio_id;           // 语音识别ID
    
} asr_data_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

// 函数声明
void mp_print_asr_cstr(const char *str);
void mp_print_asr(const char *fmt, ...);
extern int audio_capture_flag;

// ES7243E配置表声明
extern reg_cfg_t es7243e_stop_table[];
extern reg_cfg_t es7243e_config_table[];
extern reg_cfg_t es7243e_start_table[];

#ifdef __cplusplus
}
#endif

#endif // ASR_DATA_H
