#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_hid_gap.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define TAG "MEDIA_REMOTE"

#define BUTTON_VOLUME_UP GPIO_NUM_1
#define BUTTON_VOLUME_DOWN GPIO_NUM_0
#define BUTTON_NEXT GPIO_NUM_3
#define BUTTON_PREVIOUS GPIO_NUM_2
#define BUTTON_PLAY_PAUSE GPIO_NUM_4

#define OLED_ADDRESS 0x3C
#define OLED_SDA GPIO_NUM_8
#define OLED_SCL GPIO_NUM_9
#define OLED_I2C_PORT I2C_NUM_0

#define HID_REPORT_ID 3
#define HID_REPORT_LENGTH 2

#define HID_REPORT_VOLUME_UP 0x0040
#define HID_REPORT_VOLUME_DOWN 0x0080
#define HID_REPORT_SCAN_NEXT 0x0A00
#define HID_REPORT_SCAN_PREVIOUS 0x0B00
#define HID_REPORT_PLAY 0x0500

static esp_hidd_dev_t *hid_device;
static bool hid_connected;

static const uint8_t font[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

static void oled_command(uint8_t command)
{
    uint8_t packet[] = {0x00, command};
    i2c_master_write_to_device(OLED_I2C_PORT, OLED_ADDRESS, packet, sizeof(packet), pdMS_TO_TICKS(100));
}

static void oled_bytes(const uint8_t *data, size_t length)
{
    uint8_t packet[17];
    packet[0] = 0x40;
    while (length > 0) {
        size_t chunk = length > 16 ? 16 : length;
        memcpy(&packet[1], data, chunk);
        i2c_master_write_to_device(OLED_I2C_PORT, OLED_ADDRESS, packet, chunk + 1, pdMS_TO_TICKS(100));
        data += chunk;
        length -= chunk;
    }
}

static void oled_init(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = OLED_SDA,
        .scl_io_num = OLED_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(OLED_I2C_PORT, &config));
    ESP_ERROR_CHECK(i2c_driver_install(OLED_I2C_PORT, config.mode, 0, 0, 0));

    const uint8_t commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    };
    for (size_t i = 0; i < sizeof(commands); ++i) {
        oled_command(commands[i]);
    }
}

static void oled_show_action(const char *action)
{
    static uint8_t framebuffer[8][128];
    memset(framebuffer, 0, sizeof(framebuffer));

    size_t x = 4;
    for (const char *character = action; *character != '\0' && x < 128; ++character) {
        if (*character >= 'A' && *character <= 'Z') {
            const uint8_t *glyph = font[*character - 'A'];
            for (size_t column = 0; column < 5; ++column) {
                for (size_t row = 0; row < 7; ++row) {
                    if ((glyph[column] & (1U << row)) == 0) {
                        continue;
                    }
                    for (size_t scale_y = 0; scale_y < 2; ++scale_y) {
                        const size_t y = 20 + row * 2 + scale_y;
                        for (size_t scale_x = 0; scale_x < 2; ++scale_x) {
                            const size_t pixel_x = x + column * 2 + scale_x;
                            if (pixel_x < 128) {
                                framebuffer[y / 8][pixel_x] |= 1U << (y % 8);
                            }
                        }
                    }
                }
            }
        }
        x += 11;
    }

    for (uint8_t page = 0; page < 8; ++page) {
        oled_command(0xB0 | page);
        oled_command(0x00);
        oled_command(0x10);
        oled_bytes(framebuffer[page], sizeof(framebuffer[page]));
    }
}

void ble_hid_task_start_up(void)
{
}

