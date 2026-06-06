/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenThread Border Router Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_config.h"
#include "esp_ot_wifi_cmd.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_eventfd.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/uart_types.h"
#include "openthread/error.h"
#include "openthread/instance.h"
#include "openthread/logging.h"
#include "openthread/tasklet.h"

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#if CONFIG_OPENTHREAD_BR_AUTO_START
#include "example_common_private.h"
#include "protocol_examples_common.h"
#endif

#if !CONFIG_OPENTHREAD_BR_AUTO_START && CONFIG_EXAMPLE_CONNECT_ETHERNET
// TZ-1109: Add a menchanism for connecting ETH manually.
#error Currently we do not support a manual way to connect ETH, if you want to use ETH, please enable OPENTHREAD_BR_AUTO_START.
#endif

#define TAG "esp_ot_br"

#if CONFIG_OPENTHREAD_SUPPORT_HW_RESET_RCP
#define PIN_TO_RCP_RESET CONFIG_OPENTHREAD_HW_RESET_RCP_PIN
static void rcp_failure_hardware_reset_handler(void)
{
    gpio_config_t reset_pin_config;
    memset(&reset_pin_config, 0, sizeof(reset_pin_config));
    reset_pin_config.intr_type = GPIO_INTR_DISABLE;
    reset_pin_config.pin_bit_mask = BIT(PIN_TO_RCP_RESET);
    reset_pin_config.mode = GPIO_MODE_OUTPUT;
    reset_pin_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    reset_pin_config.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&reset_pin_config);
    gpio_set_level(PIN_TO_RCP_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TO_RCP_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_reset_pin(PIN_TO_RCP_RESET);
}
#endif

#if CONFIG_EXTERNAL_COEX_ENABLE
static void ot_br_external_coexist_init(void)
{
    esp_external_coex_gpio_set_t gpio_pin = ESP_OPENTHREAD_DEFAULT_EXTERNAL_COEX_CONFIG();
    esp_external_coex_set_work_mode(EXTERNAL_COEX_LEADER_ROLE);
    ESP_ERROR_CHECK(esp_enable_extern_coex_gpio_pin(CONFIG_EXTERNAL_COEX_WIRE_TYPE, gpio_pin));
}
#endif /* CONFIG_EXTERNAL_COEX_ENABLE */

static void ot_task_worker(void *aContext)
{
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t       *openthread_netif = esp_netif_new(&cfg);
    assert(openthread_netif != NULL);

    // Initialize the OpenThread stack
    ESP_ERROR_CHECK(esp_openthread_init(&config));
    ESP_ERROR_CHECK(esp_netif_attach(openthread_netif, esp_openthread_netif_glue_init(&config)));
    esp_openthread_lock_acquire(portMAX_DELAY);
#if CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC
    // The OpenThread log level directly matches ESP log level
    (void)otLoggingSetLevel(CONFIG_LOG_DEFAULT_LEVEL);
#endif // CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC
#if CONFIG_OPENTHREAD_CLI
    esp_openthread_cli_init();
#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_openthread_cli_create_task();
#endif // CONFIG_OPENTHREAD_CLI
    esp_openthread_lock_release();

    // Run the main loop
    esp_openthread_launch_mainloop();

    // Clean up
    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(openthread_netif);
    esp_vfs_eventfd_unregister();
    vTaskDelete(NULL);
}

// NOTE: an earlier revision ran an "RA suppression" task here. On every
// OT_CHANGED_THREAD_NETDATA change it removed the default-route on-mesh (OMR)
// prefix (otBorderRouterRemoveOnMeshPrefix + otBorderRouterRegister), intending
// to stop the device from advertising IPv6 Router Advertisements on the Wi-Fi
// LAN. It was removed because that approach was both ineffective and harmful:
//
//   * ESP-IDF's esp_openthread_border_router_init() already enables the
//     OpenThread Routing Manager, which is what emits the RAs. Deleting the OMR
//     prefix only removes the Thread->infrastructure ROUTE from local Network
//     Data; it does NOT stop RA emission (the on-link PIO + SLLAO are sent
//     regardless), and the routing manager simply re-publishes the prefix.
//   * The BR's RAs carry Router Lifetime 0 — the device is never advertised as
//     an IPv6 default gateway, so it cannot hijack the LAN default route. The
//     host "router" neighbour flag is just the standard NDP IsRouter bit set on
//     any RA sender; it is not "I am your gateway".
//   * The OMR default-route prefix is exactly what gives Thread/Matter devices a
//     route out to the LAN/internet, so removing it degrades the border
//     router's core function (and Matter-over-Thread connectivity).
//
// Manage any genuine LAN-side IPv6 conflict at the router (disable NAT66, keep a
// single RA source on the segment) rather than in firmware. See README
// "Network Notes".

