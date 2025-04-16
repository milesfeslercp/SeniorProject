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
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "lwip/dns.h"
#include "esp_sntp.h"
#include <sys/time.h>
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "cert.h"
// Wi-Fi Configuration
#define WIFI_SSID "TheresASharkInTheWaterV2"
#define WIFI_PASSWORD "BOOBSboobsBOOBS"

// I2C Configuration for INA260
#define I2C_MASTER_SCL_IO 22        /*!< GPIO number for I2C master clock */
#define I2C_MASTER_SDA_IO 21        /*!< GPIO number for I2C master data  */
#define I2C_MASTER_NUM I2C_NUM_0    /*!< I2C master i2c port number */
#define I2C_MASTER_FREQ_HZ 100000   /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */

// INA260 Registers
#define INA260_REG_CONFIG 0x00
#define INA260_REG_CURRENT 0x01
#define INA260_REG_BUSVOLTAGE 0x02
#define INA260_REG_POWER 0x03
#define INA260_REG_MASK_ENABLE 0x06
#define INA260_REG_ALERT_LIMIT 0x07
#define INA260_REG_MANUFACTURER_ID 0xFE
#define INA260_REG_DIE_ID 0xFF

// INA260 Address
#define INA260_ADDR 0x40

// Flow Sensor Configuration
#define FLOW_SENSOR_GPIO GPIO_NUM_4
#define CALIBRATION_FACTOR 450

// Supabase Config
#define SUPABASE_URL    "https://lifjfhshfaekvaadiyvr.supabase.co/rest/v1/table_data_1"
#define SUPABASE_KEY    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImxpZmpmaHNoZmFla3ZhYWRpeXZyIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDQ1NzkzMDUsImV4cCI6MjA2MDE1NTMwNX0.yiZyY73CHndB8DdLUrQrvHu9oyF-x-__gQBX7ktP5E0"

// Logging Tags
static const char *TAG = "Main";

// Global Variables
volatile uint32_t pulse_count = 0;
QueueHandle_t current_queue;
QueueHandle_t flow_queue;
volatile uint32_t last_pulse_count = 0;
static SemaphoreHandle_t wifi_connected_semaphore;

// Wi-Fi Event Handler
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
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
static void IRAM_ATTR pulse_counter(void *arg)
{
    pulse_count++;
}

// I2C Master Initialization
static esp_err_t i2c_master_init()
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK)
    {
        return ret;
    }

    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

// Write to INA260 register
static esp_err_t ina260_write_register(uint8_t reg, uint16_t value)
{
    uint8_t data[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_write_to_device(I2C_MASTER_NUM, INA260_ADDR, data, sizeof(data), pdMS_TO_TICKS(1000));
}

// Read from INA260 register
static esp_err_t ina260_read_register(uint8_t reg, uint16_t *value)
{
    uint8_t reg_addr = reg;
    uint8_t data[2] = {0};

    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, INA260_ADDR, &reg_addr, 1, data, 2, pdMS_TO_TICKS(1000));
    if (ret != ESP_OK)
    {
        return ret;
    }

    *value = (data[0] << 8) | data[1];
    return ESP_OK;
}

void wifi_init()
{
    wifi_connected_semaphore = xSemaphoreCreateBinary();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_wifi_set_mode(WIFI_MODE_STA);

    // Disable power save mode to prevent DHCP failures
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Increase Wi-Fi transmit power (max: 84 = 20dBm)
    esp_wifi_set_max_tx_power(84);

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
    if (xSemaphoreTake(wifi_connected_semaphore, pdMS_TO_TICKS(15000)))
    {
        ESP_LOGI(TAG, "Wi-Fi connected with IP");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to get IP address");
        vTaskDelay(portMAX_DELAY);
    }
}

