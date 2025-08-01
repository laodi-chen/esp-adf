/* volc rtc example code

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "esp_delegate.h"
#include "esp_dispatcher.h"
#include "audio_recorder.h"
#include "recorder_sr.h"
#include "audio_pipeline.h"
#include "audio_thread.h"
#include "raw_stream.h"
#include "audio_mem.h"
#include "esp_vfs_mem.h"

#include "VolcEngineRTCLite.h"
#include "coze_http_request.h"
#include "audio_processor.h"
#include "volc_rtc_message.h"

#define STATS_TASK_PRIO                    (5)
#define JOIN_EVENT_BIT                     (1 << 0)
#define WAIT_DESTORY_READ_TSK_BIT          (1 << 1)
#define WAIT_DESTORY_PROC_TSK_BIT          (1 << 2)
#define WAKEUP_REC_READING                 (1 << 0)
#define DEFAULT_MAX_QUEUE_NUM              (30)
#define DEFAULT_QUEUE_CACHE_NUM            (10)

#define ENABLE_RTCMAIN_TASK_STACK_ON_PSRAM (1)
// #define ENABLE_RTC_LICENSE_VERIFY (1)

static const char* TAG = "VOLC_RTC";

typedef struct {
    char *frame_ptr;
    int   frame_len;
} frame_package_t;

typedef enum {
    RTC_JOIN_IDLE = 0,
    RTC_JOIN,
    RTC_LEAVE,
} rtc_join_state_t;

struct volc_rtc_t {
    recorder_pipeline_handle_t record_pipeline;
    player_pipeline_handle_t   player_pipeline;
    void                      *recorder_engine;
    byte_rtc_engine_t          engine;
    EventGroupHandle_t         join_event;
    EventGroupHandle_t         wait_destory_event;
    EventGroupHandle_t         wakeup_event;
    QueueHandle_t              frame_q;
    coze_http_req_result_t    *user_info;
    esp_dispatcher_handle_t    esp_dispatcher;
    rtc_join_state_t           join_state;
    audio_thread_t             voice_read_task_handle;
    audio_thread_t             audio_data_process_task_handle;
    bool                       byte_rtc_running;
    bool                       data_proc_running;
#if defined(CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
#define DEALULT_OPUS_DATA_CHACHE_SIZE (512)
    char                    *opus_data_cache;
    int                      opus_data_cache_len;
#endif  // CONFIF_AUDIO_SUPPORT_OPUS_DECODER
};

static struct volc_rtc_t s_volc_rtc;

static void audio_pull_data_process(char *ptr, int len);

// byte rtc lite callbacks
static void byte_rtc_on_join_room_success(byte_rtc_engine_t engine, const char* channel, int elapsed_ms, bool ms)
{
    ESP_LOGI(TAG, "join channel success %s elapsed %d ms now %d ms\n", channel, elapsed_ms, elapsed_ms);
    xEventGroupSetBits(s_volc_rtc.join_event, JOIN_EVENT_BIT);
};

static __attribute__((unused)) void byte_rtc_on_rejoin_room_success(byte_rtc_engine_t engine, const char* channel, int elapsed_ms)
{
    ESP_LOGI(TAG, "rejoin channel success %s\n", channel);
};

static void byte_rtc_on_user_joined(byte_rtc_engine_t engine, const char* channel, const char* user_name, int elapsed_ms)
{
    ESP_LOGI(TAG, "remote user joined  %s:%s", channel, user_name);
};

static void byte_rtc_on_user_offline(byte_rtc_engine_t engine, const char* channel, const char* user_name, int reason)
{
    ESP_LOGI(TAG, "remote user offline  %s:%s", channel, user_name);
};

static void byte_rtc_on_user_mute_audio(byte_rtc_engine_t engine, const char* channel, const char* user_name, int muted)
{
    ESP_LOGI(TAG, "remote user mute audio  %s:%s %d", channel, user_name, muted);
};

static void byte_rtc_on_user_mute_video(byte_rtc_engine_t engine, const char* channel, const char* user_name, int muted)
{
    ESP_LOGI(TAG, "remote user mute video  %s:%s %d", channel, user_name, muted);
};

static __attribute__((unused)) void byte_rtc_on_connection_lost(byte_rtc_engine_t engine, const char* channel)
{
    ESP_LOGI(TAG, "connection Lost  %s", channel);
};

static void byte_rtc_on_room_error(byte_rtc_engine_t engine, const char* channel, int code, const char* msg)
{
    ESP_LOGE(TAG, "error occur %s %d %s", channel, code, msg ? msg : "UnKown");
};

// remote audio
static void byte_rtc_on_audio_data(byte_rtc_engine_t engine, const char* room, const char*  uid , uint16_t sent_ts,
                                   audio_data_type_e codec, const void* data_ptr, size_t data_len)
{
    ESP_LOGD(TAG, "remote audio data %s %s %d %d %p %zu", room, uid, sent_ts, codec, data_ptr, data_len);

    pipe_player_state_e state;
    player_pipeline_get_state(s_volc_rtc.player_pipeline, &state);
    if (state == PIPE_STATE_IDLE) {
        return;
    }
    frame_package_t frame = { 0 };
    frame.frame_ptr = audio_calloc(1, data_len);
    memcpy(frame.frame_ptr, data_ptr, data_len);
    frame.frame_len = data_len;
    if (xQueueSend(s_volc_rtc.frame_q, &frame, pdMS_TO_TICKS(10)) != pdPASS) {
        ESP_LOGW(TAG, "audio frame queue full");
    }
}

// remote video
static void byte_rtc_on_video_data(byte_rtc_engine_t engine, const char*  channel, const char* uid, uint16_t sent_ts,
                                    video_data_type_e codec, int is_key_frame,
                                    const void * data_ptr, size_t data_len){ }

// remote message
void on_message_received(byte_rtc_engine_t engine, const char*  room, const char* uid, const uint8_t* message, int size, bool binary)
{
    ESP_LOGD(TAG, "on_message_received uid: %s, message: %s, message size: %d", uid, message, size);
    volc_rtc_message_process(message, size);
}

static void on_key_frame_gen_req(byte_rtc_engine_t engine, const char*  channel, const char*  uid) {}
// byte rtc lite callbacks end.

static void audio_pull_data_process(char *ptr, int len)
{
    char *data_ptr = ptr;
    int data_len = len;
    /* Since OPUS is in VBR mode, it needs to be packaged into a length + data format first then to decoder*/
