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
#include "google_sr.h"
#include "json_utils.h"

#include "board.h"
#include "baidu_access_token.h"

static const char *TAG = "GOOGLE_SR";

#define BAIDU_SR_ENDPOINT "https://vop.baidu.com/pro_api"

#define BAIDU_SR_BEGIN "{"                        \
                        "\"format\": \"pcm\","     \
                        "\"rate\": 16000,"         \
                        "\"channel\": 1,"          \
                        "\"cuid\": \"esp32-toy\"," \
                        "\"token\": \"%s\","       \
                        "\"dev_pid\": 80001,"      \
                        "\"speech\":"              \
                        "\""
/*  base64 audio bytes  */
#define BAIDU_SR_END "\","        \
                      "\"len\":%d" \
                      "}"

#define BAIDU_SR_TASK_STACK (8 * 1024)

#define TEST_WITH_PYTHON (1)

typedef struct google_sr
{
    audio_pipeline_handle_t pipeline;
    int remain_len;
    int sr_total_write;
    int sr_audio_total_bytes;
    bool is_begin;
    char *buffer;
    char *b64_buffer;
    audio_element_handle_t i2s_reader;
    audio_element_handle_t http_stream_writer;
    char *api_token;
    int sample_rates;
    int buffer_size;
    char *response_text;
    google_sr_event_handle_t on_begin;
} google_sr_t;

static int _http_write_chunk(esp_http_client_handle_t http, const char *buffer, int len)
{
    char header_chunk_buffer[16];
    int header_chunk_len = sprintf(header_chunk_buffer, "%x\r\n", len);
    if (esp_http_client_write(http, header_chunk_buffer, header_chunk_len) <= 0)
    {
        return ESP_FAIL;
    }
    int write_len = esp_http_client_write(http, buffer, len);
    if (write_len <= 0)
    {
        ESP_LOGE(TAG, "Error write chunked content");
        return ESP_FAIL;
    }
    if (esp_http_client_write(http, "\r\n", 2) <= 0)
    {
        return ESP_FAIL;
    }
    return write_len;
}

