#include "webconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "webconfig";
#define CONFIG_PATH "/sdcard/config.json"

static httpd_handle_t s_server;
static uknomi_config_t *s_cfg;

// ---- helpers ----------------------------------------------------------------
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode application/x-www-form-urlencoded value in place ('+' -> space, %xx).
static void urldecode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *o++ = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

static void form_str(const char *body, const char *key, char *dst, size_t n) {
    if (httpd_query_key_value(body, key, dst, n) == ESP_OK) urldecode(dst);
}

static int form_int(const char *body, const char *key, int cur) {
    char t[24];
    if (httpd_query_key_value(body, key, t, sizeof(t)) == ESP_OK) {
        urldecode(t);
        return atoi(t);
    }
    return cur;
}

static float form_num(const char *body, const char *key, float cur) {
    char t[24];
    if (httpd_query_key_value(body, key, t, sizeof(t)) == ESP_OK) {
        urldecode(t);
        return (float)atof(t);
    }
    return cur;
}

static bool form_checkbox(const char *body, const char *key) {
    char t[8];  // checkboxes are sent only when checked
    return httpd_query_key_value(body, key, t, sizeof(t)) == ESP_OK;
}

// ---- HTML form --------------------------------------------------------------
static const char *PAGE_FMT =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>uKnomi setup</title><style>body{font-family:sans-serif;max-width:640px;margin:1.5rem auto;"
    "padding:0 1rem}label{display:block;margin:.6rem 0 .15rem;font-weight:600}input{width:100%%;"
    "padding:.5rem;box-sizing:border-box}fieldset{margin:1rem 0;border:1px solid #ccc}h1{font-size:1.3rem}"
    ".row{display:flex;gap:.5rem}.ck{width:auto;margin-right:.4rem}button{margin-top:1rem;padding:.7rem 1.2rem}"
    "</style></head><body><h1>uKnomi capture node</h1><p>%s</p><form method=POST action=/save>"
    "<fieldset><legend>Wi-Fi</legend>"
    "<label>SSID</label><input name=wifi_ssid value=\"%s\">"
    "<label>Password</label><input name=wifi_pass type=password value=\"%s\"></fieldset>"
    "<fieldset><legend>Identity</legend>"
    "<label>Store</label><input name=store value=\"%s\">"
    "<label>Register</label><input name=reg value=\"%s\"></fieldset>"
    "<fieldset><legend>MQTT broker</legend>"
    "<label>Host</label><input name=broker_host value=\"%s\">"
    "<label>Port</label><input name=broker_port value=\"%d\">"
    "<label>Username</label><input name=broker_user value=\"%s\">"
    "<label>Password</label><input name=broker_pass type=password value=\"%s\"></fieldset>"
    "<fieldset><legend>Audio</legend>"
    "<label>VAD threshold</label><input name=vad_threshold value=\"%.2f\">"
    "<label>Pre-roll (ms)</label><input name=preroll_ms value=\"%d\">"
    "<label>Hangover (ms)</label><input name=hangover_ms value=\"%d\">"
    "<label><input class=ck type=checkbox name=retain_audio %s>Retain audio (validation)</label></fieldset>"
    "<fieldset><legend>Camera presence (optional)</legend>"
    "<label><input class=ck type=checkbox name=presence_enabled %s>Enable presence detection</label>"
    "<label>Hold (ms)</label><input name=presence_hold_ms value=\"%d\">"
    "<label>Sensitivity (motion threshold)</label><input name=presence_sensitivity value=\"%.1f\">"
    "</fieldset>"
    "<button type=submit>Save &amp; reboot</button></form></body></html>";

static esp_err_t send_form(httpd_req_t *req, const char *banner) {
    const uknomi_config_t *c = s_cfg;
    char *html = malloc(4096);
    if (!html) return httpd_resp_send_500(req);
    snprintf(html, 4096, PAGE_FMT, banner,
             c->wifi_ssid, c->wifi_pass, c->store, c->reg,
             c->broker_host, c->broker_port, c->broker_user, c->broker_pass,
             c->vad_threshold, c->preroll_ms, c->hangover_ms,
             c->retain_audio ? "checked" : "",
             c->presence_enabled ? "checked" : "", c->presence_hold_ms,
             c->presence_sensitivity);
    httpd_resp_set_type(req, "text/html");
    esp_err_t r = httpd_resp_sendstr(req, html);
    free(html);
    return r;
}

// ---- handlers ---------------------------------------------------------------
static esp_err_t root_get(httpd_req_t *req) {
    return send_form(req, uknomi_config_is_complete(s_cfg)
                              ? "Connected. Edit settings below."
                              : "First-time setup. Fill in Wi-Fi and the broker.");
}

// Captive-portal catch-all: redirect any other URL to the form.
static esp_err_t captive_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t save_post(httpd_req_t *req) {
    int len = req->content_len;
    if (len <= 0 || len > 2048) return httpd_resp_send_500(req);
    char *body = malloc(len + 1);
    if (!body) return httpd_resp_send_500(req);
    int got = 0, r;
    while (got < len && (r = httpd_req_recv(req, body + got, len - got)) > 0) got += r;
    body[got] = '\0';

    uknomi_config_t c = *s_cfg;  // overlay edits onto the current config
    form_str(body, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
    form_str(body, "wifi_pass", c.wifi_pass, sizeof(c.wifi_pass));
    form_str(body, "store", c.store, sizeof(c.store));
    form_str(body, "reg", c.reg, sizeof(c.reg));
    form_str(body, "broker_host", c.broker_host, sizeof(c.broker_host));
    c.broker_port = form_int(body, "broker_port", c.broker_port);
    form_str(body, "broker_user", c.broker_user, sizeof(c.broker_user));
    form_str(body, "broker_pass", c.broker_pass, sizeof(c.broker_pass));
    c.vad_threshold = form_num(body, "vad_threshold", c.vad_threshold);
    c.preroll_ms = form_int(body, "preroll_ms", c.preroll_ms);
    c.hangover_ms = form_int(body, "hangover_ms", c.hangover_ms);
    form_str(body, "codec", c.codec, sizeof(c.codec));
    c.retain_audio = form_checkbox(body, "retain_audio");
    c.presence_enabled = form_checkbox(body, "presence_enabled");
    c.presence_hold_ms = form_int(body, "presence_hold_ms", c.presence_hold_ms);
    c.presence_sensitivity = form_num(body, "presence_sensitivity", c.presence_sensitivity);
    free(body);

    esp_err_t saved = uknomi_config_save(CONFIG_PATH, &c);
    httpd_resp_set_type(req, "text/html");
    if (saved == ESP_OK) {
        *s_cfg = c;
        httpd_resp_sendstr(req, "<html><body><h2>Saved. Rebooting…</h2></body></html>");
        ESP_LOGI(TAG, "config saved via web; rebooting");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } else {
        httpd_resp_sendstr(req, "<html><body><h2>Save failed (SD card?).</h2></body></html>");
    }
    return ESP_OK;
}

// ---- minimal DNS hijack (captive portal) ------------------------------------
static void dns_hijack_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket failed; portal still reachable at 192.168.4.1");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < (int)sizeof(uint16_t) * 6) continue;
        buf[2] |= 0x80;          // QR = response
        buf[3] |= 0x80;          // RA
        buf[7] = 1;              // ANCOUNT = 1 (one answer)
        // Append an answer: name pointer to the question, A record, our AP IP.
        if (n + 16 > (int)sizeof(buf)) continue;
        uint8_t *a = buf + n;
        *a++ = 0xC0; *a++ = 0x0C;            // pointer to question name
        *a++ = 0x00; *a++ = 0x01;            // TYPE A
        *a++ = 0x00; *a++ = 0x01;            // CLASS IN
        *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x3C;  // TTL 60
        *a++ = 0x00; *a++ = 0x04;            // RDLENGTH 4
        *a++ = 192; *a++ = 168; *a++ = 4; *a++ = 1;          // 192.168.4.1
        sendto(sock, buf, n + 16, 0, (struct sockaddr *)&src, slen);
    }
}

// ---- entry ------------------------------------------------------------------
esp_err_t webconfig_start(uknomi_config_t *cfg, bool captive) {
    s_cfg = cfg;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t ret = httpd_start(&s_server, &hc);
    if (ret != ESP_OK) return ret;

    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_register_uri_handler(s_server, &save);
    httpd_register_uri_handler(s_server, &root);

    if (captive) {
        httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = captive_redirect};
        httpd_register_uri_handler(s_server, &any);
        xTaskCreate(dns_hijack_task, "dns", 3072, NULL, 4, NULL);
    }
    ESP_LOGI(TAG, "web config server up (captive=%d)", captive);
    return ESP_OK;
}
