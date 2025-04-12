#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "lwip/dns.h"
#include "esp_sntp.h"
#include <sys/time.h>
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

// Wi-Fi Configuration
#define WIFI_SSID "TheresASharkInTheWaterV2"
#define WIFI_PASSWORD "BOOBSboobsBOOBS"

// ADC Configuration
#define ADC_CHANNEL ADC_CHANNEL_6
#define ADC_ATTEN ADC_ATTEN_DB_12
#define ADC_BIT_WIDTH ADC_BITWIDTH_12

// Flow Sensor Configuration
#define FLOW_SENSOR_GPIO GPIO_NUM_4
#define CALIBRATION_FACTOR 450

// Firestore Configuration
#define FIRESTORE_URL "https://firestore.googleapis.com/v1/projects/esp32pumpdata/databases/(default)/documents/sensor_data_test_1"
#define FIRESTORE_API_KEY "AIzaSyD9rubGXDefcRikHpPy8hnF95DKDbtdLHQ"

// Logging Tags
static const char *TAG = "Main";

// Global Variables
adc_oneshot_unit_handle_t adc_handle;
volatile uint32_t pulse_count = 0;
QueueHandle_t adc_queue;
QueueHandle_t flow_queue;
volatile uint32_t last_pulse_count = 0; // Add this line
static SemaphoreHandle_t wifi_connected_semaphore;

// Wi-Fi Event Handler
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi connecting...");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected...");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi lost connection...");
            esp_wifi_connect();
            break;
        case IP_EVENT_STA_GOT_IP:
            xSemaphoreGive(wifi_connected_semaphore); // Signal Wi-Fi connection
            break;
        default:
            break;
    }
}

// Interrupt handler to count pulses
static void IRAM_ATTR pulse_counter(void *arg) {
    pulse_count++;
}

void wifi_init() {
    wifi_connected_semaphore = xSemaphoreCreateBinary();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_wifi_set_mode(WIFI_MODE_STA);

    // Disable power save mode to prevent DHCP failures
    esp_wifi_set_ps(WIFI_PS_NONE); // <-- ADD THIS

    // Increase Wi-Fi transmit power (max: 84 = 20dBm)
    esp_wifi_set_max_tx_power(84); // <-- OPTIONAL BUT RECOMMENDED

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // Enforce WPA2
        },
    };
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    // Wait for IP acquisition (increase timeout to 15s for debugging)
    if (xSemaphoreTake(wifi_connected_semaphore, pdMS_TO_TICKS(15000))) {
        ESP_LOGI(TAG, "Wi-Fi connected with IP");
    } else {
        ESP_LOGE(TAG, "Failed to get IP address");
        vTaskDelay(portMAX_DELAY);
    }
}


