#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "pax_codecs.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

// Constants
static char const TAG[] = "main";
#define SD_MOUNT_POINT "/sd"
#define MAX_PNG_FILES 100
#define MAX_PATH_LENGTH 512
#define IMAGE_WIDTH 800
#define IMAGE_HEIGHT 480
#define IMAGE_BYTES_PER_PIXEL 3  // PAX_BUF_24_888RGB

#define COLOR_BLACK 0xFF000000
#define COLOR_WHITE 0xFFFFFFFF

// Global variables
static size_t         display_h_res = 0;
static size_t         display_v_res = 0;
static pax_buf_t      fb            = {0};
static QueueHandle_t  input_event_queue = NULL;

// Image navigation variables
static char png_files[MAX_PNG_FILES][MAX_PATH_LENGTH];
static int  png_count            = 0;
static int  current_image_index  = 0;
static bool sd_card_available    = false;
static pax_buf_t current_image   = {0};
static bool has_current_image    = false;
static bool image_flipped        = false;

// Function to check if a filename ends with .png
static bool is_png_file(const char* filename) {
    size_t len = strlen(filename);
    if (len < 4) return false;
    return (strcasecmp(filename + len - 4, ".png") == 0);
}

// Function to scan directory for PNG files and populate the global list
static int scan_png_files(void) {
    DIR* dir = opendir(SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory %s", SD_MOUNT_POINT);
        return 0;
    }

    png_count = 0;
    int total_files = 0;
    struct dirent* entry;

    ESP_LOGI(TAG, "Scanning directory %s for files:", SD_MOUNT_POINT);

    while ((entry = readdir(dir)) != NULL && png_count < MAX_PNG_FILES) {
        if (entry->d_type == DT_REG) {
            total_files++;
            ESP_LOGI(TAG, "Found file: %s", entry->d_name);

            if (is_png_file(entry->d_name)) {
                size_t path_len = strlen(SD_MOUNT_POINT) + strlen(entry->d_name) + 2;  // +2 for '/' and '\0'
                if (path_len < MAX_PATH_LENGTH) {
                    snprintf(png_files[png_count], sizeof(png_files[png_count]),
                             "%s/%s", SD_MOUNT_POINT, entry->d_name);
                    png_count++;
                    ESP_LOGI(TAG, "  -> This is a PNG file!");
                } else {
                    ESP_LOGW(TAG, "Path too long for PNG file: %s", entry->d_name);
                }
            }
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "Scan complete. Found %d total files, %d PNG files", total_files, png_count);
    for (int i = 0; i < png_count; i++) {
        ESP_LOGI(TAG, "PNG file %d: %s", i, png_files[i]);
    }

    if (png_count == 0) {
        if (total_files == 0) {
            ESP_LOGW(TAG, "No files found in %s", SD_MOUNT_POINT);
        } else {
            ESP_LOGW(TAG, "No PNG files found in %s (found %d other files)", SD_MOUNT_POINT, total_files);
        }
    }

    return png_count;
}

// Blit the framebuffer to the physical display
static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %d", res);
    }
}

// Reverse a decoded image's pixels in place, 180 degrees.
// Operates directly on the raw pixel bytes rather than a PAX rotation
// matrix/shader, so there is no interpolation/transform edge case that can
// leave stray pixels at the buffer boundary.
static void flip_image_pixels_180(pax_buf_t* buf) {
    int      w          = pax_buf_get_width(buf);
    int      h          = pax_buf_get_height(buf);
    int      row_bytes  = w * IMAGE_BYTES_PER_PIXEL;
    uint8_t* pixels     = (uint8_t*)pax_buf_get_pixels_rw(buf);

    uint8_t* tmp_row = malloc(row_bytes);
    if (tmp_row == NULL) {
        ESP_LOGE(TAG, "Failed to allocate row buffer for flip");
        return;
    }

    // Swap row i with row (h-1-i)
    for (int y = 0; y < h / 2; y++) {
        uint8_t* top_row    = pixels + (size_t)y * row_bytes;
        uint8_t* bottom_row = pixels + (size_t)(h - 1 - y) * row_bytes;
        memcpy(tmp_row, top_row, row_bytes);
        memcpy(top_row, bottom_row, row_bytes);
        memcpy(bottom_row, tmp_row, row_bytes);
    }
    free(tmp_row);

    // Reverse pixel order within every row
    for (int y = 0; y < h; y++) {
        uint8_t* row = pixels + (size_t)y * row_bytes;
        for (int x = 0; x < w / 2; x++) {
            uint8_t* left  = row + (size_t)x * IMAGE_BYTES_PER_PIXEL;
            uint8_t* right = row + (size_t)(w - 1 - x) * IMAGE_BYTES_PER_PIXEL;
            uint8_t  tmp[IMAGE_BYTES_PER_PIXEL];
            memcpy(tmp, left, IMAGE_BYTES_PER_PIXEL);
            memcpy(left, right, IMAGE_BYTES_PER_PIXEL);
            memcpy(right, tmp, IMAGE_BYTES_PER_PIXEL);
        }
    }
}