//////////////////////////////////////////////////
/// 测试程序，发送音频数据到Pyhon server运行的目录
//////////////////////////////////////////////////
esp_err_t _test_http_stream_writer_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    char len_buf[16];
    static int total_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d", msg->buffer_len);
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        char dat[10] = {0};
        snprintf(dat, sizeof(dat), "%d", 16000);
        esp_http_client_set_header(http, "x-audio-sample-rates", dat);
        memset(dat, 0, sizeof(dat));
        snprintf(dat, sizeof(dat), "%d", 16);
        esp_http_client_set_header(http, "x-audio-bits", dat);
        memset(dat, 0, sizeof(dat));
        snprintf(dat, sizeof(dat), "%d", 1);
        esp_http_client_set_header(http, "x-audio-channel", dat);
        total_write = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST) {
        // write data
        int wlen = sprintf(len_buf, "%x\r\n", msg->buffer_len);
        if (esp_http_client_write(http, len_buf, wlen) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, msg->buffer, msg->buffer_len) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, "\r\n", 2) <= 0) {
            return ESP_FAIL;
        }
        total_write += msg->buffer_len;
        printf("\033[A\33[2K\rTotal bytes written: %d\n", total_write);
        return msg->buffer_len;
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST");
        char *buf = calloc(1, 64);
        assert(buf);
        int read_len = esp_http_client_read(http, buf, 64);
        if (read_len <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        buf[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char *)buf);
        free(buf);
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t _http_stream_writer_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    google_sr_t *sr = (google_sr_t *)msg->user_data;

    int write_len;
    size_t need_write = 0;

    //* HTTP_STREAM_PRE_REQUEST
    if (msg->event_id == HTTP_STREAM_PRE_REQUEST)
    {
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d", msg->buffer_len);
        sr->sr_total_write = 0;
        sr->sr_audio_total_bytes = 0;
        sr->is_begin = true;
        sr->remain_len = 0;
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        //* set headers
        esp_http_client_set_post_field(http, NULL, -1);
        esp_http_client_set_header(http, "Content-Type", "application/json");
        esp_http_client_set_header(http, "Connection", "keep-alive");
        esp_http_client_delete_header(http, "Content-Length");
        return ESP_OK;
    }

    //* HTTP_STREAM_ON_REQUEST
    if (msg->event_id == HTTP_STREAM_ON_REQUEST)
    {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_ON_REQUEST, lenght=%d, begin=%d", msg->buffer_len, sr->is_begin);
        //*  Write first chunk: BAIDU_SR_BEGIN
        if (sr->is_begin)
        {
            sr->is_begin = false;
            int sr_begin_len = snprintf(sr->buffer, sr->buffer_size, BAIDU_SR_BEGIN, sr->api_token);
            if (sr->on_begin)
            {
                sr->on_begin(sr);
            }
            ESP_LOGI(TAG, "BAIDU_SR_BEGIN: " BAIDU_SR_BEGIN, sr->api_token);
            return _http_write_chunk(http, sr->buffer, sr_begin_len);
        }
        //* base64-encoded need at most 1.5 times buffer than the raw audio buffer
        if (msg->buffer_len > sr->buffer_size * 3 / 2)
        {
            ESP_LOGE(TAG, "Please use SR Buffer size greeter than %d", msg->buffer_len * 3 / 2);
            return ESP_FAIL;
        }

        //* Write b64 audio data
        memcpy(sr->buffer + sr->remain_len, msg->buffer, msg->buffer_len);
        sr->remain_len += msg->buffer_len;

        //* base64 need to keep the datalen to encode as the multiple of 3
        int keep_next_time = sr->remain_len % 3;
        sr->remain_len -= keep_next_time;
        if (mbedtls_base64_encode((unsigned char *)sr->b64_buffer, sr->buffer_size, &need_write, (unsigned char *)sr->buffer, sr->remain_len) != 0)
        {
            ESP_LOGE(TAG, "Error encode b64");
            return ESP_FAIL;
        }
        //* calculate total raw audio len
        sr->sr_audio_total_bytes += sr->remain_len;
        //* keep the `keep_next_time bytes`
        if (keep_next_time > 0)
        {
            memcpy(sr->buffer, sr->buffer + sr->remain_len, keep_next_time);
        }
        sr->remain_len = keep_next_time;
        ESP_LOGD(TAG, "\033[A\33[2K\rTotal bytes written: %d", sr->sr_total_write);

        //* write base64 data
        write_len = _http_write_chunk(http, (const char *)sr->b64_buffer, need_write);
        if (write_len <= 0)
        {
            ESP_LOGE(TAG, "_http_write_chunk error, write_len=%d", write_len);
            return write_len;
        }
        sr->sr_total_write += write_len;
        return write_len;
    }

    //* HTTP_STREAM_POST_REQUEST
    if (msg->event_id == HTTP_STREAM_POST_REQUEST)
    {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
        need_write = 0;
        //* write remained bytes
        if (sr->remain_len)
        {
            if (mbedtls_base64_encode((unsigned char *)sr->b64_buffer, sr->buffer_size, &need_write, (unsigned char *)sr->buffer, sr->remain_len) != 0)
            {
                ESP_LOGE(TAG, "Error encode b64");
                return ESP_FAIL;
            }
            sr->sr_audio_total_bytes += sr->remain_len;
            write_len = _http_write_chunk(http, (const char *)sr->b64_buffer, need_write);
            if (write_len <= 0)
            {
                return write_len;
            }
        }
        //* Write End chunk: BAIDU_SR_END
        int sr_end_len = snprintf(sr->buffer, sr->buffer_size, BAIDU_SR_END, sr->sr_audio_total_bytes);
        write_len = _http_write_chunk(http, sr->buffer, sr_end_len);
        ESP_LOGI(TAG, "BAIDU_SE_END: " BAIDU_SR_END, sr->sr_audio_total_bytes);
        if (write_len <= 0)
        {
            return ESP_FAIL;
        }
        //* Finish chunked
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0)
        {
            return ESP_FAIL;
        }
        return write_len;
    }
    //* HTTP_STREAM_FINISH_REQUEST
    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST)
    {
        int read_len = esp_http_client_read(http, (char *)sr->buffer, sr->buffer_size);
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST, read_len=%d", read_len);
        if (read_len <= 0)
        {
            return ESP_FAIL;
        }
        if (read_len > sr->buffer_size - 1)
        {
            read_len = sr->buffer_size - 1;
        }
        sr->buffer[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char *)sr->buffer);
        if (sr->response_text)
        {
            free(sr->response_text);
        }
        //* parse json and get `result`
        sr->response_text = json_get_token_value(sr->buffer, "result");
        return ESP_OK;
    }
    return ESP_OK;
}

