#include "obu_warning.h"

#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    uint32_t frequency_hz;
    uint32_t duration_ms;
} buzzer_pattern_t;

typedef struct {
    gpio_num_t gpio;
    uint32_t default_frequency_hz;
    uint32_t default_duration_ms;
    QueueHandle_t queue;
    bool enabled;
    bool muted;
} expansion_buzzer_t;

static bool controller_seen(const obu_warning_controller_t *controller, uint64_t id)
{
    if (id == 0) {
        return false;
    }
    for (size_t i = 0; i < OBU_WARNING_RECENT_IDS; ++i) {
        if (controller->recent_ids[i] == id) {
            return true;
        }
    }
    return false;
}

static void controller_forget(obu_warning_controller_t *controller, uint64_t id)
{
    if (id == 0) {
        return;
    }
    for (size_t i = 0; i < OBU_WARNING_RECENT_IDS; ++i) {
        if (controller->recent_ids[i] == id) {
            controller->recent_ids[i] = 0;
        }
    }
}

void obu_warning_controller_init(obu_warning_controller_t *controller,
                                 obu_warning_output_t *output)
{
    if (controller == NULL) {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    controller->output = output;
    controller->audio_enabled = true;
}

esp_err_t obu_warning_controller_set_audio_enabled(obu_warning_controller_t *controller,
                                                   bool enabled)
{
    if (controller == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    controller->audio_enabled = enabled;
    if (controller->output != NULL && controller->output->ops != NULL &&
        controller->output->ops->set_muted != NULL) {
        return controller->output->ops->set_muted(controller->output, !enabled);
    }
    return ESP_OK;
}

esp_err_t obu_warning_controller_handle(obu_warning_controller_t *controller,
                                        const obu_warning_notification_t *warning)
{
    if (controller == NULL || warning == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!warning->active) {
        controller_forget(controller, warning->notification_id);
        return ESP_OK;
    }
    if (!warning->audible || !controller->audio_enabled ||
        controller->output == NULL || controller->output->ops == NULL ||
        controller->output->ops->notify == NULL) {
        return ESP_OK;
    }
    if (controller_seen(controller, warning->notification_id)) {
        return ESP_OK;
    }

    esp_err_t err = controller->output->ops->notify(controller->output, warning);
    if (err == ESP_OK && warning->notification_id != 0) {
        controller->recent_ids[controller->next_recent] = warning->notification_id;
        controller->next_recent = (uint8_t)((controller->next_recent + 1u) % OBU_WARNING_RECENT_IDS);
    }
    return err;
}

static void buzzer_force_off(void)
{
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void buzzer_task(void *arg)
{
    expansion_buzzer_t *buzzer = arg;
    buzzer_pattern_t pattern;

    for (;;) {
        if (xQueueReceive(buzzer->queue, &pattern, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!buzzer->enabled || buzzer->muted) {
            continue;
        }

        if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, pattern.frequency_hz) != ESP_OK) {
            continue;
        }
        if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512) != ESP_OK ||
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) != ESP_OK) {
            buzzer_force_off();
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(pattern.duration_ms));
        buzzer_force_off();
    }
}

static esp_err_t buzzer_notify(obu_warning_output_t *output,
                               const obu_warning_notification_t *warning)
{
    if (output == NULL || output->ctx == NULL || warning == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    expansion_buzzer_t *buzzer = output->ctx;
    if (!buzzer->enabled || buzzer->muted || !warning->active || !warning->audible) {
        return ESP_OK;
    }

    buzzer_pattern_t pattern = {
        .frequency_hz = buzzer->default_frequency_hz,
        .duration_ms = buzzer->default_duration_ms,
    };
    return xQueueSend(buzzer->queue, &pattern, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t buzzer_set_enabled(obu_warning_output_t *output, bool enabled)
{
    if (output == NULL || output->ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    expansion_buzzer_t *buzzer = output->ctx;
    buzzer->enabled = enabled;
    if (!enabled) {
        buzzer_force_off();
    }
    return ESP_OK;
}

static esp_err_t buzzer_set_muted(obu_warning_output_t *output, bool muted)
{
    if (output == NULL || output->ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    expansion_buzzer_t *buzzer = output->ctx;
    buzzer->muted = muted;
    if (muted) {
        buzzer_force_off();
    }
    return ESP_OK;
}

static const obu_warning_output_ops_t buzzer_ops = {
    .notify = buzzer_notify,
    .set_enabled = buzzer_set_enabled,
    .set_muted = buzzer_set_muted,
};

esp_err_t obu_expansion_buzzer_create(const obu_expansion_buzzer_config_t *config,
                                      obu_warning_output_t *out)
{
    if (config == NULL || out == NULL || config->gpio == GPIO_NUM_NC ||
        config->frequency_hz < 100 || config->frequency_hz > 10000 ||
        config->duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    expansion_buzzer_t *buzzer = calloc(1, sizeof(*buzzer));
    if (buzzer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    buzzer->gpio = config->gpio;
    buzzer->default_frequency_hz = config->frequency_hz;
    buzzer->default_duration_ms = config->duration_ms;
    buzzer->enabled = true;

    uint8_t depth = config->queue_depth == 0 ? 4 : config->queue_depth;
    buzzer->queue = xQueueCreate(depth, sizeof(buzzer_pattern_t));
    if (buzzer->queue == NULL) {
        free(buzzer);
        return ESP_ERR_NO_MEM;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = config->frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        vQueueDelete(buzzer->queue);
        free(buzzer);
        return err;
    }

    ledc_channel_config_t channel = {
        .gpio_num = config->gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        vQueueDelete(buzzer->queue);
        free(buzzer);
        return err;
    }

    if (xTaskCreate(buzzer_task, "obu_buzzer", 3072, buzzer, 5, NULL) != pdPASS) {
        vQueueDelete(buzzer->queue);
        free(buzzer);
        return ESP_ERR_NO_MEM;
    }

    out->ops = &buzzer_ops;
    out->ctx = buzzer;
    return ESP_OK;
}
