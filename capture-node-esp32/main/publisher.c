#include "publisher.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "spool.h"

static const char *TAG = "publisher";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_topic[96];

// Spool flush callback: re-publish one envelope. Stop the flush if we drop offline.
static esp_err_t republish_cb(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (!s_connected) return ESP_FAIL;
    int id = esp_mqtt_client_publish(s_client, s_topic, (const char *)data, (int)len, 1, 0);
    return id >= 0 ? ESP_OK : ESP_FAIL;
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data) {
    (void)args;
    (void)base;
    (void)data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "broker connected; flushing %d spooled", spool_count());
            spool_flush(republish_cb, NULL);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "broker disconnected — spooling until reconnect");
            break;
        default:
            break;
    }
}

esp_err_t publisher_start(const uknomi_config_t *cfg) {
    snprintf(s_topic, sizeof(s_topic), "uknomi/%s/%s/segment", cfg->store, cfg->reg);

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", cfg->broker_host, cfg->broker_port);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .session.keepalive = 60,
    };
    if (cfg->broker_user[0]) {
        mqtt_cfg.credentials.username = cfg->broker_user;
        mqtt_cfg.credentials.authentication.password = cfg->broker_pass;
    }
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) return ESP_FAIL;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_LOGI(TAG, "publishing to %s on %s", s_topic, uri);
    return esp_mqtt_client_start(s_client);
}

bool publisher_is_connected(void) { return s_connected; }

void publisher_send(const uint8_t *data, size_t len) {
    if (!s_connected) {
        spool_write(data, len);  // durable: survives WiFi/broker outage + reboot
        return;
    }
    int id = esp_mqtt_client_publish(s_client, s_topic, (const char *)data, (int)len, 1, 0);
    if (id < 0) {
        spool_write(data, len);
    } else if (spool_count() > 0) {
        spool_flush(republish_cb, NULL);  // drain backlog once we're live again
    }
}