#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)

#define frame_length_prefix (2)
    if (s_volc_rtc.opus_data_cache_len + frame_length_prefix < len) {
        s_volc_rtc.opus_data_cache = audio_realloc(s_volc_rtc.opus_data_cache, len + frame_length_prefix);
        s_volc_rtc.opus_data_cache_len = len;
    }
    s_volc_rtc.opus_data_cache[0] = (len >> 8) & 0xFF;
    s_volc_rtc.opus_data_cache[1] = len & 0xFF;
    memcpy(s_volc_rtc.opus_data_cache + frame_length_prefix, ptr, len);
    data_ptr = s_volc_rtc.opus_data_cache;
    data_len += frame_length_prefix;
#else
    data_ptr = ptr;
    data_len = len;
#endif // CONFIG_AUDIO_SUPPORT_OPUS_DECODER
    raw_stream_write(player_pipeline_get_raw_write(s_volc_rtc.player_pipeline), data_ptr, data_len);
}

static void audio_tone_player_event_cb(audio_element_status_t evt)
{
    if (evt == AEL_STATUS_STATE_FINISHED) {
        player_pipeline_run(s_volc_rtc.player_pipeline);
    }
}

#if CONFIG_LANGUAGE_WAKEUP_MODE
static esp_err_t dispatcher_audio_play(void *instance, action_arg_t *arg, action_result_t *result)
{
    audio_tone_play((char *)arg->data);
    return ESP_OK;
};
#endif // CONFIG_LANGUAGE_WAKEUP_MODE