void initialize_sntp()
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}
float read_ina260_current()
{
    uint8_t reg = INA260_REG_CURRENT;
    uint8_t data[2] = {0};

    // Write register address
    i2c_master_write_read_device(I2C_MASTER_NUM, INA260_ADDR,
                                 &reg, 1, data, 2, pdMS_TO_TICKS(1000));

    // Convert to current (1.25mA per LSB)
    return ((data[0] << 8) | data[1]) * 1.25f;
}
void init_i2c()
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}
// Current Sensor Task
void current_sensor_task(void *pvParameters)
{
    init_i2c();

    while (1)
    {
        float current = read_ina260_current();
        printf("Current: %.2f mA\n", current);

        if (xQueueSend(current_queue, &current, pdMS_TO_TICKS(1000)))
        {
            ESP_LOGI(TAG, "Sent current to queue: %.2f", current);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to send current to queue");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Flow Sensor Task
void flow_sensor_task(void *pvParameters)
{
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

    while (1)
    {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t time_elapsed_ms = current_time - last_time;

        if (time_elapsed_ms >= 1000)
        {
            uint32_t pulses = pulse_count - last_pulse_count;
            last_pulse_count = pulse_count;
            float flow_rate = (pulses / (float)CALIBRATION_FACTOR) / (time_elapsed_ms / 1000.0 / 60.0);
            ESP_LOGI(TAG, "Flow Rate: %.2f", flow_rate);

            // ACTUALLY SEND TO QUEUE
            if (xQueueSend(flow_queue, &flow_rate, pdMS_TO_TICKS(1000)))
            {
                ESP_LOGI(TAG, "Sent flow rate to queue: %.2f", flow_rate);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to send flow rate to queue");
            }

            last_time = current_time;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
const char GTS_ROOT_R4_CERT[] = 
"-----BEGIN CERTIFICATE-----\n"
"MIICnzCCAiWgAwIBAgIQf/MZd5csIkp2FV0TttaF4zAKBggqhkjOPQQDAzBHMQsw\n"
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
"MBIGA1UEAxMLR1RTIFJvb3QgUjQwHhcNMjMxMjEzMDkwMDAwWhcNMjkwMjIwMTQw\n"
"MDAwWjA7MQswCQYDVQQGEwJVUzEeMBwGA1UEChMVR29vZ2xlIFRydXN0IFNlcnZp\n"
"Y2VzMQwwCgYDVQQDEwNXRTEwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAARvzTr+\n"
"Z1dHTCEDhUDCR127WEcPQMFcF4XGGTfn1XzthkubgdnXGhOlCgP4mMTG6J7/EFmP\n"
"LCaY9eYmJbsPAvpWo4H+MIH7MA4GA1UdDwEB/wQEAwIBhjAdBgNVHSUEFjAUBggr\n"
"BgEFBQcDAQYIKwYBBQUHAwIwEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHQ4EFgQU\n"
"kHeSNWfE/6jMqeZ72YB5e8yT+TgwHwYDVR0jBBgwFoAUgEzW63T/STaj1dj8tT7F\n"
"avCUHYwwNAYIKwYBBQUHAQEEKDAmMCQGCCsGAQUFBzAChhhodHRwOi8vaS5wa2ku\n"
"Z29vZy9yNC5jcnQwKwYDVR0fBCQwIjAgoB6gHIYaaHR0cDovL2MucGtpLmdvb2cv\n"
"ci9yNC5jcmwwEwYDVR0gBAwwCjAIBgZngQwBAgEwCgYIKoZIzj0EAwMDaAAwZQIx\n"
"AOcCq1HW90OVznX+0RGU1cxAQXomvtgM8zItPZCuFQ8jSBJSjz5keROv9aYsAm5V\n"
"sQIwJonMaAFi54mrfhfoFNZEfuNMSQ6/bIBiNLiyoX46FohQvKeIoJ99cx7sUkFN\n"
"7uJW\n"
"-----END CERTIFICATE-----\n";





// supabase Upload Task
void supabase_task(void *pvParameters)
{
    initialize_sntp(); // Synchronize the ESP32's clock
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        ESP_LOGI(TAG, "Waiting for time sync...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }


    // Initialize NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    // Read the current document counter
    uint32_t doc_counter = 0;
    err = nvs_get_u32(nvs_handle, "doc_counter", &doc_counter);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error reading NVS: %s", esp_err_to_name(err));
    }

    while (1)
    {
        float current;
        float flow_rate;
        if (xQueueReceive(current_queue, &current, pdMS_TO_TICKS(10000)))
        {
            ESP_LOGI(TAG, "Got current: %.2f, waiting for flow...", current);

            if (xQueueReceive(flow_queue, &flow_rate, pdMS_TO_TICKS(1000)))
            {
                ESP_LOGI(TAG, "Got both values - proceeding with upload");
                char post_data[256];
                struct timeval tv;
                gettimeofday(&tv, NULL);

                // Get seconds and convert to human-readable format
                time_t raw_time = tv.tv_sec;
                struct tm *timeinfo = gmtime(&raw_time); // Use gmtime for UTC

                char buffer[80];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);

                // Store timestamp with milliseconds
                char timestamp[100];
                snprintf(timestamp, sizeof(timestamp), "%s.%03ld", buffer, tv.tv_usec / 1000 % 1000);

                // Increment the document counter
                doc_counter++;
                err = nvs_set_u32(nvs_handle, "doc_counter", doc_counter);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "Error writing to NVS: %s", esp_err_to_name(err));
                }
                nvs_commit(nvs_handle);

                // Format post_data JSON string with sequential document number
                snprintf(post_data, sizeof(post_data),
                "{\"current_mA\": %.2f, \"flow_rate\": %.2f, \"time\": \"%s\", \"doc_number\": %lu}",
                current, flow_rate, timestamp, doc_counter);

                esp_http_client_config_t config = {
                    .url = SUPABASE_URL,
                    .method = HTTP_METHOD_POST,
                    .cert_pem = GTS_ROOT_R4_CERT,
                };
                esp_http_client_handle_t client = esp_http_client_init(&config);
                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_header(client, "apikey", SUPABASE_KEY);
                esp_http_client_set_header(client, "Authorization", "Bearer " SUPABASE_KEY);
                esp_http_client_set_post_field(client, post_data, strlen(post_data));
                esp_err_t err = esp_http_client_perform(client);
                if (err == ESP_OK)
                {
                    ESP_LOGI(TAG, "Data uploaded to Supabase, Document Number: %lu", doc_counter);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to upload data to Supabase: %s", esp_err_to_name(err));
                }
                esp_http_client_cleanup(client);
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            else
            {
                ESP_LOGE(TAG, "Timeout waiting for flow data");
            }
        }
        else
        {
            ESP_LOGE(TAG, "Timeout waiting for current data");
        }
    }

    // Close NVS handle
    nvs_close(nvs_handle);
    vTaskDelete(NULL);
}

// Main Function
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_VERBOSE);
    esp_log_level_set("*", ESP_LOG_DEBUG);  // Set the log level to debug

    wifi_init();

    current_queue = xQueueCreate(10, sizeof(float));
    flow_queue = xQueueCreate(10, sizeof(float));

    xTaskCreate(current_sensor_task, "Current Sensor Task", 4096, NULL, 5, NULL);
    xTaskCreate(flow_sensor_task, "Flow Sensor Task", 4096, NULL, 5, NULL);
    xTaskCreate(supabase_task, "supabase Task", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Tasks created. Scheduler started.");
}