void ot_br_init(void *ctx)
{
#if CONFIG_OPENTHREAD_CLI_WIFI
    ESP_ERROR_CHECK(esp_ot_wifi_config_init());
#endif
#if CONFIG_OPENTHREAD_BR_AUTO_START
#if CONFIG_EXAMPLE_CONNECT_WIFI || CONFIG_EXAMPLE_CONNECT_ETHERNET
    bool wifi_or_ethernet_connected = false;
#else
#error No backbone netif!
#endif
#if CONFIG_EXAMPLE_CONNECT_WIFI
    char wifi_ssid[32] = "";
    char wifi_password[64] = "";
    if (esp_ot_wifi_config_get_ssid(wifi_ssid) == ESP_OK) {
        ESP_LOGI(TAG, "use the Wi-Fi config from NVS");
        esp_ot_wifi_config_get_password(wifi_password);
    } else {
        ESP_LOGI(TAG, "use the Wi-Fi config from Kconfig");
        strcpy(wifi_ssid, CONFIG_EXAMPLE_WIFI_SSID);
        strcpy(wifi_password, CONFIG_EXAMPLE_WIFI_PASSWORD);
    }
    if (esp_ot_wifi_connect(wifi_ssid, wifi_password) == ESP_OK) {
        wifi_or_ethernet_connected = true;
    } else {
        ESP_LOGE(TAG, "Fail to connect to Wi-Fi, please try again manually");
    }
#endif
#if CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(example_ethernet_connect());
    wifi_or_ethernet_connected = true;
#endif
#endif // CONFIG_OPENTHREAD_BR_AUTO_START

#if CONFIG_EXTERNAL_COEX_ENABLE
    ot_br_external_coexist_init();
#endif // CONFIG_EXTERNAL_COEX_ENABLE
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp-ot-br"));

    esp_openthread_lock_acquire(portMAX_DELAY);
#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
    ESP_ERROR_CHECK(esp_openthread_state_indicator_init(esp_openthread_get_instance()));
#endif
#if CONFIG_OPENTHREAD_BR_AUTO_START
    if (wifi_or_ethernet_connected) {
        esp_openthread_set_backbone_netif(get_example_netif());
        ESP_ERROR_CHECK(esp_openthread_border_router_init());
#if CONFIG_EXAMPLE_CONNECT_WIFI
        esp_ot_wifi_border_router_init_flag_set(true);
#endif
        otOperationalDatasetTlvs dataset;
        otError error = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset);
        ESP_ERROR_CHECK(esp_openthread_auto_start((error == OT_ERROR_NONE) ? &dataset : NULL));
    } else {
        ESP_LOGE(TAG, "Auto-start mode failed, please try to start manually");
    }
#endif // CONFIG_OPENTHREAD_BR_AUTO_START
    esp_openthread_lock_release();
    vTaskDelete(NULL);
}

// --- OTBR-compatible REST API (port 8080) ---
// Implements the endpoints Home Assistant's OpenThread Border Router integration
// (python-otbr-api) calls during setup, plus a couple of convenience routes:
//   GET /node                     -> {"State":4}
//   GET /node/dataset/active      -> raw hex TLVs (text/plain), 204 if none   [HA]
//   GET /node/ba-id               -> "<32 hex>" JSON string (border agent id) [HA]
//   GET /node/ext-address         -> "<16 hex>" JSON string (ext address)     [HA]
//   GET /networks/dataset/active  -> {"ActiveDataset":"0e08..."}  (non-standard, kept)
//   GET /dataset                  -> raw hex (convenience)
#include "esp_http_server.h"
#include "openthread/border_agent.h"
#include "openthread/dataset.h"
#include "openthread/link.h"
#include "openthread/thread.h"

// Write n bytes as lowercase hex into out (out must hold 2*n + 1 chars).
static void bytes_to_hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        snprintf(out + i * 2, 3, "%02x", in[i]);
    }
    out[n * 2] = '\0';
}

static void get_dataset_hex(char *buf, size_t buflen)
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    otOperationalDatasetTlvs dataset;
    otError err = otDatasetGetActiveTlvs(instance, &dataset);
    esp_openthread_lock_release();
    buf[0] = '\0';
    if (err != OT_ERROR_NONE) {
        return;
    }
    // Bounded by buflen: need room for 2 hex chars + NUL per byte.
    size_t pos = 0;
    for (int i = 0; i < dataset.mLength && pos + 3 <= buflen; i++) {
        pos += snprintf(buf + pos, buflen - pos, "%02x", dataset.mTlvs[i]);
    }
}

// GET /node  ->  {"State":4}
// State 4 = leader/attached, which tells HA the BR is ready
static esp_err_t node_handler(httpd_req_t *req)
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    otDeviceRole role = otThreadGetDeviceRole(instance);
    esp_openthread_lock_release();
    char resp[64];
    // Map OT role to the OTBR state number HA expects: 4 means "attached/ready".
    int state;
    switch (role) {
    case OT_DEVICE_ROLE_CHILD:
    case OT_DEVICE_ROLE_ROUTER:
    case OT_DEVICE_ROLE_LEADER:
        state = 4;
        break;
    default: // OT_DEVICE_ROLE_DISABLED, OT_DEVICE_ROLE_DETACHED
        state = 1;
        break;
    }
    snprintf(resp, sizeof(resp), "{\"State\":%d}", state);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /networks/dataset/active  ->  {"ActiveDataset":"0e08..."}