static esp_err_t rec_engine_cb(audio_rec_evt_t *event, void *user_data)
{
    if (AUDIO_REC_WAKEUP_START == event->type) {
#if CONFIG_LANGUAGE_WAKEUP_MODE
        ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_WAKEUP_START");
        player_pipeline_stop(s_volc_rtc.player_pipeline);
        action_arg_t action_arg = {0};
        action_arg.data = (void *)"spiffs://spiffs/dingding.wav";

        action_result_t result = {0};
        esp_dispatcher_execute_with_func(s_volc_rtc.esp_dispatcher, dispatcher_audio_play, NULL, &action_arg, &result);
        xEventGroupSetBits(s_volc_rtc.wakeup_event, WAKEUP_REC_READING);
#endif // CONFIG_LANGUAGE_WAKEUP_MODE
    } else if (AUDIO_REC_VAD_START == event->type) {
    } else if (AUDIO_REC_VAD_END == event->type) {
    } else if (AUDIO_REC_WAKEUP_END == event->type) {
    #if CONFIG_LANGUAGE_WAKEUP_MODE
        xEventGroupClearBits(s_volc_rtc.wakeup_event, WAKEUP_REC_READING);
        player_pipeline_stop(s_volc_rtc.player_pipeline);
        ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_WAKEUP_END");
    #endif // CONFIG_LANGUAGE_WAKEUP_MODE
    } else {
    }

    return ESP_OK;
}

static esp_err_t open_audio_pipeline()
{
    s_volc_rtc.record_pipeline = recorder_pipeline_open();
    s_volc_rtc.player_pipeline = player_pipeline_open();
    recorder_pipeline_run(s_volc_rtc.record_pipeline);
    player_pipeline_run(s_volc_rtc.player_pipeline);
    return ESP_OK;
}

static void byte_rtc_engine_destroy() 
{
    if (s_volc_rtc.engine) {
        byte_rtc_fini(s_volc_rtc.engine);
        vTaskDelay(pdMS_TO_TICKS(1000));
        byte_rtc_destroy(s_volc_rtc.engine);
        s_volc_rtc.engine = NULL;
    }
}

static esp_err_t byte_rtc_engine_create()
{
#ifdef CONFIG_COZE_REQUEST
    #ifdef CONFIG_AUDIO_SUPPORT_OPUS_DECODER
        coze_http_req_audio_type_e audio_type = COZE_HTTP_REQ_AUDIO_TYPE_OPUS;
    #else
        coze_http_req_audio_type_e audio_type = COZE_HTTP_REQ_AUDIO_TYPE_G711A;
    #endif
    coze_http_req_config_t http_config = COZE_HTTP_DEFAULT_CONFIG();
    http_config.uri = CONFIG_COZE_URL;
    http_config.authorization = CONFIG_COZE_AUTHORIZATION;
    http_config.bot_id = CONFIG_COZE_BOTID;
    http_config.audio_type = audio_type;
    s_volc_rtc.user_info = coze_http_request(&http_config, COZE_HTTP_REQ_SVC_TYPE_RTC);
    if (s_volc_rtc.user_info == NULL) {
        ESP_LOGE(TAG, "Failed to get user info");
        return ESP_FAIL;
    }
    
#else
    s_volc_rtc.user_info = audio_calloc(1, sizeof(coze_http_req_result_t));
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.user_info, return ESP_FAIL);
    s_volc_rtc.user_info->app_id = CONFIG_APPID;
    s_volc_rtc.user_info->room_id = CONFIG_ROOMID;
    s_volc_rtc.user_info->uid = CONFIG_UID;
    s_volc_rtc.user_info->token = CONFIG_TOKEN;
#endif

    ESP_LOGI(TAG, "app_id: %s, room_id: %s, uid: %s, token: %s", 
            s_volc_rtc.user_info->app_id, s_volc_rtc.user_info->room_id, s_volc_rtc.user_info->uid, s_volc_rtc.user_info->token);
#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
    s_volc_rtc.opus_data_cache_len = DEALULT_OPUS_DATA_CHACHE_SIZE;
    s_volc_rtc.opus_data_cache = audio_calloc(1, s_volc_rtc.opus_data_cache_len);
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.opus_data_cache, goto cleanup);
#endif // CONFIG_AUDIO_SUPPORT_OPUS_DECODER

    byte_rtc_event_handler_t handler = {
        .on_join_room_success       =   byte_rtc_on_join_room_success,
        .on_room_error              =   byte_rtc_on_room_error,
        .on_user_joined             =   byte_rtc_on_user_joined,
        .on_user_offline            =   byte_rtc_on_user_offline,
        .on_user_mute_audio         =   byte_rtc_on_user_mute_audio,
        .on_user_mute_video         =   byte_rtc_on_user_mute_video,
        .on_audio_data              =   byte_rtc_on_audio_data,
        .on_video_data              =   byte_rtc_on_video_data,
        .on_key_frame_gen_req       =   on_key_frame_gen_req,
        .on_message_received        =   on_message_received,
    };
    s_volc_rtc.join_state = RTC_JOIN_IDLE;
    s_volc_rtc.engine = byte_rtc_create(s_volc_rtc.user_info->app_id, &handler);
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.engine, goto cleanup);

    byte_rtc_set_log_level(s_volc_rtc.engine, BYTE_RTC_LOG_LEVEL_ERROR);
