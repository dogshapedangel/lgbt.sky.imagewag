#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

// Image navigation variables
static char png_files[MAX_PNG_FILES][MAX_PATH_LENGTH];
static int png_count = 0;
static int current_image_index = 0;
static bool sd_card_available = false;
static lv_obj_t* current_image = NULL;
static bool image_flipped = false;

// Function to check if a filename ends with .png
bool is_png_file(const char* filename) {
    size_t len = strlen(filename);
    if (len < 4) return false;
    return (strcasecmp(filename + len - 4, ".png") == 0);
}

// Function to scan directory for PNG files and populate the global list
int scan_png_files(void) {
    DIR* dir = opendir(SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory %s", SD_MOUNT_POINT);
        return 0;
    }

    png_count = 0;
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
    }

    return png_count;
}

// Function to load and display an image
void load_image(int index) {
    if (!sd_card_available) {
        ESP_LOGE(TAG, "SD card not available");
        return;
    }
    
    if (png_count == 0) {
        ESP_LOGE(TAG, "No PNG files available");
        return;
    }
    
    if (index < 0 || index >= png_count) {
        ESP_LOGE(TAG, "Invalid image index: %d (total: %d)", index, png_count);
        return;
    }
    
    ESP_LOGI(TAG, "Loading image %d: %s", index, png_files[index]);
    
    lvgl_lock();
    
    // Clear existing image if it exists
    if (current_image != NULL) {
        ESP_LOGI(TAG, "Deleting previous image");
        lv_obj_delete(current_image);
        current_image = NULL;
    }
    
    // Create new image widget
    lv_obj_t* screen = lv_screen_active();
    if (screen == NULL) {
        ESP_LOGE(TAG, "Failed to get active screen");
        lvgl_unlock();
        return;
    }
    
    current_image = lv_image_create(screen);
    if (current_image == NULL) {
        ESP_LOGE(TAG, "Failed to create image widget");
        lvgl_unlock();
        return;
    }
    
    ESP_LOGI(TAG, "Created image widget, setting source...");
    
    // Set the image source
    lv_image_set_src(current_image, png_files[index]);
    
    // Set full size
    lv_obj_set_size(current_image, 800, 480);
    
    // Center the image
    lv_obj_align(current_image, LV_ALIGN_CENTER, 0, 0);
    
    // Make sure the image is visible
    lv_obj_set_style_bg_opa(current_image, LV_OPA_TRANSP, LV_PART_MAIN);
    
    // Apply flip if needed
    if (image_flipped) {
        lv_image_set_rotation(current_image, 1800); // 180 degrees = 1800 tenths of degrees
        ESP_LOGI(TAG, "Applied flip rotation");
    }
    
    current_image_index = index;
    
    lvgl_unlock();
    
    ESP_LOGI(TAG, "Image loaded successfully: %s", png_files[index]);
}

// Function to go to next image
void next_image(void) {
    if (png_count == 0) return;
    
    int next_index = (current_image_index + 1) % png_count;
    load_image(next_index);
    ESP_LOGI(TAG, "Switched to next image: %d/%d", next_index + 1, png_count);
}

// Function to go to previous image
void previous_image(void) {
    if (png_count == 0) return;
    
    int prev_index = (current_image_index - 1 + png_count) % png_count;
    load_image(prev_index);
    ESP_LOGI(TAG, "Switched to previous image: %d/%d", prev_index + 1, png_count);
}

// Function to go to random image
void random_image(void) {
    if (png_count == 0) return;
    
    srand(time(NULL));
    int random_index = rand() % png_count;
    load_image(random_index);
    ESP_LOGI(TAG, "Switched to random image: %d/%d", random_index + 1, png_count);
}

// Function to flip image upside down
void flip_image(void) {
    if (current_image == NULL) return;
    
    lvgl_lock();
    
    image_flipped = !image_flipped;
    
    if (image_flipped) {
        lv_image_set_rotation(current_image, 1800); // 180 degrees
        ESP_LOGI(TAG, "Image flipped upside down");
    } else {
        lv_image_set_rotation(current_image, 0); // 0 degrees
        ESP_LOGI(TAG, "Image flipped back to normal");
    }
    
    lvgl_unlock();
}

// Function to handle input events
void handle_input_event(bsp_input_event_t* event) {
    ESP_LOGI(TAG, "Input event received: type=%d", event->type);
    
    switch (event->type) {
        case INPUT_EVENT_TYPE_NAVIGATION:
            ESP_LOGI(TAG, "Navigation event: key=%d, state=%d", event->args_navigation.key, event->args_navigation.state);
            if (event->args_navigation.state) { // Key press (not release)
                switch (event->args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_LEFT:
                        ESP_LOGI(TAG, "Left arrow pressed");
                        previous_image();
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                        ESP_LOGI(TAG, "Right arrow pressed");
                        next_image();
                        break;
                    default:
                        ESP_LOGI(TAG, "Other navigation key: %d", event->args_navigation.key);
                        break;
                }
            }
            break;
            
        case INPUT_EVENT_TYPE_KEYBOARD:
            ESP_LOGI(TAG, "Keyboard event: ascii='%c' (0x%02x)", event->args_keyboard.ascii, event->args_keyboard.ascii);
            // Handle ASCII keyboard input
            if (event->args_keyboard.ascii == 'r' || event->args_keyboard.ascii == 'R') {
                ESP_LOGI(TAG, "R key pressed - random image");
                random_image();
            } else if (event->args_keyboard.ascii == 'f' || event->args_keyboard.ascii == 'F') {
                ESP_LOGI(TAG, "F key pressed - flip image");
                flip_image();
            }
            break;
            
        case INPUT_EVENT_TYPE_SCANCODE:
            ESP_LOGI(TAG, "Scancode event: scancode=0x%04x", event->args_scancode.scancode);
            // Handle scancode input for R and F keys
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

    // Scan for PNG files on SD card if available
    if (sd_card_available) {
        ESP_LOGI(TAG, "SD card is available, scanning for PNG files...");
        int found_files = scan_png_files();
        ESP_LOGI(TAG, "Found %d PNG files", found_files);
    } else {
        ESP_LOGW(TAG, "SD card not available, skipping PNG file scanning");
    }

    lvgl_lock();

    lv_obj_t* screen = lv_screen_active();
    
    // Set a purple background color to verify screen is working
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x8B00FF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    
    if (png_count > 0) {
        ESP_LOGI(TAG, "PNG files found, attempting to load first image...");
        lvgl_unlock(); // Unlock before calling load_image which has its own lock
        
        // Load the first image
        current_image_index = 0;
        load_image(current_image_index);
        ESP_LOGI(TAG, "Attempted to load first image: %d/%d", current_image_index + 1, png_count);
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
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        
        lvgl_unlock();
    }

    ESP_LOGI(TAG, "Starting main event loop");
    ESP_LOGI(TAG, "Controls: Left/Right arrows = navigate, R = random image, F = flip image");

    // Main event loop
    bsp_input_event_t input_event;
    while (true) {
        // Check for input events with a timeout
        if (xQueueReceive(input_event_queue, &input_event, pdMS_TO_TICKS(100)) == pdTRUE) {
            handle_input_event(&input_event);
        }
        
        // Small delay to prevent excessive CPU usage
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