google_sr_handle_t google_sr_init(google_sr_config_t *config)
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    google_sr_t *sr = calloc(1, sizeof(google_sr_t));
    AUDIO_MEM_CHECK(TAG, sr, return NULL);
    sr->pipeline = audio_pipeline_init(&pipeline_cfg);

    sr->buffer_size = config->buffer_size;
    if (sr->buffer_size <= 0)
    {
        sr->buffer_size = DEFAULT_SR_BUFFER_SIZE;
    }

    sr->buffer = malloc(sr->buffer_size);
    AUDIO_MEM_CHECK(TAG, sr->buffer, goto exit_sr_init);
    sr->b64_buffer = malloc(sr->buffer_size);
    AUDIO_MEM_CHECK(TAG, sr->b64_buffer, goto exit_sr_init);
    sr->api_token = strdup(config->api_token);
    AUDIO_MEM_CHECK(TAG, sr->api_token, goto exit_sr_init);

    //* config I2S
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT, 44100, 16, AUDIO_STREAM_READER);
    i2s_cfg.type = AUDIO_STREAM_READER;
    // Increase buffer to avoid missing data in bad network conditions
    i2s_cfg.out_rb_size = 16 * 1024;
    i2s_cfg.i2s_config.sample_rate = 16000;
    // i2s_cfg.i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    // //* set `use_apll` to `false` to avoid noise due to unstable clk
    // i2s_cfg.i2s_config.use_apll = false;
    sr->i2s_reader = i2s_stream_init(&i2s_cfg);

    // i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    // i2s_cfg.type = AUDIO_STREAM_READER;
    // // Increase buffer to avoid missing data in bad network conditions
    // i2s_cfg.out_rb_size = 16 * 1024;
    // sr->i2s_reader = i2s_stream_init(&i2s_cfg);

    //* config HTTPSTREAM
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_WRITER;
    if (TEST_WITH_PYTHON) {
        http_cfg.event_handle = _test_http_stream_writer_event_handle;
    } else {
        http_cfg.event_handle = _http_stream_writer_event_handle;
    }
    http_cfg.user_data = sr;
    http_cfg.task_stack = BAIDU_SR_TASK_STACK;

    sr->http_stream_writer = http_stream_init(&http_cfg);
    sr->sample_rates = config->record_sample_rates;
    sr->on_begin = config->on_begin;

    audio_pipeline_register(sr->pipeline, sr->http_stream_writer, "sr_http");
    audio_pipeline_register(sr->pipeline, sr->i2s_reader, "sr_i2s");
    const char *link_tag[2] = {"sr_i2s", "sr_http"};
    audio_pipeline_link(sr->pipeline, &link_tag[0], 2);
    i2s_stream_set_clk(sr->i2s_reader, config->record_sample_rates, 16, 1);

    return sr;
exit_sr_init:
    google_sr_destroy(sr);
    return NULL;
}

esp_err_t google_sr_destroy(google_sr_handle_t sr)
{
    if (sr == NULL) {
        return ESP_FAIL;
    }
    audio_pipeline_stop(sr->pipeline);
    audio_pipeline_wait_for_stop(sr->pipeline);
    audio_pipeline_terminate(sr->pipeline);
    audio_pipeline_remove_listener(sr->pipeline);
    audio_pipeline_deinit(sr->pipeline);
    free(sr->buffer);
    free(sr->b64_buffer);
    free(sr->api_token);
    free(sr);
    return ESP_OK;
}

esp_err_t google_sr_set_listener(google_sr_handle_t sr, audio_event_iface_handle_t listener)
{
    if (listener)
    {
        audio_pipeline_set_listener(sr->pipeline, listener);
    }
    return ESP_OK;
}

esp_err_t google_sr_start(google_sr_handle_t sr)
{
    audio_pipeline_reset_items_state(sr->pipeline);
    audio_pipeline_reset_ringbuffer(sr->pipeline);
    if (TEST_WITH_PYTHON) {
        audio_element_set_uri(sr->http_stream_writer, CONFIG_TEST_SERVER_URI);
    } else {
        audio_element_set_uri(sr->http_stream_writer, BAIDU_SR_ENDPOINT);
    }
    audio_pipeline_run(sr->pipeline);
    return ESP_OK;
}

char *google_sr_stop(google_sr_handle_t sr)
{
    audio_pipeline_stop(sr->pipeline);
    audio_pipeline_wait_for_stop(sr->pipeline);
    AUDIO_NULL_CHECK(TAG, sr->response_text, {
        return NULL;
    })
    //* get pure answer text
    char *pure_text = strndup(sr->response_text + 2, strlen(sr->response_text) - 4);
    free(sr->response_text);
    sr->response_text = NULL;
    return pure_text;
}