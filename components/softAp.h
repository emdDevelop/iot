#include <stdio.h>
#include <string.h>

#define SOFT_AP_IP_ADDRESS_1 192
#define SOFT_AP_IP_ADDRESS_2 168
#define SOFT_AP_IP_ADDRESS_3 5
#define SOFT_AP_IP_ADDRESS_4 18
 
#define SOFT_AP_GW_ADDRESS_1 192
#define SOFT_AP_GW_ADDRESS_2 168
#define SOFT_AP_GW_ADDRESS_3 5
#define SOFT_AP_GW_ADDRESS_4 20
 
#define SOFT_AP_NM_ADDRESS_1 255
#define SOFT_AP_NM_ADDRESS_2 255
#define SOFT_AP_NM_ADDRESS_3 255
#define SOFT_AP_NM_ADDRESS_4 0

#define AP_ESP_WIFI_SSID  "reedsensor"
#define AP_ESP_WIFI_PASS  "esp8266"
#define AP_MAX_STA_CONN    5


void wifi_init_softap(void);
