/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
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
 *
 * --- Local modifications: ESP32-C6 single-chip Thread Border Router for Home
 *     Assistant. Re-based onto the ESP-IDF v5.5.x ot_br example, which uses the
 *     high-level esp_openthread_start() / esp_openthread_border_router_start()
 *     bring-up (including the explicit Wi-Fi/802.15.4 coexistence enable).
 *
 *     The only addition to the stock example is a minimal OTBR-compatible REST
 *     API on port 8080 (start_dataset_server) so Home Assistant's OpenThread
 *     Border Router integration can read node state, the active dataset, the
 *     border-agent id and the ext address over the Wi-Fi LAN.
 *
 *     There is intentionally NO "RA suppression" here. The OpenThread routing
 *     manager (enabled by esp_openthread_border_router_init) advertises RAs with
 *     Router Lifetime 0 — it is never the LAN default gateway — and the OMR
 *     default-route prefix it publishes is exactly what gives Thread/Matter
 *     devices a route to the LAN/internet. Manage any genuine LAN-side IPv6
 *     conflict at the router (disable NAT66, single RA source). See README.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_coexist.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_spinel.h"
#include "esp_openthread_types.h"
#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_config.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "ot_examples_br.h"
#include "ot_examples_common.h"

// --- OTBR REST API dependencies (local addition) ---
#include "esp_http_server.h"
#include "openthread/border_agent.h"
#include "openthread/dataset.h"
#include "openthread/link.h"
#include "openthread/message.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
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

// --- OTBR-compatible REST API (port 8080) ---
// Implements the endpoints Home Assistant's OpenThread Border Router integration
// (python-otbr-api) calls during setup, plus a couple of convenience routes:
//   GET /node                     -> {"State":4}
//   GET /node/dataset/active      -> raw hex TLVs (text/plain), 204 if none   [HA]
//   GET /node/ba-id               -> "<32 hex>" JSON string (border agent id) [HA]
//   GET /node/ext-address         -> "<16 hex>" JSON string (ext address)     [HA]
//   GET /networks/dataset/active  -> {"ActiveDataset":"0e08..."}  (non-standard, kept)
//   GET /dataset                  -> raw hex (convenience)
//   GET /logs                     -> recent device log lines (text/plain, oldest first)
//   GET /heap                     -> {"FreeHeap":..,"MinFreeHeap":..,"LargestFreeBlock":..}
//   GET /diag                     -> Thread table usage vs. limits (children, routers, SRP, ...)

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

// --- /logs: in-RAM log ring buffer (local addition) ---
// Mirrors every esp_log line into a fixed circular buffer so the most recent
// output can be read back over WiFi (the board is otherwise only observable on
// the UART0 / USB-Serial-JTAG console). The hook still forwards to the original
// vprintf, so serial logging is unchanged. Sized to stay well within the 4 MB
// build's RAM budget; holds roughly the last ~80-100 lines.
#define LOG_RING_SIZE 8192
static char s_log_ring[LOG_RING_SIZE];
static size_t s_log_head;                                       // next write index
static bool s_log_wrapped;                                      // ring filled at least once
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;                           // original console sink

static void log_ring_write(const char *data, size_t len)
{
    portENTER_CRITICAL(&s_log_mux);
    for (size_t i = 0; i < len; i++) {
        s_log_ring[s_log_head++] = data[i];
        if (s_log_head >= LOG_RING_SIZE) {
            s_log_head = 0;
            s_log_wrapped = true;
        }
    }
    portEXIT_CRITICAL(&s_log_mux);
}

// esp_log hook: capture into the ring, then forward to the original sink so the
// serial console keeps working. Called in task context only (ISR/early logs go
// through a different path), so the short critical section in log_ring_write is safe.
static int log_vprintf_hook(const char *fmt, va_list ap)
{
    char line[256];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap_copy);
    va_end(ap_copy);
    if (n > 0) {
        size_t len = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        log_ring_write(line, len);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : n;
}

static void start_log_capture(void)
{
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf_hook);
}

