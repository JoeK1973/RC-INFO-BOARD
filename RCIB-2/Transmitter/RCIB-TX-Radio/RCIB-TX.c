/*
 * RCIB-TX integrated firmware
 * Target: ESP32-S3, ESP-IDF 6.1
 *
 * Inputs:
 *   RC-Timing PC serial output -> UART0 RX, GPIO44, 9600 8N1
 *
 * Output:
 *   RIDE Board -> ESP32-S3 native USB-OTG host port, CDC-ACM
 *   Expected RIDE VID:PID 0451:16A6
 *
 * Required component:
 *   idf.py add-dependency "espressif/usb_host_cdc_acm"
 *
 * This build assembles incoming ASCII records and fans each record out to
 * independent tasks: priority USB forwarding and lower-priority race parsing.
 * The original record bytes are forwarded unchanged. ESP-NOW broadcasts the
 * latest parsed race state independently of the USB forwarding path.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"

#include "driver/uart.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_types_cdc.h"

/* ----------------------------- Configuration ---------------------------- */

#define RX_UART                 UART_NUM_0
#define RX_GPIO                 44
#define RX_BAUD                 9600
#define RX_BUFFER_SIZE          256
#define RECORD_SIZE             256

#define RIDE_VID                0x0451
#define RIDE_PID                0x16A6
#define RIDE_BAUD               9600

#define USB_QUEUE_LENGTH        8
#define PARSE_QUEUE_LENGTH      8
#define MAX_CARS                32
#define MAX_IMPROVERS           32

static const char *TAG = "RCIB-TX";

typedef struct {
    uint16_t length;
    char bytes[RECORD_SIZE];       /* Includes '[' and ']', excludes NUL */
} ride_record_t;

typedef struct {
    int round;
    int race;
    int minutes;
    int seconds;
    size_t car_count;
    int cars[MAX_CARS];
    int laps[MAX_CARS];
    size_t improver_count;
    int improvers[MAX_IMPROVERS];
} race_info_t;

static QueueHandle_t usb_queue;
static QueueHandle_t parse_queue;
static SemaphoreHandle_t race_info_mutex;

static uint32_t dropped_usb_records = 0;
static uint32_t dropped_parse_records = 0;
static race_info_t latest_race_info;
static bool latest_race_info_valid = false;

/*
 * Public accessor for later RCIB/ESP-NOW development.
 * Returns false until a valid race record has been parsed.
 */