static const uint8_t media_report_map[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,
    0x09, 0x02, 0xA1, 0x02, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x01, 0x25, 0x0A, 0x75, 0x04,
    0x95, 0x01, 0x81, 0x00, 0xC0, 0x05, 0x0C, 0x09,
    0x86, 0x15, 0xFF, 0x25, 0x01, 0x75, 0x02, 0x95,
    0x01, 0x81, 0x46, 0x09, 0xE9, 0x09, 0xEA, 0x15,
    0x00, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x09,
    0xE2, 0x09, 0x30, 0x09, 0x83, 0x09, 0x81, 0x09,
    0xB0, 0x09, 0xB1, 0x09, 0xB2, 0x09, 0xB3, 0x09,
    0xB4, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7, 0x15,
    0x01, 0x25, 0x0C, 0x75, 0x04, 0x95, 0x01, 0x81,
    0x00, 0x09, 0x80, 0xA1, 0x02, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x03, 0x15, 0x01, 0x25, 0x03, 0x75,
    0x02, 0x81, 0x00, 0xC0, 0x81, 0x03, 0xC0
};

static esp_hid_raw_report_map_t report_maps[] = {
    {.data = media_report_map, .len = sizeof(media_report_map)},
};

static esp_hid_device_config_t hid_config = {
    .vendor_id = 0x303A,
    .product_id = 0x1001,
    .version = 0x0100,
    .device_name = "C3 Media Remote",
    .manufacturer_name = "ESP32-C3",
    .serial_number = "C3-MEDIA-01",
    .report_maps = report_maps,
    .report_maps_len = 1,
};

static void send_media_key(uint16_t usage)
{
    uint8_t report[HID_REPORT_LENGTH] = {
        (uint8_t)(usage & 0xFF),
        (uint8_t)(usage >> 8),
    };
    if (!hid_connected) {
        return;
    }
    esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID, report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(12));
    memset(report, 0, sizeof(report));
    esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID, report, sizeof(report));
}

static void hid_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        hid_connected = true;
        ESP_LOGI(TAG, "BLE HID host connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        hid_connected = false;
        ESP_LOGI(TAG, "BLE HID host disconnected");
        esp_hid_ble_gap_adv_start();
        break;
    default:
        break;
    }
}

static void buttons_task(void *argument)
{
    const gpio_num_t pins[] = {
        BUTTON_VOLUME_UP,
        BUTTON_VOLUME_DOWN,
        BUTTON_NEXT,
        BUTTON_PREVIOUS,
        BUTTON_PLAY_PAUSE,
    };
    const uint16_t reports[] = {
        HID_REPORT_VOLUME_UP,
        HID_REPORT_VOLUME_DOWN,
        HID_REPORT_SCAN_NEXT,
        HID_REPORT_SCAN_PREVIOUS,
        HID_REPORT_PLAY,
    };
    const char *actions[] = {
        "VOLUME UP",
        "VOLUME DOWN",
        "NEXT TRACK",
        "PREVIOUS",
        "PLAY PAUSE",
    };
    int last_state[5] = {1, 1, 1, 1, 1};
    int candidate_state[5] = {1, 1, 1, 1, 1};
    TickType_t changed_at[5] = {0};

    while (true) {
        TickType_t now = xTaskGetTickCount();
        for (size_t i = 0; i < 5; ++i) {
            int reading = gpio_get_level(pins[i]);
            if (reading != candidate_state[i]) {
                candidate_state[i] = reading;
                changed_at[i] = now;
            }
            if (reading != last_state[i] && now - changed_at[i] >= pdMS_TO_TICKS(35)) {
                last_state[i] = reading;
                if (reading == 0) {
                    ESP_LOGI(TAG, "Button %u pressed", (unsigned)i);
                    oled_show_action(actions[i]);
                    send_media_key(reports[i]);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    const gpio_num_t button_pins[] = {
        BUTTON_VOLUME_UP, BUTTON_VOLUME_DOWN, BUTTON_NEXT, BUTTON_PREVIOUS, BUTTON_PLAY_PAUSE,
    };
    for (size_t i = 0; i < 5; ++i) {
        gpio_config_t button_config = {
            .pin_bit_mask = 1ULL << button_pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&button_config));
    }

    oled_init();
    oled_show_action("READY");

    ESP_ERROR_CHECK(esp_hid_gap_init(HID_DEV_MODE));
    ESP_ERROR_CHECK(esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, hid_config.device_name));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler));
    ESP_ERROR_CHECK(esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hid_event_callback, &hid_device));

    xTaskCreate(buttons_task, "buttons", 4096, NULL, 5, NULL);
}
