#include <string.h>
#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "midi.h"
#include "nvs.h"
#include "nvs_flash.h"

/* The examples use WiFi configuration that you can set via 'make menuconfig'.
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define WIFI_SSID "bigfoot"
#define WIFI_PASS "12341234"

static bool shut_http_down = true;
static httpd_handle_t server = NULL;

static const char * TAG = "WiFi AP";

typedef struct
{
    char * type;
    const char * start;
    const char * end;
} file_cxt;

extern const char index_start[] asm("_binary_index_html_start");
extern const char index_end[] asm("_binary_index_html_end");
extern const char comp_start[] asm("_binary_components_js_start");
extern const char comp_end[] asm("_binary_components_js_end");
extern const char pure_start[] asm("_binary_pure_css_start");
extern const char pure_end[] asm("_binary_pure_css_end");

static const char *CAPTIVE_HTML = "Open <a href=\"http://192.168.4.1\">configuration page";

char * button_names[] = {"top_left", "top_center", "top_right", "bottom_left", "bottom_center", "bottom_right"};

static char * initial_format = "var initial_values = `";
char * default_values[] = {"action=cc&channel=0&d1=89&d2=127",
                           "action=cc&channel=0&d1=109&d2=127",
                           "action=cc&channel=0&d1=108&d2=127",
                           "action=cc&channel=0&d1=68&d2=127",
                           "action=cc&channel=0&d1=86&d2=127",
                           "action=cc&channel=0&d1=85&d2=127"};


static esp_err_t file_handler(httpd_req_t * req)
{
    file_cxt * ctx = req->user_ctx;
    httpd_resp_set_type(req, ctx->type);
    httpd_resp_send(req, ctx->start, ctx->end - ctx->start - 1);
    return ESP_OK;
}

static esp_err_t initial_handler(httpd_req_t * req)
{
    static size_t init_len = 0;
    esp_err_t err;
    size_t len = 0;
    size_t lengths[N_BUTTONS];
    size_t key_lengths[N_BUTTONS];

    if (init_len == 0)
        init_len = strlen(initial_format);

    nvs_handle storage_handle;
    err = nvs_open(STORAGE, NVS_READWRITE, &storage_handle);
    ESP_ERROR_CHECK(err);

    /* first calculate whole length */
    for (size_t i = 0; i < N_BUTTONS; i++)
    {
        size_t vallen;
        char * key = button_names[i];
        key_lengths[i] = strlen(key);

        lengths[i] = 0; /* indicates that it's default */
        err = nvs_get_str(storage_handle, key, NULL, &vallen);
        switch (err)
        {
            case ESP_OK:
                lengths[i] = vallen - 1; /* minus zero terminator */
                break;
            default:
                if (err != ESP_ERR_NVS_NOT_FOUND)
                    ESP_LOGE(TAG, "Error (%s) reading!: %s\n", key, esp_err_to_name(err));

                len += strlen(default_values[i]);
        }
        len += key_lengths[i] + 1; /* plus ':' */
        len += lengths[i];
    }

    /* we allocate more, just for any case and \n delimiters */
    char * res = malloc(init_len + len + 100);
    if (res == 0)
    {
        ESP_LOGE(TAG, "could not allocate memory");
        return ESP_FAIL;
    }
    char * pos = res;

    /* fill the output */
    memcpy(pos, initial_format, init_len);
    pos += init_len;

    for (size_t i = 0; i < N_BUTTONS; i++)
    {
        char * key = button_names[i];
        memcpy(pos, key, key_lengths[i]);
        pos += key_lengths[i];
        *pos++ = ':';

        if (lengths[i] == 0)
        {
            size_t vallen = strlen(default_values[i]);
            memcpy(pos, default_values[i], vallen);
            pos += vallen;
        }
        else
        {
            size_t len = lengths[i] + 1;
            ESP_ERROR_CHECK(nvs_get_str(storage_handle, key, pos, &len))
            pos += lengths[i];
        }

        *pos = '\n';
        pos++;
    }

    *pos++ = '`';
    *pos++ = ';';
    *pos = '\0';
    ESP_LOGI(TAG, "%s", res);

    nvs_close(storage_handle);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, res, pos - res);
    free(res);

    shut_http_down = false;
    return ESP_OK;
}

static esp_err_t captive_handler(httpd_req_t * req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1");
    httpd_resp_send(req, CAPTIVE_HTML, strlen(CAPTIVE_HTML));
    return ESP_OK;
}