bool rcib_get_latest_race_info(race_info_t *out)
{
    if (out == NULL || race_info_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(race_info_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    bool valid = latest_race_info_valid;
    if (valid) {
        *out = latest_race_info;
    }

    xSemaphoreGive(race_info_mutex);
    return valid;
}

/* ------------------------------- Parser ---------------------------------- */

/* Parse a non-negative decimal integer. Reject empty, non-digit and overflow. */
static bool parse_uint_field(const char *text, int *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    int result = 0;
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
        int digit = text[i] - '0';
        if (result > (INT32_MAX - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }

    *value = result;
    return true;
}

/*
 * Parse comma-separated integers. The protocol normally has a trailing comma,
 * e.g. "6,4,10,". An empty string is valid only for the improvers list.
 */
static bool parse_number_list(const char *text, int *values,
                              size_t capacity, size_t *count,
                              bool allow_empty)
{
    *count = 0;

    if (text == NULL || text[0] == '\0') {
        return allow_empty;
    }

    const char *p = text;
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t token_len = comma ? (size_t)(comma - p) : strlen(p);

        /* A final comma terminates the list; it is not an empty item. */
        if (token_len == 0) {
            return false;
        }
        if (*count >= capacity || token_len >= 12) {
            return false;
        }

        char token[12];
        memcpy(token, p, token_len);
        token[token_len] = '\0';

        int number;
        if (!parse_uint_field(token, &number)) {
            return false;
        }

        values[(*count)++] = number;

        if (comma == NULL) {
            break;
        }

        p = comma + 1;
        if (*p == '\0') {
            break; /* Expected trailing comma */
        }
    }

    return true;
}

/*
 * Split the text inside [...] on ':' while preserving empty fields.
 * The documented format has seven meaningful fields and a final empty field:
 * round:race:mins:secs:cars:laps:improvers:
 */
static bool parse_race_record(const char *record, race_info_t *info)
{
    if (record == NULL || info == NULL) {
        return false;
    }

    size_t length = strlen(record);
    if (length < 4 || record[0] != '[' || record[length - 1] != ']') {
        return false;
    }

    char payload[RECORD_SIZE];
    size_t payload_len = length - 2;
    if (payload_len >= sizeof(payload)) {
        return false;
    }
    memcpy(payload, record + 1, payload_len);
    payload[payload_len] = '\0';

    char *fields[8] = {0};
    size_t field_count = 1;
    fields[0] = payload;

    for (size_t i = 0; i < payload_len; i++) {
        if (payload[i] == ':') {
            payload[i] = '\0';
            if (field_count >= 8) {
                return false;
            }
            fields[field_count++] = &payload[i + 1];
        }
    }

    /* Seven data fields plus the protocol's trailing empty field. */
    if (field_count != 8 || fields[7][0] != '\0') {
        return false;
    }

    memset(info, 0, sizeof(*info));

    if (!parse_uint_field(fields[0], &info->round) ||
        !parse_uint_field(fields[1], &info->race) ||
        !parse_uint_field(fields[2], &info->minutes) ||
        !parse_uint_field(fields[3], &info->seconds)) {
        return false;
    }

    if (info->seconds > 59) {
        return false;
    }

    if (!parse_number_list(fields[4], info->cars, MAX_CARS,
                           &info->car_count, false)) {
        return false;
    }

    /* Parse the two parallel lists independently, then compare their lengths. */
    size_t lap_count = 0;
    if (!parse_number_list(fields[5], info->laps, MAX_CARS,
                           &lap_count, false)) {
        return false;
    }
    if (lap_count != info->car_count) {
        return false;
    }

    if (!parse_number_list(fields[6], info->improvers, MAX_IMPROVERS,
                           &info->improver_count, true)) {
        return false;
    }

    return true;
}

/* --------------------------- UART receive task --------------------------- */

static void uart_receiver_task(void *arg)
{
    (void)arg;

    uint8_t buffer[RX_BUFFER_SIZE];
    ride_record_t record = {0};
    bool receiving = false;

    ESP_LOGI(TAG, "UART receiver ready: GPIO%d, %d baud, 8N1",
             RX_GPIO, RX_BAUD);

    while (1) {
        int count = uart_read_bytes(RX_UART, buffer, sizeof(buffer),
                                    pdMS_TO_TICKS(1000));
        if (count <= 0) {
            continue;
        }

        for (int i = 0; i < count; i++) {
            char c = (char)buffer[i];

            if (c == '[') {
                record.length = 1;
                record.bytes[0] = '[';
                receiving = true;
                continue;
            }

            if (!receiving) {
                continue;
            }

            if (record.length >= RECORD_SIZE - 1) {
                /* Discard malformed/oversized input and resynchronise. */
                record.length = 0;
                receiving = false;
                continue;
            }

            record.bytes[record.length++] = c;

            if (c == ']') {
                record.bytes[record.length] = '\0';

                /*
                 * Forwarding queue is serviced at higher task priority.
                 * Never wait here: UART reception must not be held up by USB.
                 */
                if (xQueueSend(usb_queue, &record, 0) != pdTRUE) {
                    dropped_usb_records++;
                }

                /* Parsing is independent and cannot delay USB forwarding. */
                if (xQueueSend(parse_queue, &record, 0) != pdTRUE) {
                    dropped_parse_records++;
                }

                record.length = 0;
                receiving = false;
            }
        }
    }
}

static void race_parser_task(void *arg)
{
    (void)arg;
    ride_record_t record;

    while (1) {
        if (xQueueReceive(parse_queue, &record, portMAX_DELAY) == pdTRUE) {
            race_info_t parsed;

            if (parse_race_record(record.bytes, &parsed)) {
                if (xSemaphoreTake(race_info_mutex, portMAX_DELAY) == pdTRUE) {
                    latest_race_info = parsed;
                    latest_race_info_valid = true;
                    xSemaphoreGive(race_info_mutex);
                }
            }
            /* Invalid records are ignored; no per-record logging. */
        }
    }
}


/* ------------------------------- ESP-NOW ---------------------------------- */

#define ESPNOW_CHANNEL          6
#define ESPNOW_TX_POWER_QDBM    66      /* 66 quarter-dBm = 16.5 dBm */
#define ESPNOW_INTERVAL_MS      250

static const uint8_t ESPNOW_BROADCAST_MAC[ESP_NOW_ETH_ALEN] =
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define RCIB_PACKET_MAGIC       0x52434942UL /* "RCIB" */
#define RCIB_PACKET_VERSION     1
#define RCIB_PACKET_RACE_DATA   1

/*
 * Keep this wire format in sync with the RCIB-Radio-RX firmware.
 * All multi-byte fields are native ESP32 little-endian values.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  packet_type;
    uint32_t sequence;
    uint32_t round;
    uint32_t race;
    uint32_t elapsed_seconds;
} rcib_radio_packet_t;

static uint32_t radio_sequence = 0;

static void espnow_send_callback(const esp_now_send_info_t *tx_info,
                                 esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "ESP-NOW broadcast send failed");
    }
}

static bool espnow_initialise(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialisation failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Network interface initialisation failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Default event loop creation failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Could not create default Wi-Fi station interface");
        return false;
    }

    const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialisation failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi storage configuration failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi mode configuration failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi channel configuration failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Long Range protocol configuration failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_max_tx_power(ESPNOW_TX_POWER_QDBM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX power configuration failed: %s", esp_err_to_name(err));
        return false;
    }

    int8_t actual_power = 0;
    err = esp_wifi_get_max_tx_power(&actual_power);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi maximum TX power configured: %.2f dBm",
                 actual_power / 4.0f);
    }

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW initialisation failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_now_register_send_cb(espnow_send_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW callback registration failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, ESPNOW_BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "ESP-NOW broadcast peer add failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    esp_now_rate_config_t rate = {
        .phymode = WIFI_PHY_MODE_LR,
        .rate = WIFI_PHY_RATE_LORA_250K,
        .ersu = false,
        .dcm = false,
    };

    err = esp_now_set_peer_rate_config(ESPNOW_BROADCAST_MAC, &rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW 250 Kbps LR rate configuration failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "ESP-NOW ready: broadcast, channel %d, LR 250 Kbps",
             ESPNOW_CHANNEL);
    return true;
}

static void espnow_race_task(void *arg)
{
    (void)arg;

    if (!espnow_initialise()) {
        ESP_LOGE(TAG, "Radio task stopping; USB pass-through remains active");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        race_info_t race;

        if (rcib_get_latest_race_info(&race)) {
            rcib_radio_packet_t packet = {
                .magic = RCIB_PACKET_MAGIC,
                .version = RCIB_PACKET_VERSION,
                .packet_type = RCIB_PACKET_RACE_DATA,
                .sequence = ++radio_sequence,
                .round = (uint32_t)race.round,
                .race = (uint32_t)race.race,
                .elapsed_seconds =
                    ((uint32_t)race.minutes * 60U) + (uint32_t)race.seconds,
            };

            esp_err_t err = esp_now_send(ESPNOW_BROADCAST_MAC,
                                         (const uint8_t *)&packet,
                                         sizeof(packet));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ESP-NOW send request failed: %s",
                         esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ESPNOW_INTERVAL_MS));
    }
}

/* ----------------------------- USB host task ----------------------------- */

static void usb_host_daemon_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB host event error: %s", esp_err_to_name(err));
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB host reports all devices freed");
        }
    }
}

