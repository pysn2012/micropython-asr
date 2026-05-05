#include <string.h>
#include <stdarg.h>
#include "py/nlr.h"
#include "py/obj.h"
#include "py/objtype.h"
#include "py/runtime.h"
#include "py/binary.h"
#include "py/mpstate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "asr_data.h"

int audio_capture_flag = 0;
int free_task = 0;
int cb_free_task = 0;

void mp_print_asr_cstr(const char *str) {
    mp_printf(&mp_plat_print, "%s", str);
}

void mp_print_asr(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mp_vprintf(&mp_plat_print, fmt, args);  // 用 MicroPython 的 vprintf
    va_end(args);
}

extern void create_asr(void);
extern void init_asr(int time, int flag, int blck, int lrck, int dsin, int dout, int mlck);
extern void add_asr_command(int id, const char *command);
extern void free_asr(void);
extern void start_tts(void);

static mp_obj_t g_asr_callback = mp_const_none;//音频识别结果回调
static QueueHandle_t asr_result_queue = NULL;//音频识别结果队列
QueueHandle_t tts_data_queue = NULL;//TTS数据队列

// 设置回调
static mp_obj_t mp_set_asr_callback(mp_obj_t callback) {
    if (callback == mp_const_none || mp_obj_is_callable(callback)) {
        g_asr_callback = callback;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("callback must be callable or None"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_set_asr_callback_obj, mp_set_asr_callback);

// 提供给 C++ 调用，用于把数据塞进队列
void audio_push_result(asr_data_obj_t *data) {
    if (asr_result_queue) {
        asr_data_obj_t copy = *data;
        xQueueSend(asr_result_queue, &copy, 0); // 立即返回，满了就丢弃最新数据
    }
}

// 专门的队列消费任务
static void asr_callback_task(void* arg) {
    asr_data_obj_t data;
    while (1) {
        if (cb_free_task == 1) {
            break;
        }
        if (xQueueReceive(asr_result_queue, &data, portMAX_DELAY)) {
            if (g_asr_callback != mp_const_none) {
                // 把识别ID推送到回调函数
                mp_sched_schedule(g_asr_callback, mp_obj_new_int(data.audio_id));
            }
        }
    }
    vTaskDelete(NULL);
}

// 启动音频采集任务
static mp_obj_t mp_start_asr(void) {
    if (!asr_result_queue) {
        asr_result_queue = xQueueCreate(10, sizeof(asr_data_obj_t)); // 最多缓存10个结果
    }
    create_asr();
    xTaskCreatePinnedToCore(asr_callback_task, "asr_cb_task", 4096, NULL, 5, NULL, 1);//这个保留
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_start_asr_obj, mp_start_asr);

//初始化麦克风，I2S
static mp_obj_t mp_init_asr(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // 定义参数列表
    enum { ARG_wake_time, ARG_wake_flag , ARG_blck , ARG_lrck ,ARG_dsin ,ARG_dout ,ARG_mlck};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_wake_time, MP_ARG_INT, {.u_int = 6000} },   // 默认 6000
        { MP_QSTR_wake_flag, MP_ARG_INT, {.u_int = 1} },      // 默认 1
		{ MP_QSTR_blck, MP_ARG_INT, {.u_int = -1} },   // 默认 6000
        { MP_QSTR_lrck, MP_ARG_INT, {.u_int = -1} },      // 默认 1
		{ MP_QSTR_dsin, MP_ARG_INT, {.u_int = -1} },   // 默认 6000
        { MP_QSTR_dout, MP_ARG_INT, {.u_int = -1} },      // 默认 1
		{ MP_QSTR_mlck, MP_ARG_INT, {.u_int = -1} },   // 默认 6000
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    free_task = 0;
    cb_free_task = 0;
    init_asr(args[ARG_wake_time].u_int, args[ARG_wake_flag].u_int, args[ARG_blck].u_int, args[ARG_lrck].u_int, args[ARG_dsin].u_int, args[ARG_dout].u_int, args[ARG_mlck].u_int);

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mp_init_asr_obj, 0, mp_init_asr);

static mp_obj_t mp_add_asr_command(mp_obj_t id, mp_obj_t command) {
    add_asr_command(mp_obj_get_int(id), mp_obj_str_get_str(command));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_add_asr_command_obj, mp_add_asr_command);

static mp_obj_t mp_free_asr(void) {
    free_task = 1;
    free_asr();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_free_asr_obj, mp_free_asr);

static mp_obj_t mp_start_tts(void) {
    if (!tts_data_queue) {
        tts_data_queue = xQueueCreate(10, sizeof(const char *));
    }
    start_tts();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_start_tts_obj, mp_start_tts);

static mp_obj_t mp_add_tts_data(mp_obj_t data) {
    size_t len;
    const char *buf = mp_obj_str_get_data(data, &len);
    char *copy = malloc(len + 1);
    memcpy(copy, buf, len);
    copy[len] = '\0';
    xQueueSend(tts_data_queue, &copy, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_add_tts_data_obj, mp_add_tts_data);

static const mp_rom_map_elem_t k10_asr_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_asr) },
    { MP_ROM_QSTR(MP_QSTR_init_asr), MP_ROM_PTR(&mp_init_asr_obj) },//初始化麦克风，I2S
    { MP_ROM_QSTR(MP_QSTR_set_asr_callback), MP_ROM_PTR(&mp_set_asr_callback_obj) },//设置音频采集回调
    { MP_ROM_QSTR(MP_QSTR_start_asr), MP_ROM_PTR(&mp_start_asr_obj) },//启动音频采集任务
    { MP_ROM_QSTR(MP_QSTR_add_asr_command), MP_ROM_PTR(&mp_add_asr_command_obj) },//添加音频采集命令
    { MP_ROM_QSTR(MP_QSTR_free_asr), MP_ROM_PTR(&mp_free_asr_obj) },//释放音频采集资源
    { MP_ROM_QSTR(MP_QSTR_ASR_MODE_SINGLE),         MP_ROM_INT(ASR_MODE_SINGLE) },
    { MP_ROM_QSTR(MP_QSTR_ASR_MODE_CONTINUOUS),     MP_ROM_INT(ASR_MODE_CONTINUOUS) },
    { MP_ROM_QSTR(MP_QSTR_start_tts), MP_ROM_PTR(&mp_start_tts_obj) },//TTS数据队列
    { MP_ROM_QSTR(MP_QSTR_add_tts_data), MP_ROM_PTR(&mp_add_tts_data_obj) },//TTS数据队列
};

static MP_DEFINE_CONST_DICT(k10_asr_globals, k10_asr_globals_table);

const mp_obj_module_t mp_k10_asr_system = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&k10_asr_globals,
};

MP_REGISTER_MODULE(MP_QSTR_asr, mp_k10_asr_system);