#ifdef RTC_TEST_ENV
    byte_rtc_set_params(s_volc_rtc.engine, "{\"env\":2}"); // test env
#endif
    byte_rtc_set_params(s_volc_rtc.engine, "{\"debug\":{\"log_to_console\":1}}"); 
    byte_rtc_set_params(s_volc_rtc.engine,"{\"rtc\":{\"thread\":{\"pinned_to_core\":1}}}");
    byte_rtc_set_params(s_volc_rtc.engine,"{\"rtc\":{\"thread\":{\"priority\":6}}}");

#if defined(ENABLE_RTCMAIN_TASK_STACK_ON_PSRAM)
    byte_rtc_set_params(s_volc_rtc.engine, "{\"rtc\":{\"thread\":{\"stack_in_ext\":1}}}");
#endif  /* ENABLE_RTCMAIN_TASK_STACK_ON_PSRAM */

#if defined(ENABLE_RTC_LICENSE_VERIFY)
    byte_rtc_set_params(s_volc_rtc.engine, "{\"rtc\":{\"license\":{\"enable\":1}}}");
    //     byte_rtc_set_params(s_volc_rtc.engine, "{\"rtc\":{\"license\":{\"enable\":1, \"content\":\"1234567890\"}}}");

#if defined(ENABLE_RTCMAIN_TASK_STACK_ON_PSRAM) 
    esp_err_t ret = esp_vfs_mem_register("/mem", 5);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register memory filesystem: %d", ret);
        return ESP_FAIL;
    }
    byte_rtc_set_params(s_volc_rtc.engine, "{\"rtc\":{\"root_path\":\"/mem\"}}");
    // need to write license content to /mem/VolcEngineRTCLite.lic
#else
    byte_rtc_set_params(s_volc_rtc.engine, "{\"rtc\":{\"root_path\":\"/spiffs\"}}");
#endif  /* ENABLE_RTCMAIN_TASK_STACK_ON_PSRAM */
#endif  /* ENABLE_RTC_LICENSE_VERIFY */

    byte_rtc_init(s_volc_rtc.engine);
#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
    byte_rtc_set_audio_codec(s_volc_rtc.engine, AUDIO_CODEC_TYPE_OPUS);
#elif defined (CONFIG_AUDIO_SUPPORT_G711A_DECODER)
    byte_rtc_set_audio_codec(s_volc_rtc.engine, AUDIO_CODEC_TYPE_G711A);
#else // AACLC Encoder
    byte_rtc_set_audio_codec(s_volc_rtc.engine, AUDIO_CODEC_TYPE_AACLC);
#endif // CONFIG_AUDIO_SUPPORT_OPUS_DECODER
    open_audio_pipeline();

    ESP_LOGI(TAG, "start join room\n");
    byte_rtc_room_options_t options = { 0 };
    options.auto_subscribe_audio    = 1;
    options.auto_subscribe_video    = 0;
    options.auto_publish_audio      = 1;
    options.auto_publish_video      = 0;
    byte_rtc_join_room(s_volc_rtc.engine, s_volc_rtc.user_info->room_id, s_volc_rtc.user_info->uid, s_volc_rtc.user_info->token, &options);
    // Default wait time is forever
    xEventGroupWaitBits(s_volc_rtc.join_event, JOIN_EVENT_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "join room success\n");
    s_volc_rtc.join_state = RTC_JOIN;
    return ESP_OK;

cleanup:
#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
    AUDIO_SAFE_FREE(s_volc_rtc.opus_data_cache, audio_free);
