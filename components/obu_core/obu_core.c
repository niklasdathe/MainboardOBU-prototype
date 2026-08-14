#include "obu_core.h"
#include <string.h>
#include "esp_timer.h"

void obu_bus_init(obu_bus_t *bus) {
    memset(bus, 0, sizeof(*bus));
    bus->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
}

esp_err_t obu_bus_subscribe(obu_bus_t *bus, QueueHandle_t queue, uint8_t *subscriber_id) {
    if (!bus || !queue) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&bus->lock);
    if (bus->count >= OBU_BUS_MAX_SUBSCRIBERS) {
        taskEXIT_CRITICAL(&bus->lock);
        return ESP_ERR_NO_MEM;
    }
    uint8_t id=bus->count++;
    bus->queues[id]=queue;
    taskEXIT_CRITICAL(&bus->lock);
    if (subscriber_id) *subscriber_id=id;
    return ESP_OK;
}

esp_err_t obu_bus_publish(obu_bus_t *bus, const obu_event_t *event) {
    if (!bus || !event || event->payload_len > OBU_EVENT_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;
    obu_event_t normalized = *event;
    if (!normalized.hub_monotonic_us) normalized.hub_monotonic_us = obu_monotonic_us();
    bool dropped=false;
    taskENTER_CRITICAL(&bus->lock);
    uint8_t count=bus->count;
    QueueHandle_t local[OBU_BUS_MAX_SUBSCRIBERS];
    for (uint8_t i=0;i<count;i++) local[i]=bus->queues[i];
    taskEXIT_CRITICAL(&bus->lock);
    for (uint8_t i=0;i<count;i++) {
        if (xQueueSend(local[i], &normalized, 0) != pdTRUE) {
            taskENTER_CRITICAL(&bus->lock);
            bus->subscriber_drops[i]++;
            taskEXIT_CRITICAL(&bus->lock);
            dropped=true;
        }
    }
    return dropped ? ESP_ERR_TIMEOUT : ESP_OK;
}

uint32_t obu_bus_subscriber_drops(const obu_bus_t *bus, uint8_t id) {
    return (!bus || id >= OBU_BUS_MAX_SUBSCRIBERS) ? 0 : bus->subscriber_drops[id];
}

uint64_t obu_monotonic_us(void) { return (uint64_t)esp_timer_get_time(); }