void initialize_sntp() {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}
// ADC Task
void adc_task(void *pvParameters) {
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BIT_WIDTH,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL, &channel_cfg));

    while (1) {
        int raw_reading = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL, &raw_reading));
        ESP_LOGI(TAG, "ADC Reading: %d", raw_reading);
        xQueueSend(adc_queue, &raw_reading, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Flow Sensor Task
void flow_sensor_task(void *pvParameters) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FLOW_SENSOR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(FLOW_SENSOR_GPIO, pulse_counter, NULL);

    uint32_t last_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (1) {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t time_elapsed_ms = current_time - last_time;

        if (time_elapsed_ms >= 1000) {
            uint32_t pulses = pulse_count - last_pulse_count;
            last_pulse_count = pulse_count;
            float flow_rate = (pulses / (float)CALIBRATION_FACTOR) / (time_elapsed_ms / 1000.0 / 60.0);
            ESP_LOGI(TAG, "Flow Rate: %.2f", flow_rate);

            xQueueSend(flow_queue, &flow_rate, portMAX_DELAY);
            last_time = current_time;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
// Firebase Upload Task
void firebase_task(void *pvParameters) {
    initialize_sntp(); // Synchronize the ESP32's clock

    // Initialize NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    // Read the current document counter
    uint32_t doc_counter = 0;
    err = nvs_get_u32(nvs_handle, "doc_counter", &doc_counter);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Error reading NVS: %s", esp_err_to_name(err));
    }

    while (1) {
        int adc_reading;
        float flow_rate;
        if (xQueueReceive(adc_queue, &adc_reading, portMAX_DELAY) && xQueueReceive(flow_queue, &flow_rate, portMAX_DELAY)) {
            char post_data[256];
            struct timeval tv;
            gettimeofday(&tv, NULL);

            // Get seconds and convert to human-readable format
            time_t raw_time = tv.tv_sec;  // Use tv.tv_sec instead of incorrect calculation
            struct tm *timeinfo = gmtime(&raw_time); // Use gmtime for UTC

            char buffer[80];  
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo); 

            // Store timestamp with milliseconds
            char timestamp[100];  // Ensure sufficient size for formatted timestamp
            snprintf(timestamp, sizeof(timestamp), "%s.%03ld", buffer, tv.tv_usec / 1000 % 1000);

            // Increment the document counter
            doc_counter++;
            err = nvs_set_u32(nvs_handle, "doc_counter", doc_counter);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error writing to NVS: %s", esp_err_to_name(err));
            }
            nvs_commit(nvs_handle);

            // Format post_data JSON string with sequential document number
            snprintf(post_data, sizeof(post_data),
                    "{\"fields\":{\"adc_reading\":{\"integerValue\":%d},\"flow_rate\":{\"doubleValue\":%.2f},\"time\":{\"stringValue\":\"%s\"},\"doc_number\":{\"integerValue\":%lu}}}",
                    adc_reading, flow_rate, timestamp, doc_counter);

            esp_http_client_config_t config = {
                .url = FIRESTORE_URL,
                .method = HTTP_METHOD_POST,
                .skip_cert_common_name_check = true, // Skip CN (Common Name) check
                .cert_pem = NULL, // No certificate provided
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, post_data, strlen(post_data));

            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Data uploaded to Firestore, Document Number: %lu", doc_counter);
            } else {
                ESP_LOGE(TAG, "Failed to upload data to Firestore: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    // Close NVS handle
    nvs_close(nvs_handle);
    vTaskDelete(NULL);
}

// Main Function
void app_main(void) {

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_VERBOSE);
    wifi_init();

    adc_queue = xQueueCreate(10, sizeof(int));
    flow_queue = xQueueCreate(10, sizeof(float));

    xTaskCreate(adc_task, "ADC Task", 4096, NULL, 5, NULL);
    xTaskCreate(flow_sensor_task, "Flow Sensor Task", 4096, NULL, 5, NULL);
    xTaskCreate(firebase_task, "Firebase Task", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Tasks created. Scheduler started.");
}

// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "esp_adc/adc_oneshot.h"
// #include "esp_adc/adc_cali.h"
// #include "esp_adc/adc_cali_scheme.h"
// #include "driver/gpio.h"

// // ADC Configuration
// #define ADC_CHANNEL       ADC_CHANNEL_6
// #define ADC_ATTEN         ADC_ATTEN_DB_12
// #define ADC_BIT_WIDTH     ADC_BITWIDTH_12

// // Flow Sensor Configuration
// #define FLOW_SENSOR_GPIO GPIO_NUM_4  // GPIO pin connected to the flow sensor
// #define CALIBRATION_FACTOR 450       // Pulses per liter (adjust based on your sensor's datasheet)

// // Logging Tags
// static const char *TAG_ADC = "ADC Example";
// static const char *TAG_FLOW = "Flow Sensor";

// // Global Variables
// adc_oneshot_unit_handle_t adc_handle;
// adc_cali_handle_t cali_handle;
// volatile uint32_t pulse_count = 0;   // Counter for pulses
// uint32_t last_pulse_count = 0;       // Last recorded pulse count
// float flow_rate = 0;                 // Flow rate in liters per minute

// // ADC Calibration Initialization
// static bool init_adc_calibration(adc_cali_handle_t *out_handle) {
//     adc_cali_line_fitting_config_t cali_config = {
//         .unit_id = ADC_UNIT_1,
//         .atten = ADC_ATTEN,
//         .bitwidth = ADC_BIT_WIDTH,
//     };
    
//     esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, out_handle);
//     if (ret == ESP_OK) {
//         ESP_LOGI(TAG_ADC, "ADC calibration initialized.");
//         return true;
//     } else {
//         ESP_LOGW(TAG_ADC, "ADC calibration not supported on this device.");
//         return false;
//     }
// }

// // ADC Task
// void adc_task(void *pvParameters) {
//     // Initialize ADC
//     adc_oneshot_unit_init_cfg_t unit_cfg = {
//         .unit_id = ADC_UNIT_1,
//     };
//     ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

//     adc_oneshot_chan_cfg_t channel_cfg = {
//         .atten = ADC_ATTEN,
//         .bitwidth = ADC_BIT_WIDTH,
//     };
//     ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL, &channel_cfg));

//     // Initialize ADC calibration
//     bool cali_enabled = init_adc_calibration(&cali_handle);

//     ESP_LOGI(TAG_ADC, "Starting ADC reading loop...");
//     while (1) {
//         int raw_reading = 0;
//         ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL, &raw_reading));

//         if (cali_enabled) {
//             int voltage = 0;  // Voltage should be of type int
//             ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, raw_reading, &voltage));
//             printf("Raw: %d\tVoltage: %d mV\n", raw_reading, voltage);
//         } else {
//             printf("Raw: %d\n", raw_reading);
//         }

//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }

// // Interrupt handler to count pulses
// static void IRAM_ATTR pulse_counter(void *arg) {
//     pulse_count++;
// }

// // Function to calculate flow rate
// float calculate_flow_rate(uint32_t pulses, uint32_t time_elapsed_ms) {
//     // Convert time to seconds
//     float time_elapsed_s = time_elapsed_ms / 1000.0;
//     // Calculate flow rate in liters per minute
//     float flow_rate_lpm = (pulses / (float)CALIBRATION_FACTOR) / (time_elapsed_s / 60.0);
//     return flow_rate_lpm;
// }

// // Flow Sensor Task
// void flow_sensor_task(void *pvParameters) {
//     // Configure GPIO pin for the flow sensor
//     gpio_config_t io_conf = {
//         .pin_bit_mask = (1ULL << FLOW_SENSOR_GPIO),
//         .mode = GPIO_MODE_INPUT,
//         .pull_up_en = GPIO_PULLUP_ENABLE,  // Enable pull-up resistor
//         .intr_type = GPIO_INTR_POSEDGE,    // Trigger interrupt on rising edge
//     };
//     gpio_config(&io_conf);

//     // Install GPIO interrupt service
//     gpio_install_isr_service(0);
//     gpio_isr_handler_add(FLOW_SENSOR_GPIO, pulse_counter, NULL);

//     ESP_LOGI(TAG_FLOW, "Flow sensor initialized. Starting flow rate measurement...");

//     uint32_t last_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

//     while (1) {
//         // Calculate elapsed time
//         uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
//         uint32_t time_elapsed_ms = current_time - last_time;

//         if (time_elapsed_ms >= 1000) {  // Update flow rate every second
//             // Calculate the number of pulses since the last update
//             uint32_t pulses = pulse_count - last_pulse_count;
//             last_pulse_count = pulse_count;

//             // Calculate flow rate
//             flow_rate = calculate_flow_rate(pulses, time_elapsed_ms);

//             // Log the flow rate
//             ESP_LOGI(TAG_FLOW, "Flow Rate: %.2f L/min", flow_rate);

//             // Reset the timer
//             last_time = current_time;
//         }

//         vTaskDelay(pdMS_TO_TICKS(100));  // Small delay to prevent busy-waiting
//     }
// }

// // Main Function
// void app_main(void) {
//     // Create ADC Task
//     xTaskCreate(adc_task, "ADC Task", 2048, NULL, 5, NULL);

//     // Create Flow Sensor Task
//     xTaskCreate(flow_sensor_task, "Flow Sensor Task", 2048, NULL, 5, NULL);

//     ESP_LOGI("Main", "Tasks created. Scheduler started.");
// }