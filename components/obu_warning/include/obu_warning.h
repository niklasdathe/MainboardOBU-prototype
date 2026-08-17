#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBU_WARNING_RECENT_IDS 8u
#define OBU_WARNING_TEXT_MAX 48u

typedef enum {
    OBU_WARNING_KIND_UNKNOWN = 0,
    OBU_WARNING_KIND_DENM,
    OBU_WARNING_KIND_SYSTEM,
} obu_warning_kind_t;

typedef enum {
    OBU_WARNING_SEVERITY_INFO = 0,
    OBU_WARNING_SEVERITY_CAUTION,
    OBU_WARNING_SEVERITY_WARNING,
    OBU_WARNING_SEVERITY_CRITICAL,
} obu_warning_severity_t;

/*
 * Canonical payload for OBU_DATA_WARNING events.
 * notification_id must be stable for one active warning episode. For DENM it
 * should be derived from the DENM ActionID so repeated receptions can update
 * the visual state without producing repeated audible notifications.
 */
typedef struct {
    uint64_t notification_id;
    uint32_t source_station_id;
    uint16_t cause_code;
    uint16_t subcause_code;
    obu_warning_kind_t kind;
    obu_warning_severity_t severity;
    bool active;
    bool audible;
    char text[OBU_WARNING_TEXT_MAX];
} obu_warning_notification_t;

typedef struct obu_warning_output obu_warning_output_t;

typedef struct {
    esp_err_t (*notify)(obu_warning_output_t *output,
                        const obu_warning_notification_t *warning);
    esp_err_t (*set_enabled)(obu_warning_output_t *output, bool enabled);
    esp_err_t (*set_muted)(obu_warning_output_t *output, bool muted);
} obu_warning_output_ops_t;

struct obu_warning_output {
    const obu_warning_output_ops_t *ops;
    void *ctx;
};

typedef struct {
    obu_warning_output_t *output;
    bool audio_enabled;
    uint64_t recent_ids[OBU_WARNING_RECENT_IDS];
    uint8_t next_recent;
} obu_warning_controller_t;

typedef struct {
    gpio_num_t gpio;
    uint32_t frequency_hz;
    uint32_t duration_ms;
    uint8_t queue_depth;
} obu_expansion_buzzer_config_t;

void obu_warning_controller_init(obu_warning_controller_t *controller,
                                 obu_warning_output_t *output);
esp_err_t obu_warning_controller_set_audio_enabled(obu_warning_controller_t *controller,
                                                   bool enabled);
esp_err_t obu_warning_controller_handle(obu_warning_controller_t *controller,
                                        const obu_warning_notification_t *warning);

esp_err_t obu_expansion_buzzer_create(const obu_expansion_buzzer_config_t *config,
                                      obu_warning_output_t *out);

/*
 * Queue a short non-warning pulse on an Expansion Base buzzer. This call is
 * non-blocking and deliberately keeps queue capacity in reserve for warning
 * notifications, so diagnostic sounds such as V2X Geiger mode cannot starve
 * safety-warning audio.
 */
esp_err_t obu_expansion_buzzer_pulse(obu_warning_output_t *output,
                                     uint32_t frequency_hz,
                                     uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
