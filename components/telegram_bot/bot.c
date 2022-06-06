#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#    include "esp_crt_bundle.h"
#endif

#include "bot.h"
#include "esp_http_client.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

#define TELEGRAM_BOT_API_KEY CONFIG_TELEGRAM_BOT_API_KEY
#define TELEGRAM_BOT_ADMIN_ID CONFIG_TELEGRAM_BOT_ADMIN_ID

static const char * TAG = "BOT";

extern const char api_telegram_org_root_cert_start[] asm("_binary_api_telegram_org_root_cert_pem_start");
extern const char api_telegram_org_root_cert_end[] asm("_binary_api_telegram_org_root_cert_pem_end");

typedef enum
{
    SEND_MESSAGE,
    GET_UPDATES,
    DELETE_WEBHOOK
} TelegramMethod_t;

typedef struct
{
    TelegramMethod_t method;
    char * post_data;
} Query_t;

static QueueHandle_t queries_q = NULL;

bool make_query(TelegramMethod_t method, char * post_data, bool wait)
{
    Query_t * query = malloc(sizeof(Query_t));
    query->method = method;
    query->post_data = post_data;

    BaseType_t res;

again:
    res = xQueueSend(queries_q, (void *)&query, 0);

    if (wait && res == errQUEUE_FULL)
    {
        vTaskDelay(10);
        goto again;
    }

    if (res != pdPASS)
    {
        free(query);
        return false;
    }

    return true;
}

esp_err_t _http_event_handler(esp_http_client_event_t * evt)
{
    static char * output_buffer; // Buffer to store response of http request from event handler
    static int output_len; // Stores number of bytes read
    switch (evt->event_id)
    {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client))
            {
                // If user_data buffer is configured, copy the response into the buffer
                if (evt->user_data)
                {
                    memcpy((char *)evt->user_data + output_len, evt->data, evt->data_len);
                }
                else
                {
                    if (output_buffer == NULL)
                    {
                        output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client) + 1);
                        output_len = 0;
                        if (output_buffer == NULL)
                        {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    memcpy(output_buffer + output_len, evt->data, evt->data_len);
                }
                output_len += evt->data_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL)
            {
                output_buffer[output_len] = '\0';
                ESP_LOGI(TAG, "%s", output_buffer);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED: {
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");

            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0)
            {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (output_buffer != NULL)
            {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        }
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            esp_http_client_set_header(evt->client, "From", "user@example.com");
            esp_http_client_set_header(evt->client, "Accept", "text/html");
            break;
    }
    return ESP_OK;
}

static void queryMakerTask(void * queue)
{
    esp_http_client_config_t config = {
        .host = "api.telegram.org",
        .path = "/",
		.transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = api_telegram_org_root_cert_start,
        .timeout_ms = 5000,
        .event_handler = _http_event_handler,
        .is_async = true,
		.keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char query_buf[100];

    Query_t * query;

    while (true)
    {
        BaseType_t res;

        query = NULL;
        res = xQueueReceive(queries_q, &query, (TickType_t)10000);
        if (res == errQUEUE_EMPTY)
            continue;

        char * method;
        switch (query->method)
        {
            case SEND_MESSAGE:
                method = "sendMessage";
                break;
            case GET_UPDATES:
                method = "getUpdates";
                break;
            case DELETE_WEBHOOK:
                method = "deleteWebhook";
                break;
            default:
                ESP_LOGI(TAG, "got unknown method for telegram bot");
                goto cleanup;
        }

        strcpy(query_buf, "/bot" TELEGRAM_BOT_API_KEY "/");
        strcpy(query_buf + strlen(query_buf), method);
        ESP_LOGI(TAG, "making query to url = %s, url len = %zu", query_buf, strlen(query_buf));

        esp_err_t err;
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_url(client, query_buf);

        if (query->post_data != NULL)
        {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, query->post_data, strlen(query->post_data));
        }
        else
        {
            esp_http_client_delete_header(client, "Content-Type");
            esp_http_client_set_post_field(client, NULL, 0);
        }

        do
            err = esp_http_client_perform(client);
        while (err == ESP_ERR_HTTP_EAGAIN);

        if (err == ESP_OK)
            ESP_LOGI(
                TAG,
                "HTTPS Status = %d, content_length = %lld",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
        else
            ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));

    cleanup:
        if (query)
        {
            if (query->post_data != NULL)
                free(query->post_data);

            free(query);
        }
    }

    // unreachable, just in case we will make graceful ending
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

void init_query_queue()
{
    queries_q = xQueueCreate(10, sizeof(Query_t *));
    if (queries_q == NULL)
        ESP_LOGE(TAG, "could not create message queue");

    xTaskCreate(&queryMakerTask, "make queries", 8192 * 3, NULL, 5, NULL);
}


void sendMessageToAdmin(char * text)
{
    const char * format = "{\"chat_id\": " TELEGRAM_BOT_ADMIN_ID ", \"text\": \"%s\"}";
    char * msg = malloc(strlen(format) + strlen(text) + 10);
    sprintf(msg, format, text);

    make_query(SEND_MESSAGE, msg, false);
}

static void readUpdatesTask(void * pv)
{
    while (true)
    {
        make_query(GET_UPDATES, NULL, false);

        // sleep for 60s
        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

void initTelegramBot(void)
{
    init_query_queue();
    make_query(DELETE_WEBHOOK, NULL, true);
    xTaskCreate(&readUpdatesTask, "readUpdates", 8192, NULL, 5, NULL);
}