// GET /logs  ->  the ring buffer contents as text/plain, oldest line first.
static esp_err_t logs_handler(httpd_req_t *req)
{
    char *buf = malloc(LOG_RING_SIZE);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    // Snapshot under the lock (linearising the circular buffer), then send outside it.
    size_t len;
    bool wrapped;
    portENTER_CRITICAL(&s_log_mux);
    wrapped = s_log_wrapped;
    if (s_log_wrapped) {
        size_t tail = LOG_RING_SIZE - s_log_head;  // head..end holds the oldest bytes
        memcpy(buf, s_log_ring + s_log_head, tail);
        memcpy(buf + tail, s_log_ring, s_log_head);
        len = LOG_RING_SIZE;
    } else {
        memcpy(buf, s_log_ring, s_log_head);
        len = s_log_head;
    }
    portEXIT_CRITICAL(&s_log_mux);

    // Once wrapped, the oldest retained byte usually lands mid-line; drop that
    // leading fragment so the response always starts on a clean line boundary.
    char *out = buf;
    size_t out_len = len;
    if (wrapped) {
        char *nl = memchr(buf, '\n', len);
        if (nl != NULL) {
            out = nl + 1;
            out_len = len - (size_t)(out - buf);
        }
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, out, out_len);
    free(buf);
    return ESP_OK;
}

// GET /heap  ->  current heap headroom as JSON. Diagnostic: every Thread/Matter
// service the advertising proxy mirrors onto the LAN costs heap (mDNS + SRP
// server entries), so watch MinFreeHeap as the mesh grows — it is the low-water
// mark since boot. No OpenThread state is read, so no lock is needed.
static esp_err_t heap_handler(httpd_req_t *req)
{
    char resp[112];
    int n = snprintf(resp, sizeof(resp),
                     "{\"FreeHeap\":%u,\"MinFreeHeap\":%u,\"LargestFreeBlock\":%u}",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}

// GET /diag  ->  usage of the fixed-size Thread tables vs. their limits, as JSON.
// Diagnostic for "a new device won't join until the BR is restarted": read this
// BEFORE restarting. Children == ChildrenMax means the BR silently ignores MLE
// Parent Requests (a joiner that only hears the BR can't attach); a low
// MsgBuffersFree / high MsgBuffersMaxUsed means message-pool exhaustion;
// AddrCacheQuerying close to AddrCacheMax means LAN->Thread forwarding is stalled
// on address queries; SrpHosts/SrpServices track what the advertising proxy
// mirrors onto mDNS. Uptime (seconds) tells you how long state has accumulated.
static esp_err_t diag_handler(httpd_req_t *req)
{
    uint16_t children = 0, children_sleepy = 0, children_max;
    uint16_t routers = 0, router_links = 0, neighbors = 0;
    uint16_t srp_hosts = 0, srp_services = 0;
    uint16_t cache_entries = 0, cache_querying = 0;
    otBufferInfo buffers;

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    const char *role = otThreadDeviceRoleToString(otThreadGetDeviceRole(instance));

    children_max = otThreadGetMaxAllowedChildren(instance);
    for (uint16_t i = 0; i < children_max; i++) {
        otChildInfo child;
        if (otThreadGetChildInfoByIndex(instance, i, &child) == OT_ERROR_NONE) {
            children++;
            if (!child.mRxOnWhenIdle) {
                children_sleepy++;
            }
        }
    }

    uint8_t max_router_id = otThreadGetMaxRouterId(instance);
    for (uint16_t id = 0; id <= max_router_id; id++) {
        otRouterInfo router;
        if (otThreadGetRouterInfo(instance, id, &router) == OT_ERROR_NONE && router.mAllocated) {
            routers++;
            if (router.mLinkEstablished) {
                router_links++;
            }
        }
    }

    otNeighborInfoIterator neighbor_iter = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo neighbor;
    while (otThreadGetNextNeighborInfo(instance, &neighbor_iter, &neighbor) == OT_ERROR_NONE) {
        neighbors++;
    }

    for (const otSrpServerHost *host = otSrpServerGetNextHost(instance, NULL); host != NULL;
         host = otSrpServerGetNextHost(instance, host)) {
        if (otSrpServerHostIsDeleted(host)) {
            continue;
        }
        srp_hosts++;
        for (const otSrpServerService *svc = otSrpServerHostGetNextService(host, NULL); svc != NULL;
             svc = otSrpServerHostGetNextService(host, svc)) {
            if (!otSrpServerServiceIsDeleted(svc)) {
                srp_services++;
            }
        }
    }

    otCacheEntryIterator cache_iter;
    memset(&cache_iter, 0, sizeof(cache_iter));
    otCacheEntryInfo entry;
    while (otThreadGetNextCacheEntry(instance, &entry, &cache_iter) == OT_ERROR_NONE) {
        cache_entries++;
        if (entry.mState == OT_CACHE_ENTRY_STATE_QUERY || entry.mState == OT_CACHE_ENTRY_STATE_RETRY_QUERY) {
            cache_querying++;
        }
    }

    otMessageGetBufferInfo(instance, &buffers);
    esp_openthread_lock_release();

    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "{\"Uptime\":%lu,\"Role\":\"%s\","
                     "\"Children\":%u,\"ChildrenSleepy\":%u,\"ChildrenMax\":%u,"
                     "\"Routers\":%u,\"RouterLinks\":%u,\"Neighbors\":%u,"
                     "\"SrpHosts\":%u,\"SrpServices\":%u,"
                     "\"AddrCache\":%u,\"AddrCacheQuerying\":%u,\"AddrCacheMax\":%u,"
                     "\"MsgBuffersTotal\":%u,\"MsgBuffersFree\":%u,\"MsgBuffersMaxUsed\":%u}",
                     (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ), role,
                     children, children_sleepy, children_max,
                     routers, router_links, neighbors,
                     srp_hosts, srp_services,
                     cache_entries, cache_querying, (unsigned)CONFIG_OPENTHREAD_TMF_ADDR_CACHE_ENTRIES,
                     buffers.mTotalBuffers, buffers.mFreeBuffers, buffers.mMaxUsedBuffers);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}