#endif // CONFIG_AUDIO_SUPPORT_OPUS_DECODER
    return ESP_FAIL;
}

static void audio_data_process_task(void *args)
{
    frame_package_t frame = { 0 };
    s_volc_rtc.data_proc_running = true;
    while (s_volc_rtc.data_proc_running) {
        xQueueReceive(s_volc_rtc.frame_q, &frame, portMAX_DELAY);
        if (frame.frame_ptr) {
            audio_pull_data_process(frame.frame_ptr, frame.frame_len);
            audio_free(frame.frame_ptr);
        }
    }
    xEventGroupSetBits(s_volc_rtc.wait_destory_event, WAIT_DESTORY_PROC_TSK_BIT);
    vTaskDelete(NULL);
}

static void voice_read_task(void *args)
{
    const int voice_data_read_sz = recorder_pipeline_get_default_read_size(s_volc_rtc.record_pipeline);
    uint8_t *voice_data = audio_calloc(1, voice_data_read_sz);
    s_volc_rtc.byte_rtc_running = true;

#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
    audio_frame_info_t audio_frame_info = {.data_type = AUDIO_DATA_TYPE_OPUS};
#elif defined (CONFIG_AUDIO_SUPPORT_G711A_DECODER)
    audio_frame_info_t audio_frame_info = {.data_type = AUDIO_DATA_TYPE_PCMA};
#else
    audio_frame_info_t audio_frame_info = {.data_type = AUDIO_DATA_TYPE_AACLC};
#endif // CONFIG_AUDIO_SUPPORT_OPUS_DECODER

#if defined (CONFIG_LANGUAGE_WAKEUP_MODE)
    TickType_t wait_tm = portMAX_DELAY;
#endif // CONFIG_LANGUAGE_WAKEUP_MODE
    while (s_volc_rtc.byte_rtc_running) {
    #if defined (CONFIG_LANGUAGE_WAKEUP_MODE)
        EventBits_t bits = xEventGroupWaitBits(s_volc_rtc.wakeup_event, WAKEUP_REC_READING , false, true, wait_tm);
        if (bits & WAKEUP_REC_READING) {
            int ret = audio_recorder_data_read(s_volc_rtc.recorder_engine, voice_data, voice_data_read_sz, portMAX_DELAY);
            if (ret == 0 || ret == -1) {
                if (ret == 0) {
                    vTaskDelay(15 / portTICK_PERIOD_MS);
                    continue;
                }
                ESP_LOGE(TAG, "audio_recorder_data_read failed, ret: %d\n", ret);
                xEventGroupClearBits(s_volc_rtc.wakeup_event,  WAKEUP_REC_READING);
            } else {
                byte_rtc_send_audio_data(s_volc_rtc.engine, s_volc_rtc.user_info->room_id, voice_data, voice_data_read_sz, &audio_frame_info);
            }
        }
    #else
        int read_len = audio_recorder_data_read(s_volc_rtc.recorder_engine, voice_data, voice_data_read_sz, portMAX_DELAY);
        if (read_len == voice_data_read_sz) {
            byte_rtc_send_audio_data(s_volc_rtc.engine, s_volc_rtc.user_info->room_id, voice_data, voice_data_read_sz, &audio_frame_info);
        }
    #endif
    }
    xEventGroupClearBits(s_volc_rtc.wakeup_event, WAKEUP_REC_READING);
    xEventGroupSetBits(s_volc_rtc.wait_destory_event, WAIT_DESTORY_READ_TSK_BIT);
    audio_free(voice_data);
    vTaskDelete(NULL);
}