static void draw_message(const char* line1, const char* line2, const char* line3) {
    pax_background(&fb, COLOR_BLACK);
    int y = 200;
    if (line1) {
        pax_draw_text(&fb, COLOR_WHITE, pax_font_sky_mono, 28, 20, y, line1);
        y += 34;
    }
    if (line2) {
        pax_draw_text(&fb, COLOR_WHITE, pax_font_sky_mono, 28, 20, y, line2);
        y += 34;
    }
    if (line3) {
        pax_draw_text(&fb, COLOR_WHITE, pax_font_sky_mono, 28, 20, y, line3);
    }
    blit();
}

// Redraw the current frame: the loaded image, or a fallback message
static void render_frame(void) {
    if (has_current_image) {
        pax_background(&fb, COLOR_BLACK);
        pax_draw_image_op(&fb, &current_image, 0, 0);
        blit();
    } else if (!sd_card_available) {
        draw_message("SD Card Error", "Check GPIO pins", "and reboot device");
    } else {
        draw_message("No PNG files found", "Check console for", "file listing");
    }
}

static void free_current_image(void) {
    if (has_current_image) {
        pax_buf_destroy(&current_image);
        has_current_image = false;
    }
}

// Decode and display the PNG at the given index. Returns false (and leaves
// the previously displayed image, if any, untouched) on any failure.
static bool load_image(int index) {
    if (!sd_card_available) {
        ESP_LOGE(TAG, "SD card not available");
        return false;
    }
    if (png_count == 0) {
        ESP_LOGE(TAG, "No PNG files available");
        return false;
    }
    if (index < 0 || index >= png_count) {
        ESP_LOGE(TAG, "Invalid image index: %d (total: %d)", index, png_count);
        return false;
    }

    ESP_LOGI(TAG, "Loading image %d: %s", index, png_files[index]);

    FILE* fd = fopen(png_files[index], "rb");
    if (fd == NULL) {
        ESP_LOGE(TAG, "Failed to open %s", png_files[index]);
        return false;
    }

    pax_png_info_t info;
    if (!pax_info_png_fd(&info, fd)) {
        ESP_LOGE(TAG, "Failed to read PNG info for %s", png_files[index]);
        fclose(fd);
        return false;
    }
    if (info.width != IMAGE_WIDTH || info.height != IMAGE_HEIGHT) {
        ESP_LOGW(TAG, "Skipping %s: expected %dx%d, got %" PRIu32 "x%" PRIu32,
                 png_files[index], IMAGE_WIDTH, IMAGE_HEIGHT, info.width, info.height);
        fclose(fd);
        return false;
    }

    rewind(fd);
    pax_buf_t new_image;
    bool      decoded = pax_decode_png_fd(&new_image, fd, PAX_BUF_24_888RGB, CODEC_FLAG_STRICT);
    fclose(fd);

    if (!decoded) {
        ESP_LOGE(TAG, "Failed to decode %s", png_files[index]);
        return false;
    }
    if (pax_buf_get_type(&new_image) != PAX_BUF_24_888RGB) {
        ESP_LOGE(TAG, "Decoded %s in unexpected pixel format, skipping", png_files[index]);
        pax_buf_destroy(&new_image);
        return false;
    }

    if (image_flipped) {
        flip_image_pixels_180(&new_image);
    }

    free_current_image();
    current_image      = new_image;
    has_current_image  = true;
    current_image_index = index;

    render_frame();

    ESP_LOGI(TAG, "Image loaded successfully: %s", png_files[index]);
    return true;
}

static void next_image(void) {
    if (png_count == 0) return;
    int next_index = (current_image_index + 1) % png_count;
    load_image(next_index);
    ESP_LOGI(TAG, "Switched to next image: %d/%d", next_index + 1, png_count);
}

static void previous_image(void) {
    if (png_count == 0) return;
    int prev_index = (current_image_index - 1 + png_count) % png_count;
    load_image(prev_index);
    ESP_LOGI(TAG, "Switched to previous image: %d/%d", prev_index + 1, png_count);
}

static void random_image(void) {
    if (png_count == 0) return;
    int random_index = rand() % png_count;
    load_image(random_index);
    ESP_LOGI(TAG, "Switched to random image: %d/%d", random_index + 1, png_count);
}

