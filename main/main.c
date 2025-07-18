#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include "sdkconfig.h"

// Define the LED pin for ESP32-P4-Nano
#define LED_PIN 15

// CAN Interface pins for ESP32-P4-Nano
// CAN Bridge: CAN1 <-> CAN3 (man-in-the-middle)
#define CAN1_TX_PIN 16
#define CAN1_RX_PIN 17

// CAN Interface 2 (separate logging interface)
#define CAN2_TX_PIN 18
#define CAN2_RX_PIN 19

// CAN Interface 3 (bridge partner with CAN1)
#define CAN3_TX_PIN 20
#define CAN3_RX_PIN 21

// SD Card pins for ESP32-P4-Nano (SDIO interface)
#define SD_CMD_PIN  44
#define SD_CLK_PIN  43
#define SD_D0_PIN   39
#define SD_D1_PIN   40
#define SD_D2_PIN   41
#define SD_D3_PIN   42

// CAN Configuration
static const twai_timing_config_t CAN_BITRATE = TWAI_TIMING_CONFIG_500KBITS();
static const twai_filter_config_t CAN_FILTER = TWAI_FILTER_CONFIG_ACCEPT_ALL();

// Task priorities - Bridge tasks have highest priority for minimal delay
#define BRIDGE_TASK_PRIORITY 4
#define CAN_TASK_PRIORITY 3
#define SD_WRITE_TASK_PRIORITY 2
#define LED_TASK_PRIORITY 1

// Tags for ESP log messages
static const char* TAG = "CAN_BRIDGE";
static const char* LED_TAG = "LED_STATUS";
static const char* SD_TAG = "SD_CARD";
static const char* BRIDGE_TAG = "CAN_BRIDGE";

// CAN message counters
static uint32_t can1_msg_count = 0;
static uint32_t can2_msg_count = 0;
static uint32_t can3_msg_count = 0;
static uint32_t bridge_can1_to_can3_count = 0;
static uint32_t bridge_can3_to_can1_count = 0;

// Mutex for shared resources
static SemaphoreHandle_t print_mutex;
static SemaphoreHandle_t sd_mutex;

// SD Card variables
static FILE* log_file = NULL;
static char log_filename[64];
static bool sd_card_ready = false;

// CAN message queue for SD card logging (increased for high-speed operation)
#define CAN_QUEUE_SIZE 500
typedef struct {
    uint8_t interface;  // 1=CAN1, 2=CAN2, 3=CAN3, 11=CAN1->CAN3 bridge, 13=CAN3->CAN1 bridge
    twai_message_t message;
    uint64_t timestamp;
} can_log_entry_t;

static QueueHandle_t can_log_queue;

// Function to get timestamp in microseconds
static uint64_t get_timestamp_us(void) {
    return esp_timer_get_time();
}

