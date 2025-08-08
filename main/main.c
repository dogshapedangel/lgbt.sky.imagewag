#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp_lvgl.h"
#include "core/lv_obj.h"
#include "display/lv_display.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "font/lv_font.h"
#include "hal/lcd_types.h"
#include "layouts/flex/lv_flex.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "widgets/label/lv_label.h"
#include "widgets/textarea/lv_textarea.h"
#include "widgets/image/lv_image.h"
#include "misc/cache/lv_image_cache.h"

// Constants
static char const TAG[] = "main";
#define SD_MOUNT_POINT "/sd"
#define MAX_PNG_FILES 100
#define MAX_PATH_LENGTH 512

// Global variables
static esp_lcd_panel_handle_t       display_lcd_panel    = NULL;
static esp_lcd_panel_io_handle_t    display_lcd_panel_io = NULL;
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format;
static QueueHandle_t                input_event_queue = NULL;

// Function to check if a filename ends with .png
bool is_png_file(const char* filename) {
    size_t len = strlen(filename);
    if (len < 4) return false;
    return (strcasecmp(filename + len - 4, ".png") == 0);
}

// Function to scan directory for PNG files and return a random one
char* get_random_png_file(void) {
    DIR* dir = opendir(SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory %s", SD_MOUNT_POINT);
        return NULL;
    }

    static char png_files[MAX_PNG_FILES][MAX_PATH_LENGTH];
    int png_count = 0;
    int total_files = 0;
    struct dirent* entry;

    ESP_LOGI(TAG, "Scanning directory %s for files:", SD_MOUNT_POINT);

    // Scan directory for PNG files
    while ((entry = readdir(dir)) != NULL && png_count < MAX_PNG_FILES) {
        if (entry->d_type == DT_REG) {
            total_files++;
            ESP_LOGI(TAG, "Found file: %s", entry->d_name);
            
            if (is_png_file(entry->d_name)) {
                // Check if the full path will fit in the buffer
                size_t path_len = strlen(SD_MOUNT_POINT) + strlen(entry->d_name) + 2; // +2 for '/' and '\0'
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
    
    // Log all found PNG files for debugging
    for (int i = 0; i < png_count; i++) {
        ESP_LOGI(TAG, "PNG file %d: %s", i, png_files[i]);
    }

    if (png_count == 0) {
        if (total_files == 0) {
            ESP_LOGW(TAG, "No files found in %s", SD_MOUNT_POINT);
        } else {
            ESP_LOGW(TAG, "No PNG files found in %s (found %d other files)", SD_MOUNT_POINT, total_files);
        }
        return NULL;
    }

    // Select a random PNG file
    srand(time(NULL));
    int random_index = rand() % png_count;
    ESP_LOGI(TAG, "Selected random PNG file: %s", png_files[random_index]);
    
    // Return a pointer to the selected filename
    static char selected_file[MAX_PATH_LENGTH];
    strcpy(selected_file, png_files[random_index]);
    return selected_file;
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
    ESP_ERROR_CHECK(bsp_device_initialize());

    // Set log level to maximum verbosity to see all debug messages
    esp_log_level_set("*", ESP_LOG_VERBOSE);

    // Mount SD card with proper GPIO configuration for Tanmatsu
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    sdmmc_card_t* card;
    const char mount_point[] = SD_MOUNT_POINT;
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
    bool sd_card_available = false;
    
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
            ESP_LOGE(TAG, "Failed to initialize the SD card (%s). Make sure SD card is inserted.", esp_err_to_name(sd_ret));
        }
        ESP_LOGW(TAG, "Continuing without SD card - will show fallback message");
        sd_card_available = false;
    } else {
        ESP_LOGI(TAG, "SD card mounted successfully");
        sdmmc_card_print_info(stdout, card);
        sd_card_available = true;
    }

    // Fetch the handle for using the screen, this works even when
    res = bsp_display_get_panel(&display_lcd_panel);
    ESP_ERROR_CHECK(res);                             // Check that the display handle has been initialized
    bsp_display_get_panel_io(&display_lcd_panel_io);  // Do not check result of panel IO handle: not all types of
                                                      // display expose a panel IO handle
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format);
    ESP_ERROR_CHECK(res);  // Check that the display parameters have been initialized

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Initialise LVGL
    lvgl_init(display_h_res, display_v_res, display_color_format, display_lcd_panel, display_lcd_panel_io,
              input_event_queue);

    // Increase LVGL image cache size to handle larger images (2MB)
    lv_image_cache_resize(2 * 1024 * 1024, true);
    ESP_LOGI(TAG, "LVGL image cache resized to 2MB");

    ESP_LOGW(TAG, "Hello world!");

    // Get a random PNG file from SD card only if SD card is available
    char* random_png = NULL;
    if (sd_card_available) {
        random_png = get_random_png_file();
    } else {
        ESP_LOGW(TAG, "SD card not available, skipping PNG file selection");
    }

    lvgl_lock();

    lv_obj_t* screen = lv_screen_active();
    
    // Set a purple background color to verify screen is working
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x8B00FF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    
    // Remove flex layout to allow proper manual positioning of the image

    if (random_png != NULL) {
        ESP_LOGI(TAG, "Attempting to display image: %s", random_png);
        
        // Create and display the image - remove test rectangle since images work
        lv_obj_t* img = lv_image_create(screen);
        
        // Set the image source first
        lv_image_set_src(img, random_png);
        
        // Use full size now that cache is increased to 2MB
        lv_obj_set_size(img, 800, 480);
        
        ESP_LOGI(TAG, "Image widget created and source set: %s", random_png);
        
        // Center the image perfectly on screen
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    } else {
        // Fallback: show a label if no PNG files found or SD card not available
        lv_obj_t* label = lv_label_create(screen);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_42, LV_STATE_DEFAULT);
        
        if (!sd_card_available) {
            lv_label_set_text(label, "SD Card Error\nCheck GPIO pins\nand reboot device");
            ESP_LOGW(TAG, "SD card not available, showing error message");
        } else {
            lv_label_set_text(label, "No PNG files found\nCheck console for\nfile listing");
            ESP_LOGW(TAG, "No PNG files found, showing fallback message. Check logs for file listing.");
        }
    }

    lvgl_unlock();    // You can add your main loop here
}