static esp_err_t active_dataset_handler(httpd_req_t *req)
{
    char hex[OT_OPERATIONAL_DATASET_MAX_LENGTH * 2 + 1];
    get_dataset_hex(hex, sizeof(hex));
    if (hex[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No active dataset");
        return ESP_FAIL;
    }
    // HA expects: {"ActiveDataset":"<hex>"}
    char resp[OT_OPERATIONAL_DATASET_MAX_LENGTH * 2 + 32];
    snprintf(resp, sizeof(resp), "{\"ActiveDataset\":\"%s\"}", hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /dataset  ->  raw hex (convenience / fallback)
static esp_err_t dataset_handler(httpd_req_t *req)
{
    char hex[OT_OPERATIONAL_DATASET_MAX_LENGTH * 2 + 1];
    get_dataset_hex(hex, sizeof(hex));
    if (hex[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No active dataset");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, hex);
    return ESP_OK;
}

// GET /node/dataset/active  ->  raw hex TLVs as text/plain, 204 if no dataset.
// This is the standard OTBR path python-otbr-api reads (Accept: text/plain).
static esp_err_t node_dataset_active_handler(httpd_req_t *req)
{
    char hex[OT_OPERATIONAL_DATASET_MAX_LENGTH * 2 + 1];
    get_dataset_hex(hex, sizeof(hex));
    httpd_resp_set_type(req, "text/plain");
    if (hex[0] == '\0') {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, hex);
    return ESP_OK;
}

// GET /node/ba-id  ->  "<32 hex>" (JSON string). HA aborts setup if this 404s.
static esp_err_t ba_id_handler(httpd_req_t *req)
{
    otBorderAgentId id;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otBorderAgentGetId(esp_openthread_get_instance(), &id);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No border agent id");
        return ESP_FAIL;
    }
    char hex[OT_BORDER_AGENT_ID_LENGTH * 2 + 1];
    bytes_to_hex(id.mId, OT_BORDER_AGENT_ID_LENGTH, hex);
    char resp[OT_BORDER_AGENT_ID_LENGTH * 2 + 3]; // quotes + NUL
    snprintf(resp, sizeof(resp), "\"%s\"", hex);  // python-otbr-api does json()->fromhex
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /node/ext-address  ->  "<16 hex>" (JSON string).
static esp_err_t ext_address_handler(httpd_req_t *req)
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    otExtAddress ext = *otLinkGetExtendedAddress(esp_openthread_get_instance());
    esp_openthread_lock_release();
    char hex[sizeof(ext.m8) * 2 + 1];
    bytes_to_hex(ext.m8, sizeof(ext.m8), hex);
    char resp[sizeof(ext.m8) * 2 + 3];
    snprintf(resp, sizeof(resp), "\"%s\"", hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static void start_dataset_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start dataset HTTP server");
        return;
    }
    httpd_uri_t uris[] = {
        { .uri = "/node",                    .method = HTTP_GET, .handler = node_handler },
        { .uri = "/node/dataset/active",     .method = HTTP_GET, .handler = node_dataset_active_handler },
        { .uri = "/node/ba-id",              .method = HTTP_GET, .handler = ba_id_handler },
        { .uri = "/node/ext-address",        .method = HTTP_GET, .handler = ext_address_handler },
        { .uri = "/networks/dataset/active", .method = HTTP_GET, .handler = active_dataset_handler },
        { .uri = "/dataset",                 .method = HTTP_GET, .handler = dataset_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "OTBR REST API started on port 8080");
}
// --- end OTBR REST API ---

void app_main(void)
{
    // Used eventfds:
    // * netif
    // * task queue
    // * border router
    esp_vfs_eventfd_config_t eventfd_config = {
#if CONFIG_OPENTHREAD_RADIO_NATIVE || CONFIG_OPENTHREAD_RADIO_SPINEL_SPI
        // * radio driver (A native radio device needs a eventfd for radio driver.)
        // * SpiSpinelInterface (The Spi Spinel Interface needs a eventfd.)
        // The above will not exist at the same time.
        .max_fds = 4,
#else
        .max_fds = 3,
#endif
    };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_OPENTHREAD_SUPPORT_HW_RESET_RCP
    esp_openthread_register_rcp_failure_handler(rcp_failure_hardware_reset_handler);
#endif
    xTaskCreate(ot_task_worker, "ot_br_main", 8192, xTaskGetCurrentTaskHandle(), 5, NULL);
    xTaskCreate(ot_br_init, "ot_br_init", 6144, NULL, 4, NULL);
    start_dataset_server();
}