static bool configure_ride_cdc(cdc_acm_dev_hdl_t cdc_dev)
{
    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = RIDE_BAUD,
        .bCharFormat = 0, /* 1 stop bit */
        .bParityType = 0, /* no parity */
        .bDataBits = 8,
    };

    esp_err_t err = cdc_acm_host_line_coding_set(cdc_dev, &line_coding);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Setting RIDE CDC line coding failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = cdc_acm_host_set_control_line_state(cdc_dev, true, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DTR/RTS request failed: %s (continuing)",
                 esp_err_to_name(err));
    }

    return true;
}

static void ride_usb_task(void *arg)
{
    (void)arg;

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 256,
        .in_buffer_size = 64,
        .event_cb = NULL,
        .data_cb = NULL,
        .user_arg = NULL,
    };

    while (1) {
        cdc_acm_dev_hdl_t cdc_dev = NULL;

        ESP_LOGI(TAG, "Waiting for RIDE Board USB CDC %04X:%04X",
                 RIDE_VID, RIDE_PID);

        esp_err_t err = cdc_acm_host_open(RIDE_VID, RIDE_PID, 0,
                                          &dev_config, &cdc_dev);
        if (err != ESP_OK || cdc_dev == NULL) {
            ESP_LOGW(TAG, "RIDE CDC open failed/timed out: %s; retrying",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "RIDE Board CDC connected");

        if (!configure_ride_cdc(cdc_dev)) {
            cdc_acm_host_close(cdc_dev);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool connected = true;
        while (connected) {
            ride_record_t record;

            if (xQueueReceive(usb_queue, &record,
                              pdMS_TO_TICKS(1000)) != pdTRUE) {
                continue;
            }

            err = cdc_acm_host_data_tx_blocking(
                cdc_dev,
                (const uint8_t *)record.bytes,
                record.length,
                2000
            );

            if (err == ESP_OK) {
                ESP_LOGD(TAG, "Forwarded %u-byte RIDE record",
                         (unsigned)record.length);
            } else {
                ESP_LOGW(TAG, "RIDE USB transmit failed: %s; reconnecting",
                         esp_err_to_name(err));
                connected = false;
            }
        }

        cdc_acm_host_close(cdc_dev);
        ESP_LOGW(TAG, "RIDE Board CDC closed; queued records retained");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ------------------------------- app_main -------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "RCIB-TX starting (UART + USB CDC + ESP-NOW race broadcast)");

    usb_queue = xQueueCreate(USB_QUEUE_LENGTH, sizeof(ride_record_t));
    parse_queue = xQueueCreate(PARSE_QUEUE_LENGTH, sizeof(ride_record_t));
    race_info_mutex = xSemaphoreCreateMutex();

    if (usb_queue == NULL || parse_queue == NULL || race_info_mutex == NULL) {
        ESP_LOGE(TAG, "Could not create queues or race-data mutex");
        return;
    }

    const uart_config_t uart_config = {
        .baud_rate = RX_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(RX_UART, RX_BUFFER_SIZE * 2,
                                         0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RX_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(RX_UART, UART_PIN_NO_CHANGE, RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host Library installed");

    BaseType_t ok = xTaskCreate(usb_host_daemon_task, "usb_host_daemon",
                                4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create USB host daemon task");
        return;
    }

    esp_err_t err = cdc_acm_host_install(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CDC-ACM host driver install failed: %s",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "CDC-ACM host driver installed");

    ok = xTaskCreate(uart_receiver_task, "ride_uart_rx", 4096,
                     NULL, 8, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create UART receiver task");
        return;
    }

    ok = xTaskCreate(ride_usb_task, "ride_usb_tx", 6144,
                     NULL, 7, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create RIDE USB task");
        return;
    }

    ok = xTaskCreate(race_parser_task, "race_parser", 4096,
                     NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create race parser task");
        return;
    }

    ok = xTaskCreate(espnow_race_task, "espnow_race_tx", 4096,
                     NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create ESP-NOW race task");
        return;
    }

    ESP_LOGI(TAG, "Initialisation complete: USB pass-through, race parser and ESP-NOW");
}
