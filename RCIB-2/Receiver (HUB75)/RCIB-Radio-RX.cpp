#include "hub75.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "RCIB_RADIO_RX";

static constexpr uint32_t RCIB_MAGIC = 0x52434942; // "RCIB"
static constexpr uint8_t RADIO_PROTOCOL_VERSION = 1;
static constexpr uint8_t PACKET_TYPE_RACE_DATA = 1;

static const uint8_t WIFI_CHANNEL = 6;
static const uint32_t LINK_TIMEOUT_MS = 1500;
static const uint32_t DISPLAY_INTERVAL_MS = 3000;

static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] =
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// This structure must match rcib_radio_packet_t in RCIB-TX-Radio.
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t packet_type;
    uint32_t sequence;
    uint32_t round;
    uint32_t race;
    uint32_t elapsed_seconds;
} rcib_radio_packet_t;

typedef struct {
    rcib_radio_packet_t packet;
    uint8_t source_mac[ESP_NOW_ETH_ALEN];
    int8_t rssi;
} received_message_t;

static QueueHandle_t rx_queue = nullptr;

// Compact 3x5 font copied from the working HUB75 test.
// Each row uses the bottom 3 bits.
static const uint8_t font[][5] = {
    {7, 5, 5, 5, 7}, // 0
    {2, 6, 2, 2, 7}, // 1
    {7, 1, 7, 4, 7}, // 2
    {7, 1, 7, 1, 7}, // 3
    {5, 5, 7, 1, 1}, // 4
    {7, 4, 7, 1, 7}, // 5
    {7, 4, 7, 5, 7}, // 6
    {7, 1, 1, 1, 1}, // 7
    {7, 5, 7, 5, 7}, // 8
    {7, 5, 7, 1, 7}, // 9
    {6, 5, 6, 5, 5}, // R
    {5, 5, 7, 5, 5}, // H
    {0, 2, 0, 2, 0}  // :
};

static int font_index(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == 'R') return 10;
    if (c == 'H') return 11;
    if (c == ':') return 12;
    return -1;
}

static void draw_text(Hub75Driver &display, const char *text)
{
    display.clear();

    const int scale = 4;
    const int char_height = 20;
    const int gap = 1;

    int widths[6] = {};
    int count = 0;
    int total_width = 0;

    while (text[count] != '\0' && count < 6) {
        widths[count] = (text[count] == ':') ? 4 : 12;
        total_width += widths[count];
        count++;
    }

    if (count == 0) return;

    total_width += (count - 1) * gap;
    int x = (64 - total_width) / 2;
    int y = (32 - char_height) / 2;

    for (int i = 0; i < count; i++) {
        if (text[i] == ':') {
            display.fill(x, y + 2, 4, 4, 255, 0, 0);
            display.fill(x, y + 14, 4, 4, 255, 0, 0);
        } else {
            const int index = font_index(text[i]);
            if (index >= 0) {
                for (int row = 0; row < 5; row++) {
                    for (int col = 0; col < 3; col++) {
                        if (font[index][row] & (1 << (2 - col))) {
                            display.fill(
                                x + col * scale,
                                y + row * scale,
                                scale,
                                scale,
                                255, 0, 0
                            );
                        }
                    }
                }
            }
        }
        x += widths[i] + gap;
    }
}

static void format_race_id(uint32_t round, uint32_t race, char output[7])
{
    // Five characters: R3:H3. The existing font draws R and H in capitals.
    snprintf(output, 7, "R%u:H%u", (unsigned)(round % 10),
             (unsigned)(race % 10));
}

static void format_elapsed_time(uint32_t elapsed_seconds, char output[6])
{
    const uint32_t minutes = (elapsed_seconds / 60U) % 100U;
    const uint32_t seconds = elapsed_seconds % 60U;
    snprintf(output, 6, "%02" PRIu32 ":%02" PRIu32, minutes, seconds);
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_set_protocol(
        WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
        WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR
    ));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "Receiver STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Wi-Fi channel %u, ESP-NOW Long Range receive enabled",
             WIFI_CHANNEL);
}