static void log_clear(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("AUDIO_THREAD", ESP_LOG_ERROR);
    esp_log_level_set("i2c_bus_v2", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_HAL", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_ERROR);
    esp_log_level_set("I2S_STREAM_IDF5.x", ESP_LOG_ERROR);
    esp_log_level_set("RSP_FILTER", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_EVT", ESP_LOG_ERROR);
}

static esp_err_t volc_rtc_init(void)
{
    esp_err_t ret = ESP_OK;
    /* note: Enable message subtype and function call features */
    // volc_rtc_message_init(VOLC_RTC_MESSAGE_TYPE_SUBTITLE | VOLC_RTC_MESSAGE_TYPE_FUNCTION_CALL);
    ret = byte_rtc_engine_create();
    AUDIO_RET_ON_FALSE(TAG, ret, goto cleanup, "Failed to create byte RTC engine");
    s_volc_rtc.recorder_engine = audio_record_engine_init(s_volc_rtc.record_pipeline, rec_engine_cb);
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.recorder_engine, goto cleanup);
    vTaskDelay(pdMS_TO_TICKS(200));
    ret = audio_thread_create(&s_volc_rtc.voice_read_task_handle, "voice_read_task", voice_read_task, (void *)NULL, 5 * 1024, 5, true, 0);
    AUDIO_RET_ON_FALSE(TAG, ret, goto cleanup, "Failed to create voice read task");
    ret = audio_thread_create(&s_volc_rtc.audio_data_process_task_handle, "audio_data_process_task", audio_data_process_task, (void *)NULL, 5 * 1024, 10, true, 0);
    AUDIO_RET_ON_FALSE(TAG, ret, goto cleanup, "Failed to create audio data process task");
    return ESP_OK;
cleanup:
    if (s_volc_rtc.voice_read_task_handle) {
        audio_thread_cleanup(&s_volc_rtc.voice_read_task_handle);
    }
    if (s_volc_rtc.audio_data_process_task_handle) {
        audio_thread_cleanup(&s_volc_rtc.audio_data_process_task_handle);
    }
    byte_rtc_engine_destroy();
    return ESP_FAIL;
}

esp_err_t volc_rtc_deinit(void)
{
    s_volc_rtc.byte_rtc_running = false;
    s_volc_rtc.data_proc_running = false;
#if defined (CONFIG_LANGUAGE_WAKEUP_MODE)
    xEventGroupSetBits(s_volc_rtc.wakeup_event, WAKEUP_REC_READING);
#endif  /* #if defined (CONFIG_LANGUAGE_WAKEUP_MODE) */
    xEventGroupWaitBits(s_volc_rtc.wait_destory_event, WAIT_DESTORY_READ_TSK_BIT | WAIT_DESTORY_PROC_TSK_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    byte_rtc_engine_destroy();
    AUDIO_SAFE_FREE(s_volc_rtc.join_event, vEventGroupDelete);
    AUDIO_SAFE_FREE(s_volc_rtc.wait_destory_event, vEventGroupDelete);
    audio_record_engine_deinit(s_volc_rtc.record_pipeline);
    player_pipeline_close(s_volc_rtc.player_pipeline);
    recorder_pipeline_close(s_volc_rtc.record_pipeline);
    return ESP_OK;
}

void volc_rtc_app_startup(void)
{
    esp_err_t ret = ESP_OK;
    log_clear();
    ret = audio_tone_init(audio_tone_player_event_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio tone: %d", ret);
        return;
    }

    s_volc_rtc.join_event = xEventGroupCreate();
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.join_event, goto cleanup);
    
    s_volc_rtc.wait_destory_event = xEventGroupCreate();
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.wait_destory_event, goto cleanup);
    
    s_volc_rtc.wakeup_event = xEventGroupCreate();
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.wakeup_event, goto cleanup);
    
    s_volc_rtc.frame_q = xQueueCreate(DEFAULT_MAX_QUEUE_NUM, sizeof(frame_package_t));
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.frame_q, goto cleanup);
    
    s_volc_rtc.esp_dispatcher = esp_dispatcher_get_delegate_handle();
    AUDIO_MEM_CHECK(TAG, s_volc_rtc.esp_dispatcher, goto cleanup);

    ret = volc_rtc_init();
    AUDIO_RET_ON_FALSE(TAG, ret, goto cleanup, "Failed to initialize volc RTC");
    
    return;

cleanup:
    if (s_volc_rtc.frame_q) {
    vQueueDelete(s_volc_rtc.frame_q);
    }
    if (s_volc_rtc.wakeup_event) {
        vEventGroupDelete(s_volc_rtc.wakeup_event);
    }
    if (s_volc_rtc.wait_destory_event) {
        vEventGroupDelete(s_volc_rtc.wait_destory_event);
    }
    if (s_volc_rtc.join_event) {
        vEventGroupDelete(s_volc_rtc.join_event);
    }
    return;
}
