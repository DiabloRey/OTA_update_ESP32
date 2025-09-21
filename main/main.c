#include "main.h"

#ifdef ENC_DECR
#include "mbedtls/aes.h"

// Example 128-bit key (16 bytes)
static const unsigned char aes_key[16] = {
    0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C,
    0x0D, 0x0E, 0x0F, 0x10
};

// IV (Initialization Vector) for CBC mode (16 bytes)
static const unsigned char aes_iv[16] = {
    0x10, 0x0F, 0x0E, 0x0D,
    0x0C, 0x0B, 0x0A, 0x09,
    0x08, 0x07, 0x06, 0x05,
    0x04, 0x03, 0x02, 0x01
};
#endif

#define OTA_CHUNK_SIZE 1024
uint8_t buffer[OTA_CHUNK_SIZE + 16];    // Shared buffer

#define HEADER_SIZE 0x100   // leave first 256 bytes unencrypted

#define WIFI_SSID      "SSID"  // provide your WiFi SSID
#define WIFI_PASS      "PSWD"  // provide your WiFi PSWD
#define OTA_URL        "http://172.24.231.57:8000/firmware_encrypted.bin"

static const char *TAG = "OTA_HTTP";
static bool wifi_connected = false;

/* ---------- Wi-Fi ---------- */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying Wi-Fi connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------- Print firmware info once ---------- */
static void print_running_firmware_info(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (!app_desc) return;
    ESP_LOGI(TAG, "Running Firmware Info:");
    ESP_LOGI(TAG, " Project: %s", app_desc->project_name);
    ESP_LOGI(TAG, " Version: %s", app_desc->version);
    ESP_LOGI(TAG, " Compiled: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, " ESP-IDF: %s", app_desc->idf_ver);
}

/* ---------- do_ota_update ---------- */
static bool do_ota_update(void)
{
    bool updated = false;
    esp_err_t err;

    ESP_LOGI(TAG, "Starting OTA...");

    esp_http_client_config_t config = {
        .url = OTA_URL,
        .timeout_ms = 60000,
        .is_async = false,
        .skip_cert_common_name_check = true,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGW(TAG, "Server not available");
        esp_http_client_cleanup(client);
        return false;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status code: %d", status);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP server returned error");
    }

#ifdef ENC_DECR
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, aes_key, 128) != 0) {
        ESP_LOGE(TAG, "AES key setup failed");
        goto cleanup;
    }
    uint8_t iv[16];
    memcpy(iv, aes_iv, 16);
    uint8_t decrypted[OTA_CHUNK_SIZE + 16];
    int leftover_len = 0;
#endif

    // --- Read OTA header (plaintext) ---
    uint8_t header_buf[HEADER_SIZE];
    int read_bytes = 0;
    while (read_bytes < HEADER_SIZE) {
        int r = esp_http_client_read(client, (char *)header_buf + read_bytes,
                                     HEADER_SIZE - read_bytes);
        if (r <= 0) {
            ESP_LOGE(TAG, "Failed to read OTA header, read=%d", r);
            goto cleanup;
        }
        read_bytes += r;
    }

    esp_app_desc_t new_app_desc;
    // esp_app_desc_t starts at offset 0x20
    memcpy(&new_app_desc, header_buf + 0x20, sizeof(esp_app_desc_t));


    // ---------------- Version Check ----------------
    const esp_app_desc_t *current_app = esp_app_get_description();
    if (new_app_desc.version[0] == '\0' || strlen(new_app_desc.version) >= sizeof(new_app_desc.version)) {
        ESP_LOGE(TAG, "Invalid firmware header (version string corrupted), skipping OTA");
        goto cleanup;
    }

    if (strcmp(new_app_desc.project_name, current_app->project_name) == 0 &&
        strcmp(new_app_desc.version, current_app->version) == 0) {
        ESP_LOGI(TAG, "Firmware up-to-date: %s v%s", new_app_desc.project_name, new_app_desc.version);
        goto cleanup;
    }

    ESP_LOGI(TAG, "New firmware found: %s v%s (current: %s v%s)",
             new_app_desc.project_name, new_app_desc.version,
             current_app->project_name, current_app->version);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        goto cleanup;
    }

    esp_ota_handle_t update_handle = 0;
    if ((err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        goto cleanup;
    }

    // Write header (plaintext, unencrypted)
    if ((err = esp_ota_write(update_handle, header_buf, read_bytes)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
        goto cleanup;
    }

    int total_written = read_bytes;

    // --- OTA main loop ---
    while (1) {
        int r = esp_http_client_read(client, (char *)buffer + leftover_len, OTA_CHUNK_SIZE - leftover_len);
        if (r < 0) {
            ESP_LOGE(TAG, "HTTP read error: %d", r);
            break;
        } else if (r == 0) {
#ifdef ENC_DECR
            if (leftover_len > 0) {
                if (leftover_len % 16 != 0) break;
                if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, leftover_len, iv, buffer, decrypted) != 0) break;
                int pad = decrypted[leftover_len - 1];
                if (pad <= 0 || pad > 16) pad = 0;
                esp_ota_write(update_handle, decrypted, leftover_len - pad);
            }
#endif
            break;
        }

#ifdef ENC_DECR
        leftover_len += r;
        int full_blocks2 = (leftover_len / 16) * 16;
        if (full_blocks2 > 0) {
            if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, full_blocks2, iv, buffer, decrypted) != 0) break;
            esp_ota_write(update_handle, decrypted, full_blocks2);
            total_written += full_blocks2;
            leftover_len -= full_blocks2;
            if (leftover_len > 0) memmove(buffer, buffer + full_blocks2, leftover_len);
        }
#else
        esp_ota_write(update_handle, buffer, r);
        total_written += r;
#endif
    }

    ESP_LOGI(TAG, "Total written: %d bytes", total_written);

    if (esp_ota_end(update_handle) == ESP_OK) {
        if ((err = esp_ota_set_boot_partition(update_partition)) == ESP_OK) {
            ESP_LOGI(TAG, "OTA complete, rebooting...");
            updated = true;
            esp_restart();
        }
    }

cleanup:
#ifdef ENC_DECR
    mbedtls_aes_free(&aes);
#endif
    esp_http_client_cleanup(client);
    return updated;
}

/* ---------- ota_task ---------- */
void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "OTA Task started");

    const TickType_t wifi_retry_delay = pdMS_TO_TICKS(5000);
    const TickType_t ota_retry_delay  = pdMS_TO_TICKS(30000);

    bool ota_done = false;
    int ota_fail_count = 0;

    while (1) {
        if (!wifi_connected) {
            ESP_LOGW(TAG, "Wi-Fi not connected, retrying in 5s...");
            vTaskDelay(wifi_retry_delay);
            continue;
        }

        if (!ota_done) {
            ESP_LOGI(TAG, "Checking for OTA update...");
            bool updated = do_ota_update();
            if (updated) {
                ota_done = true;
                break;
            } else {
                ota_fail_count++;
                ESP_LOGW(TAG, "OTA attempt %d failed, retrying...", ota_fail_count);
                TickType_t backoff = ota_retry_delay * (ota_fail_count > 5 ? 5 : ota_fail_count);
                vTaskDelay(backoff);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }

    vTaskDelete(NULL);
}

/* ---------- app_main ---------- */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();
    print_running_firmware_info();

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback() returned %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Marked OTA app as valid");
        }
    } else {
        ESP_LOGI(TAG, "Running from factory partition (no mark valid)");
    }

    xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);
}
