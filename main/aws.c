#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "esp_wifi.h"

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "portmacro.h"

#include "mqtt_client.h"

// Constants
#define WIFI_SSID "SecondFloor"
#define WIFI_PASS "manha2001"
#define NTP_RESYNC_DELAY 2 * 60 * 1000 // min * sec * ms
#define MQTT_TOPIC "parsed_data"

#define WIFI_TASK_STACK_SIZE 4096
#define WIFI_TASK_PRIORITY 5

#define SUCCESS 1 << 0
#define FAILURE 1 << 1

#define SNTP_TIME_SERVER "time.google.com"

#define MAX_ENTRIES 100

// Global 
static EventGroupHandle_t wifi_event_group;
static TimerHandle_t ntp_sync_timer = NULL;

static int s_retry_num = 0;
static const char *TAG = "aws";
static const char *broker_ip = "mqtt://192.168.100.3";

esp_err_t connect_wifi(void);

struct Entry {
        char date[11];
        char time[6];
        int duration;
};
struct Entry entries[MAX_ENTRIES];

// connect_wifi helper functions
bool is_wifi_connected(void) 
{
        wifi_ap_record_t wifi_info;
        esp_err_t result = esp_wifi_sta_get_ap_info(&wifi_info);
        return (result == ESP_OK); 
}

static void wifi_task(void* pvParameters) 
{
        const int retry_delay = 5000;
        while (true)
        {
                if (!is_wifi_connected())
                {
                        connect_wifi();
                        ESP_LOGI(TAG, "Is wifi connected: %s", is_wifi_connected() ? "True" : "False");

                        // delay for a while before checking the Wi-Fi connection status again
                        vTaskDelay(pdMS_TO_TICKS(retry_delay));
                }
        }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
        const int max_failure = 5;

	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		ESP_LOGI(TAG, "Connecting to AP...");
		esp_wifi_connect();
	} 
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		if (s_retry_num < max_failure)
		{
			ESP_LOGI(TAG, "Reconnecting to AP...");
			esp_wifi_connect();
			s_retry_num++;
		} 
                else 
                {
			xEventGroupSetBits(wifi_event_group, FAILURE);
		}
	}
}

static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
                ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                ESP_LOGI(TAG, "IP - " IPSTR, IP2STR(&event->ip_info.ip));

                s_retry_num = 0;

                xEventGroupSetBits(wifi_event_group, SUCCESS);
        }
}

/* 
 * connect_wifi initializes and manages ESP32 Wi-Fi connectivity. It connects to the specified Wi-Fi network with credentials, 
 * handles connection retries, and reports success or failure. It uses event handlers for Wi-Fi and IP events, providing status 
 * through SUCCESS or FAILURE. 
 */
esp_err_t connect_wifi(void)
{
	int status = FAILURE;

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	esp_netif_create_default_wifi_sta();

	// wifi station with the default wifi configuration
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        wifi_event_group = xEventGroupCreate();

        esp_event_handler_instance_t wifi_handler_event_instance;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &wifi_handler_event_instance));

        esp_event_handler_instance_t got_ip_event_instance;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &ip_event_handler,
                                                            NULL,
                                                            &got_ip_event_instance));

        wifi_config_t wifi_config = 
        {
                .sta = 
                {
                        .ssid = WIFI_SSID,
                        .password = WIFI_PASS,
                        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                        .pmf_cfg = { .capable = true, .required = false },
                },
        };

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "STA initialization complete");

        EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                               SUCCESS | FAILURE,
                                               pdFALSE,
                                               pdFALSE,
                                               portMAX_DELAY);

        if (bits & SUCCESS) 
        {
                ESP_LOGI(TAG, "Connected to ap");
                status = SUCCESS;
        } 
        else if (bits & FAILURE) 
        {
                ESP_LOGI(TAG, "Failed to connect to ap");
                status = FAILURE;
        }
        else 
        {
                ESP_LOGE(TAG, "UNEXPECTED EVENT");
                status = FAILURE;
        }

        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_instance));
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_event_instance));

        vEventGroupDelete(wifi_event_group);

        return status;
}

