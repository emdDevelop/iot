/* Iot */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include <netdb.h>
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include <esp_http_server.h>
#include "config.h"
#include "softAp.h"

void http_rest_with_url(void);
static void disconnect_server();
static void connect_server();
static bool check_if_credentials_set_by_user(void);

struct Wifi_credentials {
    char ssid[50];
    char password[50];
} wifi_crds;

const int WIFI_CONNECTED_EVENT = BIT0;
const int WIFI_SSID_SET = BIT0;
const int WIFI_PASSWORD_SET = BIT1;
const int WIFI_CREDENTIALS_SET = WIFI_SSID_SET | WIFI_PASSWORD_SET;

static EventGroupHandle_t wifi_event_group;
static EventGroupHandle_t wifi_event_credentials;

static const char *TAG = "Reed_sensor";
static int s_retry_num = 0;

esp_timer_handle_t periodic_timer;
static httpd_handle_t server = NULL;

static void periodic_timer_callback(void* arg)
{
    char str[256];
    int64_t time_since_boot = esp_timer_get_time();
    sprintf(str, "%lld", time_since_boot);
    itoa(time_since_boot,str,10);
    ESP_LOGI(TAG, "Periodic timer called, time since boot: %s us", str);
    nvs_handle_t storage_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &storage_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } {
        ESP_ERROR_CHECK(nvs_set_i32(storage_handle, "restart_counter", 0));
        ESP_ERROR_CHECK(nvs_commit(storage_handle));
        nvs_close(storage_handle);
    }
    esp_wifi_stop();
    esp_deep_sleep(0);
}

const esp_timer_create_args_t periodic_timer_args = {
        .callback = &periodic_timer_callback,
        /* name is optional, but may help identify the timer when debugging */
        .name = "periodic"
};

esp_err_t client_event_get_handler(esp_http_client_event_handle_t evt)
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            printf("HTTP_EVENT_ON_DATA: %.*s\n", evt->data_len, (char *)evt->data);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}

void http_rest_with_url(void)
{
    esp_http_client_config_t config_get = {
        .url = "https://maker.ifttt.com/trigger/main_door_open/with/key/dQhH-wb1XBAjxVi_jgAYSS",
        .cert_pem = NULL,
        .event_handler = client_event_get_handler
        };
        
    esp_http_client_handle_t client = esp_http_client_init(&config_get);
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } 
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        connect_server();
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        disconnect_server();
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",MAC2STR(event->mac), event->aid);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    } 
}

void wifi_init_sta()
{
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (!check_if_credentials_set_by_user()) {
        wifi_config_t wifi_config_set = {0};
        strncpy((char *)wifi_config_set.sta.ssid, wifi_crds.ssid, 32);
        strncpy((char *)wifi_config_set.sta.password, wifi_crds.password, 64);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config_set));
    } 

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

/* An HTTP GET handler */
esp_err_t reed_sensor_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;

    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        /* Copy null terminated value string into buffer */
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-2") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-2", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-2: %s", buf);
        }
        free(buf);
    }

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char param[32];
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "ssid", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => ssid=%s", param);
                strcpy(wifi_crds.ssid, param);
                xEventGroupSetBits(wifi_event_credentials, WIFI_SSID_SET);
            }
            if (httpd_query_key_value(buf, "password", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => password=%s", param);
                strcpy(wifi_crds.password, param);
                xEventGroupSetBits(wifi_event_credentials, WIFI_PASSWORD_SET);

            }
        }
        free(buf);
    }

    /* Set some custom headers */
    httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
    httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    const char* resp_str = (const char*) req->user_ctx;
    httpd_resp_send(req, resp_str, strlen(resp_str));

    /* After sending the HTTP response the old HTTP request
     * headers are lost. Check if HTTP request headers can be read now. */
    if (httpd_req_get_hdr_value_len(req, "Host") == 0) {
        ESP_LOGI(TAG, "Request headers lost");
    }
    return ESP_OK;
}

