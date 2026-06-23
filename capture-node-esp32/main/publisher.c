#include "publisher.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "spool.h"

#define UKNOMI_FW "0.1.0"
#define STATUS_REFRESH_US (60 * 1000000ULL)

static const char *TAG = "publisher";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_topic[96];
static char s_status_topic[96];

// Publish a retained status message so the IP can be discovered off the broker
// (the operator has no access to the store's DHCP table). Retained => the last
// known value is available to the aggregator at any time.
static void publish_status(void) {
    if (!s_connected) return;
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip);
    char ipstr[16];
    esp_ip4addr_ntoa(&ip.ip, ipstr, sizeof(ipstr));

    wifi_ap_record_t ap = {0};
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "ip", ipstr);
    cJSON_AddNumberToObject(o, "rssi", rssi);
    cJSON_AddStringToObject(o, "fw", UKNOMI_FW);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return;
    esp_mqtt_client_publish(s_client, s_status_topic, json, 0, 1, 1);  // qos1, retain
    ESP_LOGI(TAG, "status %s -> http://%s/ (rssi %d)", s_status_topic, ipstr, rssi);
    cJSON_free(json);
}

static void status_timer_cb(void *arg) {
    (void)arg;
    publish_status();
}

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
            publish_status();  // announce our IP as soon as we're online
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
    snprintf(s_status_topic, sizeof(s_status_topic), "uknomi/%s/%s/status", cfg->store, cfg->reg);

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
    esp_err_t ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) return ret;

    // Periodically refresh the retained status (rssi/uptime, and any IP change).
    const esp_timer_create_args_t targs = {.callback = status_timer_cb, .name = "status"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&targs, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, STATUS_REFRESH_US);
    }
    return ESP_OK;
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