// Function to initialize SD card
static esp_err_t init_sd_card(void) {
    esp_err_t ret;
    
    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    // Configure SD card host
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    
    // Configure SD card slot
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_CLK_PIN;
    slot_config.cmd = SD_CMD_PIN;
    slot_config.d0 = SD_D0_PIN;
    slot_config.d1 = SD_D1_PIN;
    slot_config.d2 = SD_D2_PIN;
    slot_config.d3 = SD_D3_PIN;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    const char mount_point[] = "/sdcard";
    ESP_LOGI(SD_TAG, "Initializing SD card");
    
    sdmmc_card_t* card;
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(SD_TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(SD_TAG, "Failed to initialize SD card (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    
    // Print card info
    ESP_LOGI(SD_TAG, "SD card mounted successfully");
    ESP_LOGI(SD_TAG, "Card size: %llu MB", ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    
    return ESP_OK;
}

// Function to create log file with timestamp
static esp_err_t create_log_file(void) {
    // Generate filename with timestamp
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    snprintf(log_filename, sizeof(log_filename), "/sdcard/can_bridge_%04d%02d%02d_%02d%02d%02d.csv",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    log_file = fopen(log_filename, "w");
    if (log_file == NULL) {
        ESP_LOGE(SD_TAG, "Failed to create log file: %s", log_filename);
        return ESP_FAIL;
    }
    
    // Write SavvyCAN compatible CSV header with interface identification
    fprintf(log_file, "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8,Interface\n");
    fflush(log_file);
    
    ESP_LOGI(SD_TAG, "Created log file: %s", log_filename);
    return ESP_OK;
}

// Function to write CAN message to SD card in SavvyCAN format
static void write_can_to_sd(can_log_entry_t* entry) {
    if (!sd_card_ready || log_file == NULL) {
        return;
    }
    
    if (xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Convert timestamp to seconds with microsecond precision
        double timestamp_sec = entry->timestamp / 1000000.0;
        
        // Determine interface description
        const char* interface_desc;
        switch(entry->interface) {
            case 1: interface_desc = "CAN1"; break;
            case 2: interface_desc = "CAN2"; break;
            case 3: interface_desc = "CAN3"; break;
            case 11: interface_desc = "CAN1->CAN3"; break;
            case 13: interface_desc = "CAN3->CAN1"; break;
            default: interface_desc = "UNKNOWN"; break;
        }
        
        // Write in SavvyCAN format with interface identification
        fprintf(log_file, "%.6f,%08" PRIx32 ",%s,Rx,%d,%d",
                timestamp_sec,
                entry->message.identifier,
                entry->message.extd ? "true" : "false",
                entry->interface,
                entry->message.data_length_code);
        
        // Write data bytes (pad with 0 if less than 8 bytes)
        for (int i = 0; i < 8; i++) {
            if (i < entry->message.data_length_code) {
                fprintf(log_file, ",%02X", entry->message.data[i]);
            } else {
                fprintf(log_file, ",00");
            }
        }
        fprintf(log_file, ",%s\n", interface_desc);
        
        // Flush every 20 messages to ensure data is written
        static int flush_counter = 0;
        if (++flush_counter >= 20) {
            fflush(log_file);
            flush_counter = 0;
        }
        
        xSemaphoreGive(sd_mutex);
    }
}

// SD card logging task
static void sd_logging_task(void* pvParameters) {
    can_log_entry_t log_entry;
    
    ESP_LOGI(SD_TAG, "SD logging task started");
    
    while (1) {
        // Wait for CAN messages to log
        if (xQueueReceive(can_log_queue, &log_entry, pdMS_TO_TICKS(1000)) == pdTRUE) {
            write_can_to_sd(&log_entry);
        }
        
        // Periodically flush the file
        if (sd_card_ready && log_file != NULL) {
            if (xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                fflush(log_file);
                xSemaphoreGive(sd_mutex);
            }
        }
    }
}

// Function to format CAN ID for display
static void format_can_id(uint32_t id, bool extended, char* buffer, size_t buffer_size) {
    if (extended) {
        snprintf(buffer, buffer_size, "%08" PRIx32, id);
    } else {
        snprintf(buffer, buffer_size, "%03" PRIx32, id);
    }
}

// Function to format CAN data for display
static void format_can_data(const uint8_t* data, uint8_t length, char* buffer, size_t buffer_size) {
    buffer[0] = '\0';
    for (int i = 0; i < length && i < 8; i++) {
        char temp[4];
        snprintf(temp, sizeof(temp), "%02X ", data[i]);
        strncat(buffer, temp, buffer_size - strlen(buffer) - 1);
    }
    // Remove trailing space
    if (strlen(buffer) > 0) {
        buffer[strlen(buffer) - 1] = '\0';
    }
}

// CAN Bridge: CAN1 to CAN3 (high priority, minimal delay, optimized for >2000 msg/sec)
static void can1_to_can3_bridge_task(void* pvParameters) {
    twai_message_t message;
    can_log_entry_t log_entry;
    int consecutive_empty = 0;
    
    ESP_LOGI(BRIDGE_TAG, "CAN1->CAN3 high-speed bridge task started");
    
    while (1) {
        // Non-blocking receive for maximum speed
        if (twai_receive(&message, 0) == ESP_OK) {
            uint64_t timestamp = get_timestamp_us();
            consecutive_empty = 0;
            
            // Immediately forward to CAN3 with minimal timeout
            if (twai_transmit(&message, pdMS_TO_TICKS(1)) == ESP_OK) {
                bridge_can1_to_can3_count++;
                
                // Queue message for logging (non-blocking)
                log_entry.interface = 11; // CAN1->CAN3 bridge
                log_entry.message = message;
                log_entry.timestamp = timestamp;
                xQueueSend(can_log_queue, &log_entry, 0);
            }
        } else {
            // Only yield CPU after consecutive empty receives
            consecutive_empty++;
            if (consecutive_empty > 10) {
                vTaskDelay(pdMS_TO_TICKS(1));
                consecutive_empty = 0;
            }
        }
    }
}

// CAN Bridge: CAN3 to CAN1 (high priority, minimal delay, optimized for >2000 msg/sec)
static void can3_to_can1_bridge_task(void* pvParameters) {
    twai_message_t message;
    can_log_entry_t log_entry;
    int consecutive_empty = 0;
    
    ESP_LOGI(BRIDGE_TAG, "CAN3->CAN1 high-speed bridge task started");
    
    while (1) {
        // Non-blocking receive for maximum speed
        if (twai_receive(&message, 0) == ESP_OK) {
            uint64_t timestamp = get_timestamp_us();
            consecutive_empty = 0;
            
            // Immediately forward to CAN1 with minimal timeout
            if (twai_transmit(&message, pdMS_TO_TICKS(1)) == ESP_OK) {
                bridge_can3_to_can1_count++;
                
                // Queue message for logging (non-blocking)
                log_entry.interface = 13; // CAN3->CAN1 bridge
                log_entry.message = message;
                log_entry.timestamp = timestamp;
                xQueueSend(can_log_queue, &log_entry, 0);
            }
        } else {
            // Only yield CPU after consecutive empty receives
            consecutive_empty++;
            if (consecutive_empty > 10) {
                vTaskDelay(pdMS_TO_TICKS(1));
                consecutive_empty = 0;
            }
        }
    }
}

// CAN Interface 2 logging task (separate from bridge)
static void can2_logging_task(void* pvParameters) {
    twai_message_t message;
    char id_str[16];
    char data_str[32];
    can_log_entry_t log_entry;
    
    ESP_LOGI(TAG, "CAN2 logging task started");
    
    while (1) {
        // Wait for message to be received
        if (twai_receive(&message, pdMS_TO_TICKS(100)) == ESP_OK) {
            can2_msg_count++;
            uint64_t timestamp = get_timestamp_us();
            
            // Format CAN ID
            format_can_id(message.identifier, message.extd, id_str, sizeof(id_str));
            
            // Format CAN data
            format_can_data(message.data, message.data_length_code, data_str, sizeof(data_str));
            
            // Thread-safe logging to console
            if (xSemaphoreTake(print_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                ESP_LOGI(TAG, "CAN2 [%s] %s%s DLC:%d DATA:[%s] COUNT:%" PRIu32,
                         id_str,
                         message.extd ? "EXT" : "STD",
                         message.rtr ? " RTR" : "",
                         message.data_length_code,
                         data_str,
                         can2_msg_count);
                xSemaphoreGive(print_mutex);
            }
            
            // Queue message for SD card logging
            log_entry.interface = 2;
            log_entry.message = message;
            log_entry.timestamp = timestamp;
            xQueueSend(can_log_queue, &log_entry, 0);
        }
        
        // Small delay to prevent watchdog issues
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// LED status task
static void led_status_task(void* pvParameters) {
    bool led_state = false;
    
    ESP_LOGI(LED_TAG, "LED status task started");
    
    while (1) {
        // Toggle LED
        led_state = !led_state;
        gpio_set_level(LED_PIN, led_state);
        
        // Thread-safe status logging every 10 seconds
        static int counter = 0;
        if (counter++ >= 10) {
            counter = 0;
            if (xSemaphoreTake(print_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(LED_TAG, "Status: LED=%s, CAN2_MSG=%" PRIu32 ", BRIDGE_1->3=%" PRIu32 ", BRIDGE_3->1=%" PRIu32 ", SD=%s",
                         led_state ? "ON" : "OFF", can2_msg_count, bridge_can1_to_can3_count, bridge_can3_to_can1_count,
                         sd_card_ready ? "OK" : "FAIL");
                xSemaphoreGive(print_mutex);
            }
        }
        
        // Blink every 1 second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Initialize CAN interface
static esp_err_t init_can_interface(const twai_general_config_t* g_config) {
    esp_err_t ret;
    
    // Install TWAI driver
    ret = twai_driver_install(g_config, &CAN_BITRATE, &CAN_FILTER);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TWAI driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Start TWAI driver
    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TWAI driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t ret;
    
    // Initialize logging
    ESP_LOGI(TAG, "ESP32-P4-Nano CAN Bridge Logger Starting...");
    ESP_LOGI(TAG, "LED status pin: GPIO%d", LED_PIN);
    ESP_LOGI(TAG, "CAN Bridge: CAN1 (GPIO%d/%d) <-> CAN3 (GPIO%d/%d)", CAN1_TX_PIN, CAN1_RX_PIN, CAN3_TX_PIN, CAN3_RX_PIN);
    ESP_LOGI(TAG, "CAN2 Logging: TX=GPIO%d, RX=GPIO%d", CAN2_TX_PIN, CAN2_RX_PIN);
    ESP_LOGI(TAG, "SD Card - CMD: GPIO%d, CLK: GPIO%d, D0-D3: GPIO%d-%d", 
             SD_CMD_PIN, SD_CLK_PIN, SD_D0_PIN, SD_D3_PIN);
    
    // Create mutexes for thread-safe operations
    print_mutex = xSemaphoreCreateMutex();
    sd_mutex = xSemaphoreCreateMutex();
    if (print_mutex == NULL || sd_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return;
    }
    
    // Create CAN message queue
    can_log_queue = xQueueCreate(CAN_QUEUE_SIZE, sizeof(can_log_entry_t));
    if (can_log_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create CAN log queue");
        return;
    }
    
    // Configure LED GPIO pin as output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    // Initialize SD card
    ESP_LOGI(SD_TAG, "Initializing SD card...");
    if (init_sd_card() == ESP_OK) {
        if (create_log_file() == ESP_OK) {
            sd_card_ready = true;
            ESP_LOGI(SD_TAG, "SD card ready for logging");
        } else {
            ESP_LOGE(SD_TAG, "Failed to create log file");
        }
    } else {
        ESP_LOGE(SD_TAG, "SD card initialization failed");
    }
    
    // Configure CAN1 interface (bridge side A) - optimized for high-speed
    twai_general_config_t can1_config = {
        .mode = TWAI_MODE_NORMAL,
        .tx_io = CAN1_TX_PIN,
        .rx_io = CAN1_RX_PIN,
        .clkout_io = TWAI_IO_UNUSED,
        .bus_off_io = TWAI_IO_UNUSED,
        .tx_queue_len = 128,
        .rx_queue_len = 128,
        .alerts_enabled = TWAI_ALERT_NONE,
        .clkout_divider = 0
    };
    
    // Configure CAN2 interface (separate logging)
    twai_general_config_t can2_config = {
        .mode = TWAI_MODE_NORMAL,
        .tx_io = CAN2_TX_PIN,
        .rx_io = CAN2_RX_PIN,
        .clkout_io = TWAI_IO_UNUSED,
        .bus_off_io = TWAI_IO_UNUSED,
        .tx_queue_len = 32,
        .rx_queue_len = 32,
        .alerts_enabled = TWAI_ALERT_NONE,
        .clkout_divider = 0
    };
    
    // Configure CAN3 interface (bridge side B) - optimized for high-speed
    twai_general_config_t can3_config = {
        .mode = TWAI_MODE_NORMAL,
        .tx_io = CAN3_TX_PIN,
        .rx_io = CAN3_RX_PIN,
        .clkout_io = TWAI_IO_UNUSED,
        .bus_off_io = TWAI_IO_UNUSED,
        .tx_queue_len = 128,
        .rx_queue_len = 128,
        .alerts_enabled = TWAI_ALERT_NONE,
        .clkout_divider = 0
    };
    
    // Initialize CAN1 (bridge side A)
    ESP_LOGI(TAG, "Initializing CAN1 bridge interface...");
    ret = init_can_interface(&can1_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CAN1 initialization failed");
        return;
    }
    ESP_LOGI(TAG, "CAN1 bridge interface initialized successfully");
    
    // Initialize CAN2 (separate logging)
    ESP_LOGI(TAG, "Initializing CAN2 logging interface...");
    ret = init_can_interface(&can2_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CAN2 initialization failed");
        return;
    }
    ESP_LOGI(TAG, "CAN2 logging interface initialized successfully");
    
    // Initialize CAN3 (bridge side B)
    ESP_LOGI(TAG, "Initializing CAN3 bridge interface...");
    ret = init_can_interface(&can3_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CAN3 initialization failed");
        return;
    }
    ESP_LOGI(TAG, "CAN3 bridge interface initialized successfully");
    
    // Create high-priority bridge tasks for minimal delay
    xTaskCreate(can1_to_can3_bridge_task, "CAN1->CAN3", 4096, NULL, BRIDGE_TASK_PRIORITY, NULL);
    xTaskCreate(can3_to_can1_bridge_task, "CAN3->CAN1", 4096, NULL, BRIDGE_TASK_PRIORITY, NULL);
    
    // Create CAN2 logging task
    xTaskCreate(can2_logging_task, "CAN2_LOG", 4096, NULL, CAN_TASK_PRIORITY, NULL);
    
    // Create SD logging task
    xTaskCreate(sd_logging_task, "SD_LOG", 4096, NULL, SD_WRITE_TASK_PRIORITY, NULL);
    
    // Create LED status task
    xTaskCreate(led_status_task, "LED_STATUS", 2048, NULL, LED_TASK_PRIORITY, NULL);
    
    ESP_LOGI(TAG, "All tasks created successfully");
    ESP_LOGI(TAG, "ESP32-P4-Nano CAN Bridge Logger is ready");
    ESP_LOGI(TAG, "CAN Bridge: CAN1 <-> CAN3 (seamless passthrough)");
    ESP_LOGI(TAG, "CAN2: Separate logging interface");
    ESP_LOGI(TAG, "Logging to SD card: %s", sd_card_ready ? log_filename : "SD CARD ERROR");
    
    // Main task can delete itself as other tasks are running
    vTaskDelete(NULL);
} 