/* 
 * obtain_time fetches the current time from an NTP server. It attempts synchronization with retries, sets the time zone, 
 * and formats the time as "dd-mm-yyyy hh:mm". The result is stored in a dynamically allocated buffer, and the function 
 * returns this buffer. It also handles memory allocation failure and ensures proper NTP deinitialization.
 */
static void obtain_time(void)
{
        const int retry_count = 15;
        const int buf_size = 20;

        int retry = 0;
        time_t now = 0;
        struct tm timeinfo = { 0 };

        ESP_LOGI(TAG, "Synchronizating NTP");

        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_TIME_SERVER);

        esp_netif_sntp_init(&config);

        while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retry < retry_count) 
        {
                ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        }

        time(&now);
        localtime_r(&now, &timeinfo);

        setenv("TZ", "PKT-5", 1);
        tzset();
        localtime_r(&now, &timeinfo);

        esp_netif_sntp_deinit();
}

// function to sync ntp periodically
static void ntp_sync_timer_callback(TimerHandle_t xTimer)
{
        obtain_time();
}

// connect_mqtt helper function
static void process_data(char *data, int len)
{
        int num_entries = 0;
        const char *filter = "Something went wrong with parsing, Enter data manually";

        char tmp[len]; 
        for (int i = 0; i < len; i++) 
        {
                tmp[i] = data[i];
        }

        if (!strncmp(tmp, filter, len)) 
        {
                for (int i = 0; i < len; i++) 
                {
                        tmp[i] = '\0';
                }

                ESP_LOGI(TAG, "Clearing Data");
        }

        char *token = strtok(tmp, "|");

        while (token != NULL && num_entries < MAX_ENTRIES) 
        {
                sscanf(token, "%10[^;];%5[^;];%d", entries[num_entries].date, entries[num_entries].time, &entries[num_entries].duration);
                token = strtok(NULL, "|");
                num_entries++;
        }

        for (int i = 0; i < num_entries; i++) 
        {
                printf("Date: %s, Time: %s, Duration: %d\n", entries[i].date, entries[i].time, entries[i].duration);
        }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
        ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
        esp_mqtt_event_handle_t event = event_data;
        esp_mqtt_client_handle_t client = event->client;
        int msg_id;

        switch ((esp_mqtt_event_id_t)event_id) 
        {
                case MQTT_EVENT_CONNECTED:
                        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
                        msg_id = esp_mqtt_client_subscribe(client, "parsed_data", 0);
                        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
                        break;

                case MQTT_EVENT_DISCONNECTED:
                        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
                        break;

                case MQTT_EVENT_SUBSCRIBED:
                        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
                        break;

                case MQTT_EVENT_PUBLISHED:
                        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
                        break;

                case MQTT_EVENT_DATA:
                        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
                        if (strncmp(event->topic, MQTT_TOPIC , event->topic_len) == 0) 
                        {
                                process_data(event->data, event->data_len);
                        }

                        break;

                case MQTT_EVENT_ERROR:
                        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
                        break;

                default:
                        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
                        break;
        }
}

/*
 * connect_mqtt initializes and starts an MQTT client. It sets up the MQTT configuration with the specified broker address, 
 * initializes the client, registers an event handler for MQTT events, and finally starts the MQTT client, enabling it to connect 
 * to the configured MQTT broker and handle MQTT communication events.
 */
static void connect_mqtt(void)
{
        esp_mqtt_client_config_t mqtt_cfg = 
        {
                .broker.address.uri = broker_ip,
        };

        esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(client);
}

void app_main(void)
{
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
        {
                ESP_ERROR_CHECK(nvs_flash_erase());
                ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        
        // initialize startup
        connect_wifi();
        ESP_LOGI(TAG, "Is wifi connected: %s", is_wifi_connected() ? "True" : "False");

        obtain_time();

        ntp_sync_timer = xTimerCreate("ntp_sync_timer", pdMS_TO_TICKS(NTP_RESYNC_DELAY) , pdTRUE, NULL, ntp_sync_timer_callback);
        if (ntp_sync_timer != NULL)
        {
                xTimerStart(ntp_sync_timer, 0);
        }

        // xTaskCreate(wifi_task, "wifi_task", WIFI_TASK_STACK_SIZE, NULL, WIFI_TASK_PRIORITY, NULL);
        connect_mqtt();
}
