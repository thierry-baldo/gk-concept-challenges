/* challenge6: Every third correct messages, light a (big)LED(owski) for 3 seconds
 *
 * inspired by "examples/protocols/mqtt/tcp" of ESP-IDF
 * inspired by "examples/protocols/sntp" of ESP-IDF
 * inspired by "examples/storage/nvs_rw_value" of ESP-IDF
 * inspired by "examples/system/esp_timer" of ESP-IDF
 *
 * to obtain current date, a connection to NTP server "pool.ntp.org" is used
 *
 * Used MQTT broker is "mqtt://public.mqtthq.com" which doesn't need an account
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "mqtt_client.h"

/* force WPA2 as requested */
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

/* doesn't need an account */
#define BROKER_URL           "mqtt://public.mqtthq.com"

#define SNTP_TIME_SERVER     "pool.ntp.org"

#define DATE_LENGTH          32
#define BUFFER_LENGTH        64
#define DATE_NAME            "dude_date"

#define MESSAGE_COUNT_MAX    3

#define LED_PIN              13
#define LED_ON_DURATION_SEC  3
#define LED_ON_DURATION_US   (LED_ON_DURATION_SEC * 1000000)

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "challenge6";

static int s_retry_num = 0;

#define ESP_WIFI_MAXIMUM_RETRY           5

/* event handler to process messages about Wi-Fi */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void get_current_date(char *buffer, unsigned int size)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    gmtime_r(&now, &timeinfo);
    strftime(buffer, size, "%c", &timeinfo);
}

static void obtain_time(void)
{
    char date[DATE_LENGTH];
    int retry = 0;
    const int retry_count = 15;

    ESP_LOGI(TAG, "obtain_time() entered");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_TIME_SERVER);
    esp_netif_sntp_init(&config);
    /* first server is logged */
    ESP_LOGI(TAG, "NTP connected to %s", esp_sntp_getservername(0));

    // wait for time to be set
    while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    }

    get_current_date(date, sizeof(date));
    ESP_LOGI(TAG, "GMT: %s", date);

    ESP_LOGI(TAG, "obtain_time() terminated");
}

static void save_current_date(void)
{
    char date[DATE_LENGTH];
    char buffer[BUFFER_LENGTH];
    char last_buffer[BUFFER_LENGTH];
    esp_err_t err;
    size_t length;

    get_current_date(date, sizeof(date));
    snprintf(buffer, sizeof(buffer), "The dude abided on %s", date);
    ESP_LOGI(TAG, "current message: \"%s\"", buffer);

    nvs_handle_t handle;
    err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        length = BUFFER_LENGTH;
        memset(last_buffer, 0, length);
        err = nvs_get_str(handle, DATE_NAME, last_buffer, &length);
        if (err == ESP_OK) {
          ESP_LOGI(TAG, "last saved message: \"%s\"", last_buffer);
        }

        err = nvs_set_str(handle, DATE_NAME, buffer);
        ESP_LOGI(TAG, "nvs_set_str()=%s", (err != ESP_OK) ? "Failed!" : "Done");
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "nv_commit()=%s", (err != ESP_OK) ? "Failed!" : "Done");
        nvs_close(handle);
    }
}

static void switch_on_led(void)
{
    ESP_LOGI(TAG, "switch_on_led()");
    gpio_set_level(LED_PIN, 1);
}

static void switch_off_led(void)
{
    ESP_LOGI(TAG, "switch_off_led()");
    gpio_set_level(LED_PIN, 0);
}

static void oneshot_timer_callback(void* arg)
{
    ESP_LOGI(TAG, "oneshot_timer_callback()");

    switch_off_led();
}

/* this function is called each time we want to switch on the led
 * it starts a 3-second timer to switch off the led
 * if this function is called twice before the led has been switched off
 * we delete the timer before re-creating it
 */
static void light_led(void)
{
    static esp_timer_handle_t timer_handle;

    ESP_LOGI(TAG, "light_led()");
    switch_on_led();

    if (timer_handle != NULL) {
        esp_timer_delete(timer_handle);
        timer_handle = NULL;
    }

    const esp_timer_create_args_t oneshot_timer_args = {
            .callback = &oneshot_timer_callback,
            .arg = NULL,
            .name = "led",
    };

    if (esp_timer_create(&oneshot_timer_args, &timer_handle) == ESP_OK) {
        esp_timer_start_once(timer_handle, LED_ON_DURATION_US);
    }
}

static void led_configure(void)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    static int message_count;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, "/bigLebowski", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATE_LENGTH=%d\r\n", event->data_len);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        if (strncmp(event->data, "who are you man ?", event->data_len) == 0) {
            msg_id = esp_mqtt_client_publish(client, "/bigLebowski", "I'm The Dude", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            message_count++;
            ESP_LOGI(TAG, "message_count=%d", message_count);
            if (message_count == MESSAGE_COUNT_MAX) {
                light_led();
                message_count = 0;
            }
        }
        save_current_date();
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "MQTT_EVENT_BEFORE_CONNECT");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BROKER_URL,
    };

    ESP_LOGI(TAG, "mqtt_app_start() entered");

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    ESP_LOGI(TAG, "mqtt_app_start() terminated");
};

/* create the Wi-Fi Station */
static void wifi_init_sta(void)
{
    ESP_LOGI(TAG, "wifi_init_sta() entered");

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* register some events handlers */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    /* connection parameters to AP */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    ESP_LOGI(TAG, "wifi_init_sta() finished");
}

void app_main(void)
{
    /* Initialize Non-Volatile Storage */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_configure();

    wifi_init_sta();

    obtain_time();

    mqtt_app_start();
}
