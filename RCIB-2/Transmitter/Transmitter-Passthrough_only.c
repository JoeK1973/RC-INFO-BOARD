/*
 * This is a pass-through of RC-Timing information from software to ESP32. It does not transmit anything to external ESP32s yet
 * It also has diagnostic output for prior fault-finding which will be removed in the next version
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
 * This build assembles incoming ASCII records, parses the race information
 * locally, and forwards the original record bytes unchanged to the RIDE
 * Board. It does NOT implement ESP-NOW.
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

#include "esp_err.h"
#include "esp_log.h"

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

#define RECORD_QUEUE_LENGTH     8
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

static QueueHandle_t record_queue;
static volatile uint32_t dropped_records = 0;

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

static void log_race_info(const race_info_t *info)
{
    char order[160] = {0};
    char lap_text[160] = {0};
    char improvers[160] = {0};

    size_t used = 0;
    for (size_t i = 0; i < info->car_count; i++) {
        int n = snprintf(order + used, sizeof(order) - used,
                         "%s%d", i ? "," : "", info->cars[i]);
        if (n < 0 || (size_t)n >= sizeof(order) - used) break;
        used += (size_t)n;
    }

    used = 0;
    for (size_t i = 0; i < info->car_count; i++) {
        int n = snprintf(lap_text + used, sizeof(lap_text) - used,
                         "%s%d", i ? "," : "", info->laps[i]);
        if (n < 0 || (size_t)n >= sizeof(lap_text) - used) break;
        used += (size_t)n;
    }

    used = 0;
    for (size_t i = 0; i < info->improver_count; i++) {
        int n = snprintf(improvers + used, sizeof(improvers) - used,
                         "%s%d", i ? "," : "", info->improvers[i]);
        if (n < 0 || (size_t)n >= sizeof(improvers) - used) break;
        used += (size_t)n;
    }

    ESP_LOGI(TAG, "Round %d, race %d, time %d:%02d",
             info->round, info->race, info->minutes, info->seconds);
    ESP_LOGI(TAG, "Order: [%s]  Laps: [%s]", order, lap_text);
    ESP_LOGI(TAG, "Improvers: [%s]", improvers);
}

/* --------------------------- UART receive task --------------------------- */

static void uart_receiver_task(void *arg)
{
    (void)arg;

    uint8_t buffer[RX_BUFFER_SIZE];
    ride_record_t record = {0};
    bool receiving = false;

    ESP_LOGI(TAG, "RC-Timing input ready: UART0 RX GPIO%d, %d baud, 8N1",
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
                ESP_LOGW(TAG, "Discarding oversized/incomplete input record");
                record.length = 0;
                receiving = false;
                continue;
            }

            record.bytes[record.length++] = c;

            if (c == ']') {
                record.bytes[record.length] = '\0';

                race_info_t info;
                if (parse_race_record(record.bytes, &info)) {
                    log_race_info(&info);
                } else {
                    ESP_LOGW(TAG, "Received record could not be parsed: %s",
                             record.bytes);
                }

                if (xQueueSend(record_queue, &record, 0) != pdTRUE) {
                    dropped_records++;
                    ESP_LOGW(TAG, "Forward queue full; dropped record (total %lu)",
                             (unsigned long)dropped_records);
                }

                record.length = 0;
                receiving = false;
            }
        }
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

            if (xQueueReceive(record_queue, &record,
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
    ESP_LOGI(TAG, "RCIB-TX starting (UART receive + parse + USB CDC forward)");
    ESP_LOGI(TAG, "ESP-NOW is not enabled in this firmware");

    record_queue = xQueueCreate(RECORD_QUEUE_LENGTH, sizeof(ride_record_t));
    if (record_queue == NULL) {
        ESP_LOGE(TAG, "Could not create record queue");
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
                     NULL, 6, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create UART receiver task");
        return;
    }

    ok = xTaskCreate(ride_usb_task, "ride_usb_tx", 6144,
                     NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create RIDE USB task");
        return;
    }

    ESP_LOGI(TAG, "Initialisation complete");
}
