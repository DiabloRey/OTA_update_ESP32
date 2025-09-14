#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"

#define WIFI_SSID      "SSID"	// Edit your SSID here for WiFi
#define WIFI_PASS      "12345678"	//Edit your PSWD here for WiFi
#define OTA_URL        "http://192.168.0.106:8000/firmware.bin"

static const char *TAG = "OTA_HTTP";

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
    }
}

static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

static void print_running_firmware_info(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Running Firmware Info:");
    ESP_LOGI(TAG, " Project: %s", app_desc->project_name);
    ESP_LOGI(TAG, " Version: %s", app_desc->version);
    ESP_LOGI(TAG, " Compiled: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, " ESP-IDF: %s", app_desc->idf_ver);
}

static void ota_task(void *pvParameter)
{
    esp_http_client_config_t config = {
        .url = OTA_URL,
	    .timeout_ms = 60000,
	    .is_async = false,
	    .skip_cert_common_name_check = true,
	    .transport_type = HTTP_TRANSPORT_OVER_TCP,
	    .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_fetch_headers(client);   // <— add this

ESP_LOGI(TAG, "HTTP status %d, content length %d",
         (unsigned int)esp_http_client_get_status_code(client),
         (unsigned int)esp_http_client_get_content_length(client));

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx",
             update_partition->subtype, (unsigned long)update_partition->address);

    /* ---- Robust header read ---- */
    esp_image_header_t img_header;
    esp_image_segment_header_t seg_header;
    esp_app_desc_t new_app_info;

    size_t hdr_size = sizeof(img_header) + sizeof(seg_header) + sizeof(new_app_info);
    uint8_t *hdr_buf = (uint8_t *)malloc(hdr_size);
    if (!hdr_buf) {
        ESP_LOGE(TAG, "Failed to allocate header buffer");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    size_t hdr_received = 0;
    const TickType_t retry_delay = pdMS_TO_TICKS(200);
    const int max_header_wait_ms = 60000;
    int elapsed_ms = 0;

    while (hdr_received < hdr_size) {
        int r = esp_http_client_read(client, (char *)(hdr_buf + hdr_received), hdr_size - hdr_received);
        if (r < 0) {
            ESP_LOGE(TAG, "HTTP read error while fetching header (%d)", r);
            free(hdr_buf);
            esp_http_client_cleanup(client);
            vTaskDelete(NULL);
            return;
        } else if (r == 0) {
            if (elapsed_ms >= max_header_wait_ms) {
                ESP_LOGE(TAG, "Timeout while reading firmware header");
                free(hdr_buf);
                esp_http_client_cleanup(client);
                vTaskDelete(NULL);
                return;
            }
            vTaskDelay(retry_delay);
            elapsed_ms += 200;
            continue;
        } else {
            hdr_received += (size_t)r;
            elapsed_ms = 0;
        }
    }

    memcpy(&img_header, hdr_buf, sizeof(img_header));
    memcpy(&seg_header, hdr_buf + sizeof(img_header), sizeof(seg_header));
    memcpy(&new_app_info, hdr_buf + sizeof(img_header) + sizeof(seg_header), sizeof(new_app_info));

    const esp_app_desc_t *running_app_info = esp_app_get_description();
    ESP_LOGI(TAG, "Current firmware version: %s", running_app_info->version);
    ESP_LOGI(TAG, "Available firmware version: %s", new_app_info.version);

    if (strcmp(new_app_info.version, running_app_info->version) <= 0) {
        ESP_LOGW(TAG, "New firmware is not newer. Skipping OTA.");
        free(hdr_buf);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Proceeding with OTA update...");

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        free(hdr_buf);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_write(update_handle, hdr_buf, hdr_received);
    free(hdr_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write header failed (%s)", esp_err_to_name(err));
        esp_ota_end(update_handle);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int binary_file_len = hdr_received;
    char ota_write_data[1024];
    while (1) {
        int data_read = esp_http_client_read(client, ota_write_data, sizeof(ota_write_data));
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error reading data");
            break;
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "Connection closed, all data received");
            break;
        }

        err = esp_ota_write(update_handle, ota_write_data, data_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            break;
        }
        binary_file_len += data_read;
    }

    ESP_LOGI(TAG, "Total binary length written: %d", binary_file_len);

    if (esp_ota_end(update_handle) == ESP_OK) {
        err = esp_ota_set_boot_partition(update_partition);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA update successful, restarting...");
            esp_restart();
        } else {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "esp_ota_end failed");
    }

    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Version: %s", esp_app_get_description()->version);
    ESP_LOGI(TAG, "Compiled: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    vTaskDelay(pdMS_TO_TICKS(10000));  // wait for IP

    xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        print_running_firmware_info();
    }
}
