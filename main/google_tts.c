/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mbedtls/base64.h"

#include "esp_http_client.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_hal.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "google_tts.h"
#include "json_utils.h"

static const char *TAG = "GOOGLE_TTS";

#define GOOGLE_TTS_ENDPOINT         "https://tsn.baidu.com/text2audio"
#define GOOGLE_TTS_TEMPLATE         "lan=zh&cuid=ESP32&ctp=1&tok=%s&tex=%s"

typedef struct google_tts {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t  i2s_writer;
    audio_element_handle_t  http_stream_reader;
    audio_element_handle_t  mp3_decoder;
    char                    *api_token;
    char                    *lang_code;
    int                     buffer_size;
    char                    *buffer;
    char                    *text;
    bool                    is_begin;
    int                     tts_total_read;
    int                     sample_rate;
    int                     remain_len;
} google_tts_t;


static esp_err_t _http_stream_reader_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    google_tts_t *tts = (google_tts_t *)msg->user_data;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        // Post text data
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, length=%d", msg->buffer_len);
        tts->tts_total_read = 0;
        tts->is_begin = true;
        int payload_len = snprintf(tts->buffer, tts->buffer_size, GOOGLE_TTS_TEMPLATE, tts->api_token, tts->text);
        ESP_LOGW(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, payload_len: %d, tts->buffer: %s", payload_len, tts->buffer);
        esp_http_client_set_post_field(http, tts->buffer, payload_len);
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        tts->remain_len = 0;

        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_RESPONSE) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_ON_RESPONSE, length=%d", msg->buffer_len);
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST, write end chunked marker");
    }

    return ESP_OK;
}

google_tts_handle_t google_tts_init(google_tts_config_t *config)
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    google_tts_t *tts = calloc(1, sizeof(google_tts_t));
    AUDIO_MEM_CHECK(TAG, tts, return NULL);

    tts->pipeline = audio_pipeline_init(&pipeline_cfg);

    tts->buffer_size = config->buffer_size;
    if (tts->buffer_size <= 0) {
        tts->buffer_size = DEFAULT_TTS_BUFFER_SIZE;
    }

    tts->buffer = malloc(tts->buffer_size);
    AUDIO_MEM_CHECK(TAG, tts->buffer, goto exit_tts_init);

    tts->api_token = strdup(config->api_token);
    AUDIO_MEM_CHECK(TAG, tts->api_token, goto exit_tts_init);

    tts->sample_rate = config->playback_sample_rate;

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    i2s_cfg.std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    i2s_cfg.std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#else
    i2s_cfg.i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
#endif
    tts->i2s_writer = i2s_stream_init(&i2s_cfg);

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.event_handle = _http_stream_reader_event_handle;
    http_cfg.user_data = tts;
    tts->http_stream_reader = http_stream_init(&http_cfg);

    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    tts->mp3_decoder = mp3_decoder_init(&mp3_cfg);

    audio_pipeline_register(tts->pipeline, tts->http_stream_reader, "tts_http");
    audio_pipeline_register(tts->pipeline, tts->mp3_decoder,        "tts_mp3");
    audio_pipeline_register(tts->pipeline, tts->i2s_writer,         "tts_i2s");
    const char *link_tag[3] = {"tts_http", "tts_mp3", "tts_i2s"};
    audio_pipeline_link(tts->pipeline, &link_tag[0], 3);
    i2s_stream_set_clk(tts->i2s_writer, config->playback_sample_rate, 16, 1);
    return tts;
exit_tts_init:
    google_tts_destroy(tts);
    return NULL;
}

esp_err_t google_tts_destroy(google_tts_handle_t tts)
{
    if (tts == NULL) {
        return ESP_FAIL;
    }
    audio_pipeline_stop(tts->pipeline);
    audio_pipeline_wait_for_stop(tts->pipeline);
    audio_pipeline_terminate(tts->pipeline);
    audio_pipeline_remove_listener(tts->pipeline);
    audio_pipeline_deinit(tts->pipeline);
    free(tts->buffer);
    free(tts->api_token);
    free(tts);
    return ESP_OK;
}

esp_err_t google_tts_set_listener(google_tts_handle_t tts, audio_event_iface_handle_t listener)
{
    if (listener) {
        audio_pipeline_set_listener(tts->pipeline, listener);
    }
    return ESP_OK;
}

bool google_tts_check_event_finish(google_tts_handle_t tts, audio_event_iface_msg_t *msg)
{
    if (msg->source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg->source == (void *) tts->i2s_writer
            && msg->cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg->data == AEL_STATUS_STATE_STOPPED) || ((int)msg->data == AEL_STATUS_STATE_FINISHED))) {
        return true;
    }
    return false;
}

esp_err_t google_tts_start(google_tts_handle_t tts, const char *text)
{
    free(tts->text);
    tts->text = strdup(text);
    if (tts->text == NULL) {
        ESP_LOGE(TAG, "Error no mem");
        return ESP_ERR_NO_MEM;
    }
    audio_pipeline_reset_items_state(tts->pipeline);
    audio_pipeline_reset_ringbuffer(tts->pipeline);
    audio_element_set_uri(tts->http_stream_reader, GOOGLE_TTS_ENDPOINT);
    audio_pipeline_run(tts->pipeline);
    return ESP_OK;
}

esp_err_t google_tts_stop(google_tts_handle_t tts)
{
    audio_pipeline_stop(tts->pipeline);
    audio_pipeline_wait_for_stop(tts->pipeline);
    ESP_LOGD(TAG, "TTS Stopped");
    return ESP_OK;
}
