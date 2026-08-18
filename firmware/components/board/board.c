#include "board/board.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "desk_board";

enum {
    I2C_MASTER_PORT = 0,
    I2C_MASTER_SDA = 8,
    I2C_MASTER_SCL = 9,
    I2C_MASTER_FREQUENCY_HZ = 400000,
    I2C_MASTER_TIMEOUT_MS = 1000,
    TOUCH_INTERRUPT_GPIO = 4,
    CH422G_MODE_ADDRESS = 0x24,
    CH422G_OUTPUT_ADDRESS = 0x38,
};

enum {
    CH422G_TP_RESET = 1U << 1,
    CH422G_LCD_BACKLIGHT = 1U << 2,
    CH422G_LCD_RESET = 1U << 3,
    CH422G_SD_CHIP_SELECT = 1U << 4,
    CH422G_USB_SELECT = 1U << 5,
};

/* Values below reproduce the Waveshare 4.3 and 4.3B reference sequence. */
#define CH422G_TOUCH_RESET_ASSERT \
    (CH422G_LCD_BACKLIGHT | CH422G_LCD_RESET | CH422G_USB_SELECT)
#define CH422G_TOUCH_RESET_RELEASE (CH422G_TOUCH_RESET_ASSERT | CH422G_TP_RESET)
#define CH422G_BACKLIGHT_ON \
    (CH422G_TP_RESET | CH422G_LCD_BACKLIGHT | CH422G_LCD_RESET | CH422G_SD_CHIP_SELECT)
#define CH422G_BACKLIGHT_OFF \
    (CH422G_TP_RESET | CH422G_LCD_RESET | CH422G_SD_CHIP_SELECT)

/*
 * esp_lcd_new_panel_io_i2c() uses _Generic to select its legacy-v1 path when
 * the bus argument is the numeric I2C port type used below. A future migration
 * to i2c_master_bus_handle_t must update both the driver and panel-IO call.
 */
_Static_assert(
    _Generic((esp_lcd_i2c_bus_handle_t)0, uint32_t: 1, default: 0),
    "Legacy esp_lcd I2C v1 routing changed; migrate board I2C calls together"
);

#define LCD_PIXEL_CLOCK_HZ (16 * 1000 * 1000)
#define LCD_DATA_WIDTH 16
#define LCD_BITS_PER_PIXEL 16
#define LCD_BOUNCE_LINES 10

#if CONFIG_DESK_BOARD_4_3_B
static const desk_board_profile_t BOARD_PROFILE = {
    .id = DESK_BOARD_PROFILE_4_3_B,
    .display_name = "Waveshare ESP32-S3-Touch-LCD-4.3B",
    .hardware_verified = false,
};
#else
static const desk_board_profile_t BOARD_PROFILE = {
    .id = DESK_BOARD_PROFILE_4_3,
    .display_name = "Waveshare ESP32-S3-Touch-LCD-4.3",
    .hardware_verified = false,
};
#endif

/* CH422G's output register is write-only and shared by the display, touch,
 * microSD and USB routing. All steady-state changes go through this shadow so
 * one peripheral cannot overwrite another peripheral's output bit. */
static uint8_t ch422g_output_shadow = CH422G_BACKLIGHT_OFF;
static SemaphoreHandle_t ch422g_output_mutex;

static esp_err_t i2c_master_init_once(void)
{
    static bool initialized = false;
    if (initialized) {
        return ESP_OK;
    }

    if (ch422g_output_mutex == NULL) {
        ch422g_output_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(ch422g_output_mutex != NULL, ESP_ERR_NO_MEM, TAG, "CH422G mutex allocation failed");
    }

    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA,
        .scl_io_num = I2C_MASTER_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQUENCY_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_MASTER_PORT, &config), TAG, "I2C configuration failed");
    ESP_RETURN_ON_ERROR(
        i2c_driver_install(I2C_MASTER_PORT, config.mode, 0, 0, 0),
        TAG,
        "I2C driver installation failed"
    );
    initialized = true;
    return ESP_OK;
}

static esp_err_t ch422g_write(uint8_t address, uint8_t value)
{
    return i2c_master_write_to_device(
        I2C_MASTER_PORT,
        address,
        &value,
        sizeof(value),
        pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS)
    );
}

static esp_err_t ch422g_output_write(uint8_t value)
{
    const esp_err_t result = ch422g_write(CH422G_OUTPUT_ADDRESS, value);
    if (result == ESP_OK) {
        ch422g_output_shadow = value;
    }
    return result;
}

static esp_err_t ch422g_output_update(uint8_t set_mask, uint8_t clear_mask)
{
    ESP_RETURN_ON_ERROR(i2c_master_init_once(), TAG, "I2C initialization failed");
    ESP_RETURN_ON_ERROR(ch422g_write(CH422G_MODE_ADDRESS, 0x01), TAG, "CH422G mode configuration failed");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(ch422g_output_mutex, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS)) == pdTRUE,
        ESP_ERR_TIMEOUT,
        TAG,
        "CH422G output lock timed out"
    );

    const uint8_t next = (uint8_t)((ch422g_output_shadow | set_mask) & (uint8_t)~clear_mask);
    const esp_err_t result = ch422g_output_write(next);
    xSemaphoreGive(ch422g_output_mutex);
    return result;
}