// URI handler structure
httpd_uri_t config_wifi = {
    .uri       = "/config_wifi",
    .method    = HTTP_GET,
    .handler   = reed_sensor_get_handler,
    .user_ctx  = "<!DOCTYPE html><html><body><h2>Sensor set wifi credentials</h2><form action=""><label >SSID:</label><br><input type='text' name='ssid' id='ssid'><br><label>wifi password:</label><br><input type='password' id='password' name='password'><br><br><input type='submit' value='Submit'></form></body></html>"
};

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SERVER_PORT;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &config_wifi);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}


static void disconnect_server()
{
    if (server) {
        ESP_LOGI(TAG, "Stopping webserver");
        httpd_stop(server);
        server = NULL;
    }
}

static void connect_server()
{
    if (server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        server = start_webserver();
    }
}

static bool check_if_credentials_set_by_user(void) {
    wifi_config_t wifi_config_get;
    nvs_handle_t storage_handle;
    int32_t restart_counter = 0;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &storage_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } {
         // value will default to 0, if not set yet in NVS
        ESP_ERROR_CHECK(nvs_get_i32(storage_handle, "restart_counter", &restart_counter));
        ESP_LOGI(TAG, "Counter is= %d", restart_counter);
        nvs_close(storage_handle);
    }
    esp_err_t ret = esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config_get);
        if (ret == ESP_OK && (strlen((const char *)&wifi_config_get.sta.ssid) != 0 || strlen((const char *)&wifi_config_get.sta.password) != 0) && restart_counter <= 3) {
        return true;
    } else {
        return false;
    }
}

void app_main(void)
{
    //The Wifi library uses NVS flash to store access point name and password
    /* Initialize NVS partition */
    esp_err_t ret_nvs = nvs_flash_init();
    if (ret_nvs == ESP_ERR_NVS_NO_FREE_PAGES || ret_nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS partition was truncated and needs to be erased");
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());
        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    nvs_handle_t storage_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &storage_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } {
        int32_t restart_counter = 0; // value will default to 0, if not set yet in NVS
        err = nvs_get_i32(storage_handle, "restart_counter", &restart_counter);
        switch (err) {
            case ESP_OK:
                ESP_LOGI(TAG, "The value is initialized");
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                ESP_LOGI(TAG, "The value is not initialized yet!");
                break;
            default :
                ESP_LOGI(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        restart_counter++;
        ESP_ERROR_CHECK(nvs_set_i32(storage_handle, "restart_counter", restart_counter));
        ESP_ERROR_CHECK(nvs_commit(storage_handle));
        nvs_close(storage_handle);
    }


    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_EVENT_ANY_BASE, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    tcpip_adapter_init();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_event_group = xEventGroupCreate();
    wifi_event_credentials = xEventGroupCreate();
    //Was the event group created successfully?
    if ( wifi_event_group == NULL ) {
        ESP_LOGI(TAG, "The event group was not created because there was insufficient FreeRTOS heap available");
    } 

    // initialize wifi station mode
    esp_deep_sleep_set_rf_option(2);

    if (check_if_credentials_set_by_user()) {
        ESP_LOGI(TAG, "Wifi configuration already stored in flash partition called NVS");
        wifi_init_sta();
    } else {
        ESP_LOGI(TAG, "Wifi credentials not set by user");
        wifi_init_softap();
        xEventGroupWaitBits(wifi_event_credentials, WIFI_CREDENTIALS_SET, pdTRUE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "Wifi credentials are set...");
        // start wifi station mode with the credentials and stop wifi ap mode
        ESP_ERROR_CHECK(esp_wifi_stop());
        wifi_init_sta();
    }
    
    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Wifi station mode initiated...");
    // do http request to ifft
    http_rest_with_url();
    //enter sleep mode after 10 sec
    ESP_ERROR_CHECK(esp_timer_start_once(periodic_timer, 10000000));


}