static void flip_image(void) {
    image_flipped = !image_flipped;
    if (has_current_image) {
        flip_image_pixels_180(&current_image);
        render_frame();
    }
    ESP_LOGI(TAG, "Image flip is now %s", image_flipped ? "on" : "off");
}

// Function to handle input events
static void handle_input_event(bsp_input_event_t* event) {
    ESP_LOGI(TAG, "Input event received: type=%d", event->type);

    switch (event->type) {
        case INPUT_EVENT_TYPE_NAVIGATION:
            ESP_LOGI(TAG, "Navigation event: key=%d, state=%d", event->args_navigation.key,
                     event->args_navigation.state);
            if (event->args_navigation.state) {  // Key press (not release)
                switch (event->args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_LEFT:
                        ESP_LOGI(TAG, "Left arrow pressed");
                        previous_image();
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                        ESP_LOGI(TAG, "Right arrow pressed");
                        next_image();
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_ESC:
                    case BSP_INPUT_NAVIGATION_KEY_F1:
                        ESP_LOGI(TAG, "Exit key pressed - returning to launcher");
                        bsp_device_restart_to_launcher();
                        break;
                    default:
                        ESP_LOGI(TAG, "Other navigation key: %d", event->args_navigation.key);
                        break;
                }
            }
            break;

        case INPUT_EVENT_TYPE_KEYBOARD:
            ESP_LOGI(TAG, "Keyboard event: ascii='%c' (0x%02x)", event->args_keyboard.ascii,
                     event->args_keyboard.ascii);
            if (event->args_keyboard.ascii == 'r' || event->args_keyboard.ascii == 'R') {
                ESP_LOGI(TAG, "R key pressed - random image");
                random_image();
            } else if (event->args_keyboard.ascii == 'f' || event->args_keyboard.ascii == 'F') {
                ESP_LOGI(TAG, "F key pressed - flip image");
                flip_image();
            } else if (event->args_keyboard.ascii == 'x' || event->args_keyboard.ascii == 'X') {
                ESP_LOGI(TAG, "X key pressed - returning to launcher");
                bsp_device_restart_to_launcher();
            }
            break;

        case INPUT_EVENT_TYPE_SCANCODE:
            ESP_LOGI(TAG, "Scancode event: scancode=0x%04x", event->args_scancode.scancode);
            if (event->args_scancode.scancode == BSP_INPUT_SCANCODE_R) {
                ESP_LOGI(TAG, "R scancode - random image");
                random_image();
            } else if (event->args_scancode.scancode == BSP_INPUT_SCANCODE_F) {
                ESP_LOGI(TAG, "F scancode - flip image");
                flip_image();
            }
            break;

        default:
            ESP_LOGI(TAG, "Unknown input event type: %d", event->type);
            break;
    }
}

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage service
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_configuration));

    // Set log level to maximum verbosity to see all debug messages
    esp_log_level_set("*", ESP_LOG_VERBOSE);

    srand(time(NULL));

    // Mount SD card with proper GPIO configuration for Tanmatsu
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024
    };

    sdmmc_card_t* card;
    const char    mount_point[] = SD_MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // Use the same GPIO configuration as Tanmatsu launcher
    sdmmc_slot_config_t slot_config = {
        .clk   = GPIO_NUM_43,
        .cmd   = GPIO_NUM_44,
        .d0    = GPIO_NUM_39,
        .d1    = GPIO_NUM_40,
        .d2    = GPIO_NUM_41,
        .d3    = GPIO_NUM_42,
        .d4    = GPIO_NUM_NC,
        .d5    = GPIO_NUM_NC,
        .d6    = GPIO_NUM_NC,
        .d7    = GPIO_NUM_NC,
        .cd    = SDMMC_SLOT_NO_CD,
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    esp_err_t sd_ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    sd_card_available = false;

    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SDMMC mount failed (%s), trying SPI mode...", esp_err_to_name(sd_ret));

        // Try SPI mode as fallback (like Tanmatsu launcher)
        sdmmc_host_t spi_host = SDSPI_HOST_DEFAULT();

        spi_bus_config_t bus_cfg = {
            .mosi_io_num     = GPIO_NUM_44,
            .miso_io_num     = GPIO_NUM_39,
            .sclk_io_num     = GPIO_NUM_43,
            .quadwp_io_num   = -1,
            .quadhd_io_num   = -1,
            .max_transfer_sz = 4000,
        };

        esp_err_t spi_init_ret = spi_bus_initialize(spi_host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (spi_init_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPI bus (%s)", esp_err_to_name(spi_init_ret));
        } else {
            sdspi_device_config_t spi_slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
            spi_slot_config.gpio_cs = GPIO_NUM_42;
            spi_slot_config.host_id = spi_host.slot;

            ESP_LOGI(TAG, "Trying to mount SD card via SPI");
            sd_ret = esp_vfs_fat_sdspi_mount(mount_point, &spi_host, &spi_slot_config, &mount_config, &card);
        }
    }

    if (sd_ret != ESP_OK) {
        if (sd_ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SD card filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the SD card (%s). Make sure SD card is inserted.",
                     esp_err_to_name(sd_ret));
        }
        ESP_LOGW(TAG, "Continuing without SD card - will show fallback message");
        sd_card_available = false;
    } else {
        ESP_LOGI(TAG, "SD card mounted successfully");
        sdmmc_card_print_info(stdout, card);
        sd_card_available = true;
    }

    // Get display parameters and rotation
    size_t                       h_res  = 0;
    size_t                       v_res  = 0;
    bsp_display_color_format_t   color_format = 0;
    bsp_display_endianness_t     data_endian  = 0;
    res = bsp_display_get_parameters(&h_res, &v_res, &color_format, &data_endian);
    ESP_ERROR_CHECK(res);
    display_h_res = h_res;
    display_v_res = v_res;

    // Convert ESP-IDF color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (color_format) {
        case BSP_DISPLAY_COLOR_FORMAT_1_PAL:      format = PAX_BUF_1_PAL;      break;
        case BSP_DISPLAY_COLOR_FORMAT_2_PAL:      format = PAX_BUF_2_PAL;      break;
        case BSP_DISPLAY_COLOR_FORMAT_4_PAL:      format = PAX_BUF_4_PAL;      break;
        case BSP_DISPLAY_COLOR_FORMAT_8_PAL:      format = PAX_BUF_8_PAL;      break;
        case BSP_DISPLAY_COLOR_FORMAT_16_PAL:     format = PAX_BUF_16_PAL;     break;
        case BSP_DISPLAY_COLOR_FORMAT_1_GREY:     format = PAX_BUF_1_GREY;     break;
        case BSP_DISPLAY_COLOR_FORMAT_2_GREY:     format = PAX_BUF_2_GREY;     break;
        case BSP_DISPLAY_COLOR_FORMAT_4_GREY:     format = PAX_BUF_4_GREY;     break;
        case BSP_DISPLAY_COLOR_FORMAT_8_GREY:     format = PAX_BUF_8_GREY;     break;
        case BSP_DISPLAY_COLOR_FORMAT_8_332RGB:   format = PAX_BUF_8_332RGB;   break;
        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:  format = PAX_BUF_16_565RGB;  break;
        case BSP_DISPLAY_COLOR_FORMAT_4_1111ARGB: format = PAX_BUF_4_1111ARGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_8_2222ARGB: format = PAX_BUF_8_2222ARGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_16_4444ARGB:format = PAX_BUF_16_4444ARGB;break;
        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB:  format = PAX_BUF_24_888RGB;  break;
        case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB:format = PAX_BUF_32_8888ARGB;break;
        default:
            ESP_LOGW(TAG, "BSP requests color format not supported by PAX (%u), defaulting to 24_888RGB",
                      color_format);
            break;
    }

    if (format != PAX_BUF_24_888RGB) {
        ESP_LOGE(TAG, "Display did not provide the requested 24_888RGB format (got %d) - image drawing assumes "
                       "24-bit RGB and will not work correctly", format);
    }

    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t      orientation      = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CCW;  break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW;   break;
        case BSP_DISPLAY_ROTATION_0:
        default:                       orientation = PAX_O_UPRIGHT;  break;
    }

    // Initialize graphics stack
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, data_endian == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    ESP_LOGW(TAG, "Hello world!");

    // Scan for PNG files on SD card if available
    if (sd_card_available) {
        ESP_LOGI(TAG, "SD card is available, scanning for PNG files...");
        int found_files = scan_png_files();
        ESP_LOGI(TAG, "Found %d PNG files", found_files);
    } else {
        ESP_LOGW(TAG, "SD card not available, skipping PNG file scanning");
    }

    if (png_count > 0) {
        ESP_LOGI(TAG, "PNG files found, attempting to load first image...");
        current_image_index = 0;
        load_image(current_image_index);
        ESP_LOGI(TAG, "Attempted to load first image: %d/%d", current_image_index + 1, png_count);
    } else {
        render_frame();
    }

    ESP_LOGI(TAG, "Starting main event loop");
    ESP_LOGI(TAG, "Controls: Left/Right arrows = navigate, R = random image, F = flip image");

    // Main event loop
    bsp_input_event_t input_event;
    while (true) {
        if (xQueueReceive(input_event_queue, &input_event, pdMS_TO_TICKS(100)) == pdTRUE) {
            handle_input_event(&input_event);
        }
    }
}