static esp_err_t touch_reset(void)
{
    /* GPIO4 selects the GT911 I2C address during reset, then remains output-low.
     * Touch therefore runs in polling mode with int_gpio_num = -1 below. */
    const gpio_config_t touch_gpio_config = {
        .pin_bit_mask = 1ULL << TOUCH_INTERRUPT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&touch_gpio_config), TAG, "Touch interrupt GPIO configuration failed");

    ESP_RETURN_ON_ERROR(ch422g_write(CH422G_MODE_ADDRESS, 0x01), TAG, "CH422G mode configuration failed");
    ESP_RETURN_ON_ERROR(
        ch422g_output_write(CH422G_TOUCH_RESET_ASSERT),
        TAG,
        "Touch reset start failed"
    );
    esp_rom_delay_us(100 * 1000);
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_INTERRUPT_GPIO, 0), TAG, "Touch address selection failed");
    esp_rom_delay_us(100 * 1000);
    ESP_RETURN_ON_ERROR(
        ch422g_output_write(CH422G_TOUCH_RESET_RELEASE),
        TAG,
        "Touch reset release failed"
    );
    esp_rom_delay_us(200 * 1000);
    return ESP_OK;
}

const desk_board_profile_t *desk_board_get_profile(void)
{
    return &BOARD_PROFILE;
}

esp_err_t desk_board_display_init(
    uint8_t frame_buffer_count,
    esp_lcd_panel_handle_t *panel_handle,
    esp_lcd_touch_handle_t *touch_handle
)
{
    ESP_RETURN_ON_FALSE(panel_handle != NULL && touch_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid output handle");

    *panel_handle = NULL;
    *touch_handle = NULL;

    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = DESK_LCD_H_RES,
            .v_res = DESK_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = LCD_DATA_WIDTH,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .num_fbs = frame_buffer_count,
        .bounce_buffer_size_px = DESK_LCD_H_RES * LCD_BOUNCE_LINES,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = GPIO_NUM_46,
        .vsync_gpio_num = GPIO_NUM_3,
        .de_gpio_num = GPIO_NUM_5,
        .pclk_gpio_num = GPIO_NUM_7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            GPIO_NUM_14,
            GPIO_NUM_38,
            GPIO_NUM_18,
            GPIO_NUM_17,
            GPIO_NUM_10,
            GPIO_NUM_39,
            GPIO_NUM_0,
            GPIO_NUM_45,
            GPIO_NUM_48,
            GPIO_NUM_47,
            GPIO_NUM_21,
            GPIO_NUM_1,
            GPIO_NUM_2,
            GPIO_NUM_42,
            GPIO_NUM_41,
            GPIO_NUM_40,
        },
        .flags.fb_in_psram = 1,
    };

    ESP_LOGI(TAG, "Initializing 800x480 RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, panel_handle), TAG, "RGB panel creation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel_handle), TAG, "RGB panel initialization failed");

    ESP_RETURN_ON_ERROR(i2c_master_init_once(), TAG, "I2C initialization failed");
    ESP_RETURN_ON_ERROR(touch_reset(), TAG, "Touch reset failed");

    esp_lcd_panel_io_handle_t touch_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    /* Required by ESP-IDF's legacy-v1 panel I2C path: the installed port owns
     * the clock, and any non-zero value is rejected with ESP_ERR_INVALID_ARG. */
    touch_io_config.scl_speed_hz = 0;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(
            (esp_lcd_i2c_bus_handle_t)I2C_MASTER_PORT,
            &touch_io_config,
            &touch_io_handle
        ),
        TAG,
        "GT911 panel IO creation failed"
    );

    const esp_lcd_touch_config_t touch_config = {
        .x_max = DESK_LCD_H_RES,
        .y_max = DESK_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(touch_io_handle, &touch_config, touch_handle),
        TAG,
        "GT911 initialization failed"
    );
    /* Establish the original steady-state value before independent backlight
     * and SD updates begin. The app decides when the backlight turns on. */
    ESP_RETURN_ON_ERROR(
        ch422g_output_write(CH422G_BACKLIGHT_OFF),
        TAG,
        "CH422G steady-state configuration failed"
    );
    return ESP_OK;
}

esp_err_t desk_board_backlight_set(bool enabled)
{
    return enabled
               ? ch422g_output_update(CH422G_LCD_BACKLIGHT, 0)
               : ch422g_output_update(0, CH422G_LCD_BACKLIGHT);
}

esp_err_t desk_board_sd_select(bool selected)
{
    /* The expander output directly drives active-low SD_CS. */
    return selected
               ? ch422g_output_update(0, CH422G_SD_CHIP_SELECT)
               : ch422g_output_update(CH422G_SD_CHIP_SELECT, 0);
}