static void espnow_receive_callback(const esp_now_recv_info_t *recv_info,
                                    const uint8_t *data,
                                    int data_len)
{
    if (recv_info == nullptr || data == nullptr ||
        data_len != (int)sizeof(rcib_radio_packet_t)) {
        return;
    }

    rcib_radio_packet_t packet;
    memcpy(&packet, data, sizeof(packet));

    if (packet.magic != RCIB_MAGIC ||
        packet.version != RADIO_PROTOCOL_VERSION ||
        packet.packet_type != PACKET_TYPE_RACE_DATA) {
        return;
    }

    received_message_t message = {};
    message.packet = packet;
    memcpy(message.source_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);

    // ESP-NOW receive callback runs in the Wi-Fi task. Never draw to HUB75
    // or perform lengthy work here; hand the packet to app_main instead.
    if (rx_queue != nullptr) {
        (void)xQueueSend(rx_queue, &message, 0);
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting RCIB radio receiver");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    Hub75Config config{};
    config.panel_width = 64;
    config.panel_height = 32;
    config.scan_wiring = Hub75ScanWiring::STANDARD_TWO_SCAN;

    config.pins.r1 = 1;
    config.pins.g1 = 2;
    config.pins.b1 = 4;
    config.pins.r2 = 5;
    config.pins.g2 = 6;
    config.pins.b2 = 7;

    config.pins.a = 8;
    config.pins.b = 9;
    config.pins.c = 10;
    config.pins.d = 11;
    config.pins.e = -1;

    config.pins.clk = 12;
    config.pins.lat = 13;
    config.pins.oe = 14;

    config.output_clock_speed = Hub75ClockSpeed::HZ_8M;
    config.min_refresh_rate = 60;
    config.brightness = 32;
    config.layout_rows = 1;
    config.layout_cols = 1;
    config.shift_driver = Hub75ShiftDriver::GENERIC;

    Hub75Driver display(config);
    if (!display.begin()) {
        ESP_LOGE(TAG, "HUB75 display initialisation failed");
        return;
    }
    ESP_LOGI(TAG, "HUB75 display initialised");

    draw_text(display, "00:00");

    rx_queue = xQueueCreate(10, sizeof(received_message_t));
    if (rx_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create receive queue");
        return;
    }

    wifi_init();

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_receive_callback));

    ESP_LOGI(TAG, "ESP-NOW receiver ready");
    ESP_LOGI(TAG, "Waiting for transmitter broadcast on channel %u", WIFI_CHANNEL);
    ESP_LOGI(TAG, "Display alternates between round/heat and elapsed time every 3 seconds");

    uint32_t packets_received = 0;
    uint32_t packets_missed = 0;
    uint32_t last_sequence = 0;
    uint32_t last_packet_tick = 0;
    uint32_t last_display_switch_tick = xTaskGetTickCount();
    uint32_t last_displayed_value = UINT32_MAX;
    bool have_packet = false;
    bool link_was_up = false;
    bool show_race_id = true;
    uint8_t transmitter_mac[ESP_NOW_ETH_ALEN] = {};
    rcib_radio_packet_t latest_packet = {};
    received_message_t message;

    while (true) {
        if (xQueueReceive(rx_queue, &message, pdMS_TO_TICKS(100)) == pdTRUE) {
            const uint32_t sequence = message.packet.sequence;

            if (have_packet) {
                if (sequence > last_sequence + 1) {
                    packets_missed += sequence - last_sequence - 1;
                } else if (sequence <= last_sequence) {
                    ESP_LOGW(TAG, "Transmitter sequence restarted/wrapped: "
                                  "%" PRIu32 " -> %" PRIu32,
                             last_sequence, sequence);
                }
            }

            packets_received++;
            last_sequence = sequence;
            last_packet_tick = xTaskGetTickCount();
            have_packet = true;
            link_was_up = true;
            latest_packet = message.packet;
            memcpy(transmitter_mac, message.source_mac, ESP_NOW_ETH_ALEN);

            // Show the race identifier as soon as the first valid packet arrives.
            if (packets_received == 1) {
                char display_text[7];
                format_race_id(latest_packet.round, latest_packet.race,
                               display_text);
                draw_text(display, display_text);
                last_display_switch_tick = xTaskGetTickCount();
                last_displayed_value = latest_packet.round * 100U + latest_packet.race;
            }

            if ((packets_received % 20) == 1) {
                ESP_LOGI(TAG,
                         "RX=%" PRIu32 ", seq=%" PRIu32
                         ", missed=%" PRIu32
                         ", round=%" PRIu32 ", race=%" PRIu32
                         ", elapsed=%" PRIu32 " s"
                         ", TX=%02X:%02X:%02X:%02X:%02X:%02X",
                         packets_received, last_sequence, packets_missed,
                         latest_packet.round, latest_packet.race,
                         latest_packet.elapsed_seconds,
                         transmitter_mac[0], transmitter_mac[1],
                         transmitter_mac[2], transmitter_mac[3],
                         transmitter_mac[4], transmitter_mac[5]);
            }
        }

        const uint32_t now = xTaskGetTickCount();
        const bool link_up = have_packet &&
            (now - last_packet_tick) < pdMS_TO_TICKS(LINK_TIMEOUT_MS);

        if (!link_up && link_was_up) {
            ESP_LOGW(TAG, "Radio link lost; last seq=%" PRIu32
                          ", received=%" PRIu32 ", missed=%" PRIu32,
                     last_sequence, packets_received, packets_missed);
            link_was_up = false;
            draw_text(display, "00:00");
            last_displayed_value = UINT32_MAX;
        }

        if (link_up) {
            if ((now - last_display_switch_tick) >=
                pdMS_TO_TICKS(DISPLAY_INTERVAL_MS)) {
                show_race_id = !show_race_id;
                last_display_switch_tick = now;
                last_displayed_value = UINT32_MAX;
            }

            // Refresh only when the value on the currently selected screen changes.
            if (show_race_id) {
                const uint32_t value = latest_packet.round * 100U + latest_packet.race;
                if (value != last_displayed_value) {
                    char display_text[7];
                    format_race_id(latest_packet.round, latest_packet.race,
                                   display_text);
                    draw_text(display, display_text);
                    last_displayed_value = value;
                }
            } else {
                if (latest_packet.elapsed_seconds != last_displayed_value) {
                    char display_text[6];
                    format_elapsed_time(latest_packet.elapsed_seconds,
                                        display_text);
                    draw_text(display, display_text);
                    last_displayed_value = latest_packet.elapsed_seconds;
                }
            }
        }
    }

}