static void start_dataset_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.max_uri_handlers = 12;  // default is 8; we register 9 and want headroom
    config.uri_match_fn = httpd_uri_match_wildcard;
    // The default 7 client sockets + 3 internal ones use up all of
    // CONFIG_LWIP_MAX_SOCKETS (10). Without these two, a client that vanishes
    // mid-connection (Wi-Fi drop/roam) leaves a half-open socket the server never
    // reclaims; after 7 of those the REST API refuses every new connection until
    // a reboot. keep_alive_enable turns on TCP keepalive so dead peers are reaped
    // (~20 s idle); lru_purge_enable evicts the oldest session if all are in use.
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
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
        { .uri = "/logs",                    .method = HTTP_GET, .handler = logs_handler },
        { .uri = "/heap",                    .method = HTTP_GET, .handler = heap_handler },
        { .uri = "/diag",                    .method = HTTP_GET, .handler = diag_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "OTBR REST API started on port 8080");
}
// --- end OTBR REST API ---

void app_main(void)
{
    // Local addition: start mirroring esp_log output into the /logs ring buffer
    // first, so the OpenThread bring-up sequence is captured from the start.
    start_log_capture();

    // Used eventfds:
    // * netif
    // * task queue
    // * border router
    size_t max_eventfd = 3;

#if CONFIG_OPENTHREAD_RADIO_NATIVE || CONFIG_OPENTHREAD_RADIO_SPINEL_SPI
    // * radio driver (A native radio device needs a eventfd for radio driver.)
    // * SpiSpinelInterface (The Spi Spinel Interface needs a eventfd.)
    // The above will not exist at the same time.
    max_eventfd++;
#endif
#if CONFIG_OPENTHREAD_RADIO_TREL
    // * TREL reception (The Thread Radio Encapsulation Link needs a eventfd for reception.)
    max_eventfd++;
#endif
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = max_eventfd,
    };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp-ot-br"));
#if CONFIG_OPENTHREAD_SUPPORT_HW_RESET_RCP
    esp_openthread_register_rcp_failure_handler(rcp_failure_hardware_reset_handler);
#endif

#if CONFIG_OPENTHREAD_CLI
    ot_console_start();
    ot_register_external_commands();
#endif

#if CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
    ot_external_coexist_init();
#endif

    static esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };

    ESP_ERROR_CHECK(esp_openthread_start(&config));
#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif
#if CONFIG_OPENTHREAD_BORDER_ROUTER_AUTO_START
    ESP_ERROR_CHECK(esp_openthread_border_router_start());
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
#endif
#endif
#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
    ESP_ERROR_CHECK(esp_openthread_state_indicator_init(esp_openthread_get_instance()));
#endif
#if CONFIG_OPENTHREAD_NETWORK_AUTO_START
    ot_network_auto_start();
#endif

    // Local addition: start the OTBR-compatible REST API for Home Assistant.
    start_dataset_server();
}