static esp_err_t configure_handler(httpd_req_t * req)
{
    char * error = NULL;
    char * buf = NULL;
    httpd_resp_set_hdr(req, "Location", "/");
    QueueHandle_t midi_queue = req->user_ctx;

    if (req->content_len > 10000)
    {
        error = "too big packet";
        goto done;
    }

    setup_unused_timer();
    buf = malloc(req->content_len + 1);
    int ret = httpd_req_recv(req, buf, req->content_len);

    if (ret <= 0 || ret != req->content_len)
    {
        error = "packet reading error";
        goto done;
    }
    buf[req->content_len] = '\0';

    char * pos;
    if ((pos = strstr(buf, "play")) != NULL)
    {
        size_t len;
        pos += 5;
        midi_event_t * events = parse_action(pos, &len, NULL);
        if (events == NULL)
        {
            error = "action parsing error";
            goto done;
        }

        for (size_t i = 0; i < len; i++)
        {
            BaseType_t ret = xQueueSend(midi_queue, &events[i], (TickType_t)0);
            if (ret == pdFALSE)
                error = "queue of events is full";
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        free(events);
    }
    else
    {
        char * next_pos;
        pos = buf;

        while ((next_pos = strstr(pos, "\n")) != NULL)
        {
            char *btn, *copy;
            size_t len;

            *next_pos = '\0';

            if (strlen(pos) >= 4000) // max storage size
            {
                error = "too long message";
                goto done;
            }

            /* parse action will modify the value, so keep copy */
            copy = strstr(pos, ":");
            if (copy == NULL)
            {
                error = "parsing error #0";
                goto done;
            }
            copy = strdup(copy + 1);

            midi_event_t * events = parse_action(pos, &len, &btn);
            if (events == NULL)
            {
                free(copy);
                error = "parsing error %#1";
                goto done;
            }

            bool btn_valid = false;
            for (size_t i = 0; btn && i < N_BUTTONS; i++)
            {
                if (strcmp(button_names[i], btn) == 0)
                {
                    btn_valid = true;
                    break;
                }
            }

            if (btn_valid && len > 0)
            {
                nvs_handle storage_handle;
                ESP_ERROR_CHECK(nvs_open(STORAGE, NVS_READWRITE, &storage_handle));
                ESP_LOGI(TAG, "writing value %s with key %s", copy, btn);
                ESP_ERROR_CHECK(nvs_set_str(storage_handle, btn, copy));
                nvs_close(storage_handle);
            }

            free(copy);
            free(events);
            pos = next_pos + 1;
        }

        /* use just saved values */
        init_button_events(midi_queue);
    }

done:
    if (error)
    {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, error);
    }
    else
    {
        httpd_resp_set_status(req, HTTPD_200);
        httpd_resp_sendstr(req, "OK");
    }

    if (buf)
        free(buf);

    return ESP_OK;
}

file_cxt index_cxt = {"text/html", index_start, index_end};
file_cxt comp_cxt = {"text/js", comp_start, comp_end};
file_cxt pure_cxt = {"text/css", pure_start, pure_end};

static void register_file_url(httpd_handle_t server, char * uri, file_cxt * cxt)
{
    httpd_uri_t data = {
        .uri = uri,
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = cxt,
    };
    httpd_register_uri_handler(server, &data);
}

static void unused_callback(TimerHandle_t arg)
{
    (void)arg;

    if (shut_http_down)
    {
        ESP_LOGI("http", "Unused timeout was triggered");
        stop_wifi();
    }
}

void start_http_server(QueueHandle_t queue)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    register_file_url(server, "/", &index_cxt);
    register_file_url(server, "/components.js", &comp_cxt);
    register_file_url(server, "/pure.css", &pure_cxt);

    httpd_uri_t initial = {
        .uri = "/initial.js",
        .method = HTTP_GET,
        .handler = initial_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &initial);

    httpd_uri_t configure = {
        .uri = "/configure",
        .method = HTTP_POST,
        .handler = configure_handler,
        .user_ctx = queue,
    };
    httpd_register_uri_handler(server, &configure);

    httpd_uri_t captive = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = captive_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &captive);

    // if web server will not be used in 10 minutes, shut it down and
    // deinit wifi
    TimerHandle_t timer_check_using = xTimerCreate("unused track", pdMS_TO_TICKS(60000 * 5), pdFALSE, NULL, unused_callback);

    if (!timer_check_using)
        ESP_LOGE("http", "could not init unused checking timer");

    shut_http_down = true;
    if (xTimerStart(timer_check_using, 0) != pdPASS)
        ESP_LOGE("http", "could not start unused checking timer");
}

static esp_err_t event_handler(void * ctx, system_event_t * event)
{
    switch (event->event_id)
    {
        case SYSTEM_EVENT_AP_STACONNECTED:
            ESP_LOGI(
                TAG, "station:" MACSTR " join, AID=%d", MAC2STR(event->event_info.sta_connected.mac), event->event_info.sta_connected.aid);
            break;
        case SYSTEM_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(
                TAG,
                "station:" MACSTR "leave, AID=%d",
                MAC2STR(event->event_info.sta_disconnected.mac),
                event->event_info.sta_disconnected.aid);
            break;
        default:
            break;
    }
    return ESP_OK;
}

void initialise_wifi()
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .ap = {.ssid = WIFI_SSID,
               .ssid_len = strlen(WIFI_SSID),
               .password = WIFI_PASS,
               .max_connection = 4,
               .authmode = WIFI_AUTH_WPA_WPA2_PSK},
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished.SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
}

void stop_wifi()
{
    if (server)
    {
        /* Stop the httpd server */
        ESP_LOGI(TAG, "HTTP server was stopped on unused timeout");
        httpd_stop(server);
    }

    shutdown_dns();
    esp_wifi_stop();
    esp_wifi_deinit();

    ESP_LOGI(TAG, "WiFI was stopped on unused timeout");
}
