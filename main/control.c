#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "hal/gpio_types.h"
#include "bot.h"

static int STARTER_ON_TIME = 5;

typedef struct
{
    gpio_num_t pin;
    int btn;
    QueueHandle_t qu;
    volatile uint32_t ts;
} isr_context;

static void IRAM_ATTR gpio_isr_handler(void * arg)
{
    isr_context * ctx = arg;
    uint32_t last_ts = ctx->ts;
    ctx->ts = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;

    if (last_ts != 0 && ctx->ts - last_ts <= 200)
        return;

    xQueueSendFromISR(ctx->qu, &ctx->btn, NULL);
}

const int buttons_count = 1;
static isr_context contexts[] = {
    {GPIO_NUM_0, 666, NULL, 0},
};

#define RELAY_PIN GPIO_NUM_4

static void control_relay(bool power_on)
{
    static bool turned_on = false;

    if (power_on && turned_on)
    {
        ESP_LOGI("relay", "relay is busy");
        return;
    }

    gpio_set_level(RELAY_PIN, power_on ? 1 : 0);
    turned_on = power_on;

    if (power_on)
    {
        ESP_LOGI("relay", "turning on");
    }
    else
    {
        sendMessageToAdmin("The starter turned off");
        ESP_LOGI("relay", "turning off");
    }
}

static void timer_callback(TimerHandle_t arg)
{
    (void)arg;
    control_relay(false);
}

static void open_relay(void)
{
    static TimerHandle_t timer_handle = NULL;

    if (timer_handle && xTimerIsTimerActive(timer_handle))
    {
        ESP_LOGI("power", "already on, skipping commmand");
        //xTimerReset(timer_handle, 0);
    }
    else
    {
        sendMessageToAdmin("Turning on the starter in few seconds");
        timer_handle = xTimerCreate("relay pin control off", pdMS_TO_TICKS(STARTER_ON_TIME * 1000), pdFALSE, NULL, timer_callback);
        if (timer_handle)
        {
            BaseType_t res = xTimerStart(timer_handle, 0);
            if (res == pdPASS)
                // enable relay if only everthing ok with timers
                control_relay(true);
        }
    }
}

static void gpio_handle_buttons(void * queue)
{
    while (true)
    {
        int btn;
        if (xQueueReceive(queue, &btn, portMAX_DELAY))
        {
            ESP_LOGI("gpio", "clicked button");
            open_relay();
        }
    }
}


void init_gpio()
{
    gpio_config_t io_conf = {};

    /* init task */
    QueueHandle_t qu = xQueueCreate(10, sizeof(int));
    xTaskCreate(gpio_handle_buttons, "handle_buttons", 2048, (void *)qu, 0, NULL);

    // set up pin mask
    memset(&io_conf, 0, sizeof(io_conf));
    for (size_t i = 0; i < buttons_count; i++)
    {
        isr_context * ctx = &contexts[i];
        io_conf.pin_bit_mask |= 1ULL << ctx->pin;
    }

    //interrupt of low level
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    //io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    /* connect pins with corresponding action */
    for (size_t i = 0; i < buttons_count; i++)
    {
        isr_context * ctx = &contexts[i];
        ctx->qu = qu;
        gpio_isr_handler_add(ctx->pin, gpio_isr_handler, (void *)ctx);
    }

    /* setup pin for relay control */
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1ULL << RELAY_PIN;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}
